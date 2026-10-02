// 文件职责：实现精确 pread、并行预加载、计时及物理范围内的安全解码。
// 本文件提供底层读取原语，不负责随机请求排序、范围合并或结果批次布局。
#include "store_io.hpp"

#include "codec.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace aastore {
namespace {

constexpr uint64_t kMinPreloadBytesPerThread = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kPreloadChunkBytes = 16ULL * 1024ULL * 1024ULL;

struct ParallelPreadContext {
    int descriptor = -1;
    char* output = nullptr;
    uint64_t bytes = 0;
    uint64_t offset = 0;
    size_t worker_count = 1;
    bool discard_source_cache = false;
    std::atomic<uint64_t> next_chunk{0};
    std::vector<std::exception_ptr>* errors = nullptr;
};

// 按配置提示内核释放已复制区间的源文件页缓存；该提示不保证立即驱逐。
void discard_pread_cache(const ParallelPreadContext* ctx,
                         uint64_t begin, uint64_t size) {
#ifdef POSIX_FADV_DONTNEED
    if (ctx->discard_source_cache) {
        (void)::posix_fadvise(
            ctx->descriptor, static_cast<off_t>(ctx->offset + begin),
            static_cast<off_t>(size), POSIX_FADV_DONTNEED);
    }
#else
    (void)ctx;
    (void)begin;
    (void)size;
#endif
}

// 读取本线程的静态分区或动态预加载块，并将异常保存到对应线程槽位。
void parallel_pread_worker(ParallelPreadContext* ctx,
                           size_t worker) noexcept {
    try {
        if (ctx->discard_source_cache) {
            while (true) {
                const uint64_t begin = ctx->next_chunk.fetch_add(
                    kPreloadChunkBytes, std::memory_order_relaxed);
                if (begin >= ctx->bytes) break;
                const uint64_t chunk =
                    std::min(kPreloadChunkBytes, ctx->bytes - begin);
                pread_exact(ctx->descriptor, ctx->output + begin, chunk,
                            ctx->offset + begin);
                discard_pread_cache(ctx, begin, chunk);
            }
            return;
        }
        const uint64_t begin = ctx->bytes * worker / ctx->worker_count;
        const uint64_t end =
            ctx->bytes * (worker + 1) / ctx->worker_count;
        pread_exact(ctx->descriptor, ctx->output + begin, end - begin,
                    ctx->offset + begin);
    } catch (...) {
        (*ctx->errors)[worker] = std::current_exception();
    }
}

}  // namespace

// 将从指定起点到当前单调时钟的间隔转换为纳秒数。
uint64_t elapsed_ns(Clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
}

// 用显式偏移循环读取，处理短读与信号中断，提前到达文件末尾则报错。
void pread_exact(int descriptor, void* output, uint64_t bytes,
                 uint64_t offset) {
    char* destination = static_cast<char*>(output);
    uint64_t completed = 0;
    while (completed < bytes) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(
            bytes - completed,
            static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())));
        const ssize_t result = ::pread(
            descriptor, destination + completed, chunk,
            static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(
                "pread packed.bin failed: " +
                std::string(std::strerror(errno)));
        }
        if (result == 0) throw std::runtime_error("short read from packed.bin");
        completed += static_cast<uint64_t>(result);
    }
}

// 按数据量和线程预算读取连续区间，较小任务串行执行，预加载可分块丢弃源页缓存。
void pread_exact_parallel(int descriptor, void* output, uint64_t bytes,
                          uint64_t offset, size_t thread_count,
                          bool discard_source_cache) {
    std::vector<std::exception_ptr> errors;
    ParallelPreadContext ctx;
    ctx.descriptor = descriptor;
    ctx.output = static_cast<char*>(output);
    ctx.bytes = bytes;
    ctx.offset = offset;
    ctx.discard_source_cache = discard_source_cache;
    ctx.errors = &errors;
    if (thread_count <= 1 || bytes < 2 * kMinPreloadBytesPerThread) {
        uint64_t begin = 0;
        while (begin < bytes) {
            const uint64_t chunk = discard_source_cache
                ? std::min(kPreloadChunkBytes, bytes - begin)
                : bytes - begin;
            pread_exact(descriptor, static_cast<char*>(output) + begin,
                        chunk, offset + begin);
            discard_pread_cache(&ctx, begin, chunk);
            begin += chunk;
        }
        return;
    }
    const size_t workers = std::min<size_t>(
        thread_count,
        static_cast<size_t>(bytes / kMinPreloadBytesPerThread));
    if (workers <= 1) {
        pread_exact(descriptor, output, bytes, offset);
        return;
    }

    std::vector<std::thread> threads;
    errors.resize(workers);
    threads.reserve(workers - 1);
    ctx.worker_count = workers;
    for (size_t worker = 1; worker < workers; ++worker) {
        threads.emplace_back(parallel_pread_worker, &ctx, worker);
    }
    parallel_pread_worker(&ctx, 0);
    for (std::thread& thread : threads) thread.join();
    for (const std::exception_ptr& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

// 将文件位置移至末尾以查询有效文件大小，定位失败时抛出异常。
uint64_t file_size(int descriptor) {
    const off_t end = ::lseek(descriptor, 0, SEEK_END);
    if (end < 0) throw std::runtime_error("cannot size packed.bin");
    return static_cast<uint64_t>(end);
}

// 将全局残基位置换算到已读范围内，检查覆盖关系并解码，按需追加终止符。
void decode_request_from_extent(const SequenceRequest& request,
                                const uint8_t* packed,
                                uint64_t packed_begin,
                                uint64_t packed_end,
                                char* output,
                                bool terminate_output) {
    if (request.length == 0) {
        if (terminate_output) output[0] = '\0';
        return;
    }

    const uint64_t aligned_start = request.start & ~uint64_t{7};
    const uint64_t decode_base = aligned_start / 8 * 5;
    const uint64_t required_end = packed_bytes_for_residues(
        request.start + request.length);
    if (decode_base < packed_begin || required_end > packed_end ||
        decode_base >= packed_end) {
        throw std::runtime_error("decode extent escaped packed range");
    }
    decode_packed_5bit_range(
        packed + static_cast<size_t>(decode_base - packed_begin),
        request.start - aligned_start, request.length, output);
    if (terminate_output) output[request.length] = '\0';
}

}  // namespace aastore
