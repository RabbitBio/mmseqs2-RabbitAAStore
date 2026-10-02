// 文件职责：验证 io_uring 的范围读取、ring 与槽位复用、嵌套调用和异常收尾。
// 同时检查每个 ring 的线程归属，以及协作 CPU 工作与 I/O 完成处理均能持续推进。
#include "../src/io_uring_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
using aastore::ByteRange;
using aastore::IoUringStats;
using aastore::RangeCompletion;
using aastore::read_ranges_io_uring;

// 条件不成立时抛出带说明的测试异常。
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct File {
    int fd = -1;
    std::vector<uint8_t> bytes;

    // 创建并解除目录链接的临时文件，写入可复现的测试字节。
    explicit File(unsigned seed) : bytes(1024 * 1024) {
        char path[] = "/tmp/aastore_uring_test_XXXXXX";
        fd = ::mkstemp(path);
        check(fd >= 0, "mkstemp failed");
        ::unlink(path);
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(i * 37 + seed);
        }
        size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t result = ::write(fd, bytes.data() + offset,
                                           bytes.size() - offset);
            check(result > 0, "fixture write failed");
            offset += static_cast<size_t>(result);
        }
    }
    // 关闭测试文件，释放其匿名临时存储。
    ~File() { if (fd >= 0) ::close(fd); }
};

// 扫描当前进程描述符，统计指向 io_uring 的项。
size_t ring_count() {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code error;
        const auto target = std::filesystem::read_symlink(entry.path(), error);
        if (!error && target.string().find("io_uring") != std::string::npos) ++count;
    }
    return count;
}

struct CheckedReadContext {
    const File* file = nullptr;
    const std::vector<ByteRange>* ranges = nullptr;
    RangeCompletion extra = nullptr;
    void* extra_context = nullptr;
    size_t completed = 0;
    bool initial = false;
};

// 记录首次提交通知已到达，供完成回调校验顺序。
bool mark_initial_submit(void* opaque, size_t worker) {
    check(worker == 0, "unexpected initial submit worker");
    static_cast<CheckedReadContext*>(opaque)->initial = true;
    return false;
}

// 验证完成回调的线程、顺序、长度和内容，再执行可选附加检查。
void check_read_completion(void* opaque, size_t worker, size_t index,
                           const uint8_t* data, uint64_t size) {
    CheckedReadContext* ctx = static_cast<CheckedReadContext*>(opaque);
    check(ctx->initial && worker == 0,
          "invalid callback ordering/worker");
    check(size == (*ctx->ranges)[index].end -
                      (*ctx->ranges)[index].begin,
          "wrong size");
    check(std::memcmp(
              data,
              ctx->file->bytes.data() + (*ctx->ranges)[index].begin,
              static_cast<size_t>(size)) == 0,
          "wrong data");
    ++ctx->completed;
    if (ctx->extra != nullptr) {
        ctx->extra(ctx->extra_context, worker, index, data, size);
    }
}

// 提供无需执行操作的首次提交通知回调。
bool ignore_initial_submit(void*, size_t) { return false; }
// 提供忽略读取内容的完成回调。
void ignore_completion(void*, size_t, size_t, const uint8_t*, uint64_t) {}

// 执行单 ring 读取并核对全部范围、队列深度和提交完成计数。
void read_checked(const File& file,
                          const std::vector<ByteRange>& ranges,
                          size_t depth,
                          RangeCompletion extra = nullptr,
                          void* extra_context = nullptr) {
    IoUringStats stats;
    std::string reason;
    CheckedReadContext ctx;
    ctx.file = &file;
    ctx.ranges = &ranges;
    ctx.extra = extra;
    ctx.extra_context = extra_context;
    const bool used = read_ranges_io_uring(file.fd, ranges, 1, depth,
        mark_initial_submit, &ctx, check_read_completion, &ctx,
        stats, reason, true);
    check(used && ctx.completed == ranges.size(), "incomplete read");
    check(stats.submissions == stats.completions, "undrained completions");
    check(stats.max_outstanding <= depth, "queue depth exceeded");
    check(stats.submit_calls != 0 && stats.wait_calls != 0,
          "missing submit/wait instrumentation");
    check(stats.submissions <= stats.submit_calls * depth,
          "submit batch exceeded queue depth");
    check(stats.empty_waits <= stats.wait_calls, "invalid empty-CQ count");
}

// 确认预期的运行时错误已经被观察到。
void expect_runtime_failure(bool failed) {
    check(failed, "expected failure was not reported");
}

struct ReuseBufferContext {
    const uint8_t* previous = nullptr;
};

// 验证连续小范围读取复用了同一缓冲地址。
void check_reused_buffer(void* opaque, size_t, size_t,
                         const uint8_t* data, uint64_t) {
    ReuseBufferContext* ctx = static_cast<ReuseBufferContext*>(opaque);
    check(ctx->previous == nullptr || ctx->previous == data,
          "small read buffer was not reused");
    ctx->previous = data;
}

struct RingCountContext {
    size_t expected = 0;
};

// 校验回调期间进程持有的 ring 数量。
void check_ring_count(void* opaque, size_t, size_t,
                      const uint8_t*, uint64_t) {
    const RingCountContext* ctx =
        static_cast<const RingCountContext*>(opaque);
    check(ring_count() == ctx->expected, "unexpected retained ring count");
}

struct NestedReadContext {
    const File* file = nullptr;
    const std::vector<ByteRange>* ranges = nullptr;
    RingCountContext ring_count;
};

// 从完成回调中发起嵌套读取，并检查嵌套 ring 的生命周期。
void run_nested_read(void* opaque, size_t, size_t,
                     const uint8_t*, uint64_t) {
    NestedReadContext* ctx = static_cast<NestedReadContext*>(opaque);
    read_checked(
        *ctx->file, *ctx->ranges, 1, check_ring_count, &ctx->ring_count);
}

// 主动从完成回调抛出异常，验证后端异常收尾。
void throw_completion(void*, size_t, size_t,
                      const uint8_t*, uint64_t) {
    throw std::runtime_error("callback failure");
}

// 主动从首次提交通知抛出异常，验证后端清理。
bool throw_initial_submit(void*, size_t) {
    throw std::runtime_error("initial callback failure");
}

struct ConcurrentReadContext {
    const File* first = nullptr;
    const File* second = nullptr;
    const std::vector<ByteRange>* ranges = nullptr;
    std::atomic<bool>* failed = nullptr;
};

// 交替并发读取两个测试文件，记录线程是否出现异常。
void concurrent_read_worker(ConcurrentReadContext* ctx,
                            size_t worker) noexcept {
    try {
        for (size_t i = 0; i < 40; ++i) {
            const File& file =
                (worker + i) % 2 ? *ctx->first : *ctx->second;
            read_checked(file, *ctx->ranges, 8);
        }
    } catch (...) {
        *ctx->failed = true;
    }
}

struct OwnerContext {
    const File* file;
    const std::vector<ByteRange>* ranges;
    std::vector<std::thread::id> owners;
    std::vector<size_t> completed;
    std::vector<size_t> initial_calls;
};

// 记录各分区首次协作回调的线程标识和调用次数，不追加协作任务。
bool record_owner(void* opaque, size_t worker) {
    auto* ctx = static_cast<OwnerContext*>(opaque);
    check(worker < ctx->owners.size(), "initial callback exceeds thread budget");
    ctx->owners[worker] = std::this_thread::get_id();
    ++ctx->initial_calls[worker];
    return false;
}

// 检查完成回调仍由提交请求的 ring 所属线程执行，并核对范围内容与完成计数。
void check_owner_completion(void* opaque, size_t worker, size_t index,
                            const uint8_t* data, uint64_t size) {
    auto* ctx = static_cast<OwnerContext*>(opaque);
    check(worker < ctx->owners.size(), "completion exceeds thread budget");
    check(ctx->initial_calls[worker] == 1 &&
              ctx->owners[worker] == std::this_thread::get_id(),
          "decode must run on the submitting ring owner");
    const ByteRange& range = ctx->ranges->at(index);
    check(size == range.end - range.begin &&
              std::memcmp(data, ctx->file->bytes.data() + range.begin,
                          static_cast<size_t>(size)) == 0,
          "multi-ring read returned wrong data");
    ++ctx->completed[worker];
}

// 验证多 ring 独占各自线程、调用线程计入预算，且全部分区读取完整。
void check_owners(const File& file, const std::vector<ByteRange>& ranges,
                  size_t threads) {
    const size_t workers = std::max<size_t>(1, std::min(threads, ranges.size()));
    OwnerContext ctx{&file, &ranges, std::vector<std::thread::id>(workers),
                     std::vector<size_t>(workers), std::vector<size_t>(workers)};
    IoUringStats stats;
    std::string reason;
    check(read_ranges_io_uring(
              file.fd, ranges, threads, 4, record_owner, &ctx,
              check_owner_completion, &ctx, stats, reason),
          "multi-ring backend unavailable");
    check(ctx.owners[0] == std::this_thread::get_id(),
          "calling thread must count toward the thread budget");
    check(std::set<std::thread::id>(ctx.owners.begin(), ctx.owners.end()).size()
              == workers,
          "each ring must have its own owner");
    size_t completed = 0;
    for (size_t worker = 0; worker < workers; ++worker) {
        check(ctx.initial_calls[worker] == 1 && ctx.completed[worker] != 0,
              "worker did not submit and decode its partition");
        completed += ctx.completed[worker];
    }
    check(completed == ranges.size() && stats.completions == stats.submissions,
          "multi-ring read lost completions");
    check(stats.submit_calls == 0 && stats.submit_ns == 0 &&
              stats.wait_calls == 0 && stats.empty_waits == 0 && stats.wait_ns == 0,
          "default multi-ring reads must not collect diagnostics");
}

struct CooperativeContext {
    CheckedReadContext reads;
    size_t slices = 0;
    size_t tail_slices = 0;
    size_t fail_after_reads = 0;
    std::thread::id owner = std::this_thread::get_id();
};

// 保持协作 CPU 工作直到全部读取完成，再执行三个尾部片段；可在指定进度抛出异常。
bool cooperative_work(void* opaque, size_t worker) {
    auto* ctx = static_cast<CooperativeContext*>(opaque);
    check(worker == 0 && std::this_thread::get_id() == ctx->owner,
          "cooperative work changed ring owner");
    ctx->reads.initial = true;
    check(++ctx->slices < 1000000, "CPU work starved I/O completions/refills");
    if (ctx->fail_after_reads &&
        ctx->reads.completed >= ctx->fail_after_reads) {
        throw std::runtime_error("late cooperative work failure");
    }
    if (ctx->reads.completed == ctx->reads.ranges->size()) {
        return ++ctx->tail_slices < 3;
    }
    std::this_thread::yield();
    return true;
}

// 验证协作工作不阻塞完成处理和补读，尾部任务能排空，后期异常能正确传播。
void check_cooperative_work(const File& file,
                            const std::vector<ByteRange>& ranges,
                            size_t depth, bool fail) {
    CooperativeContext ctx;
    ctx.reads.file = &file;
    ctx.reads.ranges = &ranges;
    ctx.fail_after_reads = fail ? depth + 1 : 0;
    IoUringStats stats;
    std::string reason;
    bool failed = false;
    try {
        check(read_ranges_io_uring(
                  file.fd, ranges, 1, depth, cooperative_work, &ctx,
                  check_read_completion, &ctx.reads, stats, reason),
              "cooperative backend unavailable");
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()) != "late cooperative work failure") throw;
        failed = true;
    }
    check(failed == fail, "cooperative callback exception was lost");
    if (!fail) {
        check(ctx.reads.completed == ranges.size() && ctx.tail_slices == 3,
              "cooperative work or disk reads were left unfinished");
        check(stats.submissions == ranges.size() &&
                  stats.completions == ranges.size() &&
                  stats.max_outstanding <= depth,
              "cooperative reads violated slot accounting");
    }
}

}  // namespace

// 覆盖 ring 复用、嵌套读取、异常收尾、并发和协作轮询路径。
int main() {
    try {
        File first(13), second(91);
        const size_t initial_rings = ring_count();
        const std::vector<ByteRange> one = {{32, 160, 0, 1}};
        IoUringStats stats;
        std::string reason;
        if (!read_ranges_io_uring(
                first.fd, one, 1, 1,
                ignore_initial_submit, nullptr,
                ignore_completion, nullptr, stats, reason)) {
            std::cout << "io_uring unavailable: " << reason << '\n';
            return 77;
        }
        check(ring_count() == initial_rings + 1, "ring not retained");
        check(stats.submissions == 1 && stats.completions == 1 &&
                  stats.submit_calls == 0 && stats.submit_ns == 0 &&
                  stats.wait_calls == 0 && stats.empty_waits == 0 && stats.wait_ns == 0,
              "default single-ring reads keep basic counts without diagnostics");

        ReuseBufferContext reuse_ctx;
        for (size_t i = 0; i < 100; ++i) {
            read_checked(
                i % 2 ? first : second, one, i % 2 ? 1 : 64,
                check_reused_buffer, &reuse_ctx);
        }
        std::vector<ByteRange> ranges;
        for (size_t i = 0; i < 129; ++i) {
            ranges.push_back({i * 1024, i * 1024 + 512, i, i + 1});
        }
        for (size_t depth : {size_t{1}, size_t{4}, size_t{64}, size_t{129}}) {
            read_checked(first, ranges, depth);
            check(ring_count() == initial_rings + 1, "ring descriptor leak");
        }
        for (size_t depth : {size_t{1}, size_t{4}, size_t{64}}) {
            check_cooperative_work(first, ranges, depth, false);
            check_cooperative_work(first, ranges, depth, true);
            check(ring_count() == initial_rings,
                  "late cooperative failure retained a ring");
            read_checked(second, ranges, depth);
        }

        for (size_t threads : {size_t{0}, size_t{1}, size_t{2}, size_t{8}}) {
            check_owners(first, ranges, threads);
            check_owners(second, one, threads);
        }
        for (bool fail_initial : {false, true}) {
            bool failed = false;
            try {
                IoUringStats failed_stats;
                read_ranges_io_uring(
                    first.fd, ranges, 4, 4,
                    fail_initial ? throw_initial_submit : ignore_initial_submit,
                    nullptr, fail_initial ? ignore_completion : throw_completion,
                    nullptr, failed_stats, reason);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            expect_runtime_failure(failed);
            check(ring_count() == initial_rings + 1,
                  "multi-ring callback failure leaked rings");
            check_owners(first, ranges, 4);
        }

        RingCountContext nested_ring_count{initial_rings + 2};
        NestedReadContext nested_ctx{&second, &one, nested_ring_count};
        read_checked(first, one, 1, run_nested_read, &nested_ctx);
        // 深度为 1 时，单槽可使用完整的 16 MiB 保留预算；
        // 300 KiB 范围应继续使用线程本地 ring，而不是创建临时 ring。
        RingCountContext large_ring_count{initial_rings + 1};
        read_checked(
            first, {{0, 300 * 1024, 0, 1}}, 1,
            check_ring_count, &large_ring_count);
        check(ring_count() == initial_rings + 1, "retained ring was replaced");

        bool failed_as_expected = false;
        try {
            read_checked(first, ranges, 64,
                         throw_completion);
        } catch (const std::runtime_error&) {
            failed_as_expected = true;
        }
        expect_runtime_failure(failed_as_expected);
        check(ring_count() == initial_rings, "failed ring retained");
        read_checked(second, ranges, 64);

        for (bool bad_descriptor : {true, false}) {
            failed_as_expected = false;
            try {
                IoUringStats failed_stats;
                std::string failed_reason;
                auto invalid = ranges;
                if (!bad_descriptor) {
                    invalid[0] = {first.bytes.size() - 5, first.bytes.size() + 5, 0, 1};
                }
                read_ranges_io_uring(bad_descriptor ? -1 : first.fd, invalid,
                    1, 64, ignore_initial_submit, nullptr,
                    ignore_completion, nullptr, failed_stats, failed_reason);
            } catch (const std::runtime_error&) {
                failed_as_expected = true;
            }
            expect_runtime_failure(failed_as_expected);
            check(ring_count() == initial_rings, "I/O error left a ring alive");
            read_checked(second, ranges, 64);
        }
        failed_as_expected = false;
        try {
            IoUringStats failed_stats;
            read_ranges_io_uring(first.fd, ranges, 1, 64,
                throw_initial_submit, nullptr, ignore_completion, nullptr,
                failed_stats, reason);
        } catch (const std::runtime_error&) {
            failed_as_expected = true;
        }
        expect_runtime_failure(failed_as_expected);
        read_checked(first, ranges, 64);

        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        ConcurrentReadContext concurrent_ctx{
            &first, &second, &ranges, &failed};
        for (size_t worker = 0; worker < 8; ++worker) {
            workers.emplace_back(
                concurrent_read_worker, &concurrent_ctx, worker);
        }
        for (auto& worker : workers) worker.join();
        check(!failed, "concurrent read failed");
        check(ring_count() == initial_rings + 1, "thread exit leaked rings");
        std::cout << "io_uring reuse, error recovery, nesting and concurrency passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
