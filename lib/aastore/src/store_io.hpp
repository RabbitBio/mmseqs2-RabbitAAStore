// 文件职责：声明读取路径共用的上下文、精确读取、解码和线程执行辅助接口。
// 批量随机读取与连续区间读取通过 StoreReadContext 借用 Store 持有的数据资源。
#ifndef AASTORE_STORE_IO_HPP
#define AASTORE_STORE_IO_HPP

#include "byte_range_planner.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <thread>
#include <vector>

namespace aastore {

using Clock = std::chrono::steady_clock;

class Metadata;

struct StoreReadContext {
    const Metadata* metadata = nullptr;
    StorageMode storage_mode = StorageMode::ByteRange;
    int packed_descriptor = -1;
    const uint8_t* full_packed = nullptr;
    uint64_t full_packed_bytes = 0;
    const uint8_t* prefix_packed = nullptr;
    uint64_t prefix_packed_bytes = 0;
};

// 返回从给定单调时钟时刻到现在经过的纳秒数。
uint64_t elapsed_ns(Clock::time_point start);

// 从指定文件偏移读足全部字节，重试中断，遇到提前 EOF 或其他错误时抛出异常。
void pread_exact(int descriptor, void* output, uint64_t bytes,
                 uint64_t offset);

// 将大区间分配给多个线程读取，可选在复制后提示内核丢弃对应源文件页缓存。
void pread_exact_parallel(int descriptor, void* output, uint64_t bytes,
                          uint64_t offset, size_t thread_count,
                          bool discard_source_cache = false);

// 通过定位到文件末尾取得字节数；后续定位读取使用显式偏移。
uint64_t file_size(int descriptor);

// 校验已读物理区间覆盖请求后解码为字符，可选追加 NUL 终止符。
void decode_request_from_extent(const SequenceRequest& request,
                                const uint8_t* packed,
                                uint64_t packed_begin,
                                uint64_t packed_end,
                                char* output,
                                bool terminate_output = true);

// 启动带上下文的索引任务；调用线程执行第零个任务，并等待其他线程完成。
template <typename Context>
void run_indexed_workers(size_t worker_count,
                         void (*worker_function)(Context*, size_t),
                         Context* context) {
    if (worker_count == 0) return;
    std::vector<std::thread> threads;
    threads.reserve(worker_count - 1);
    for (size_t worker = 1; worker < worker_count; ++worker) {
        threads.emplace_back(worker_function, context, worker);
    }
    worker_function(context, 0);
    for (std::thread& thread : threads) thread.join();
}

// 按线程索引执行可调用对象，调用线程参与工作；任务须自行处理跨线程异常。
template <typename Worker>
void run_indexed_workers(size_t worker_count, Worker&& worker_function) {
    if (worker_count == 0) return;
    std::vector<std::thread> threads;
    threads.reserve(worker_count - 1);
    for (size_t worker = 1; worker < worker_count; ++worker) {
        threads.emplace_back([&worker_function, worker]() {
            worker_function(worker);
        });
    }
    worker_function(0);
    for (std::thread& thread : threads) thread.join();
}

// 在全部工作线程结束后，按槽位顺序重新抛出首个已记录的异常。
inline void rethrow_worker_errors(
        const std::vector<std::exception_ptr>& errors) {
    for (const std::exception_ptr& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

// 执行随机批读；outputs 非空时直接写入调用方缓冲，不构造输出 arena。
void fill_batch_from_store(const StoreReadContext& store,
                           const std::vector<SequenceId>& ids,
                           const ReadOptions& options,
                           Batch& batch,
                           char* const* outputs = nullptr);

// 读取连续半开 ID 区间；outputs 非空时直接写入调用方缓冲，否则填充 Batch。
void sequential_get_from_store(const StoreReadContext& store,
                               SequenceId first_sequence,
                               SequenceId end_sequence,
                               const ReadOptions& options,
                               Batch& batch,
                               char* const* outputs = nullptr);

}  // namespace aastore

#endif
