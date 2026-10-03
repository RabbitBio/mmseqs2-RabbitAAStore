// 文件职责：按累计字节分配连续范围分区，用独立 ring 并发读取并直接解码完成项。
// 管理读槽位、短读续传、ring 复用与异常收尾，并穿插处理有限前缀解码任务。
#include "io_uring_backend.hpp"
#include "read_timer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(AASTORE_HAVE_IO_URING)
#include <liburing.h>
#include <sys/uio.h>
#endif

namespace aastore {

namespace {

#if defined(AASTORE_HAVE_IO_URING)
// 一个在途读取范围的缓冲槽；短读继续使用本槽，完整读取并回调后才允许复用。
struct ReadSlot {
    // 当前范围在本批 ranges 数组中的下标，不是序列 ID。
    size_t range_index = 0;
    // 本范围累计读到的字节数，短读续传从此位置继续填充。
    uint64_t completed = 0;
    // 本槽拥有的原始字节缓冲；I/O 完成和同步回调返回前不得移动或覆盖。
    std::vector<uint8_t, NoInitAllocator<uint8_t>> buffer;
    // READV 的单段目标描述，指向 buffer 中尚未读完的部分。
    // Linux 5.4 及以前要求 READV 的 iovec 一直有效到完成事件到达。
    iovec vector {};
};

// 一个工作线程独占的 ring 及其缓冲槽，可由线程本地状态跨批次复用。
struct ReadRing {
    // liburing 管理的提交/完成队列和内核 ring 资源。
    io_uring ring {};
    // 队列是否初始化成功，决定 reset 是否需要调用 queue_exit。
    bool initialized = false;
    // 当前是否已被一次调用借用，阻止嵌套回调复用同一个 ring。
    bool busy = false;
    // 是否使用 READV 兼容路径；false 表示内核和 liburing 均支持 READ。
    bool use_readv = true;
    // 初始化 ring 时指定的队列深度；下次调用深度变化时重建资源。
    size_t capacity = 0;
    // 各读取槽的用户态缓冲；reset 只销毁 ring，不直接释放此数组。
    std::vector<ReadSlot> slots;

    // 销毁已初始化的 ring，保留可复用的用户态缓冲。
    void reset() noexcept {
        if (initialized) io_uring_queue_exit(&ring);
        initialized = false;
        capacity = 0;
    }

    // 释放当前 ring 的内核资源。
    ~ReadRing() { reset(); }
};

// 依据操作码能力选择读取方式；旧内核或旧 liburing 无探测接口时使用 READV。
// 探测失败不代表读取失败，后续真实 I/O 和回调错误仍照常向调用方传播。
bool select_read_opcode(ReadRing& state, std::string& unavailable_reason) {
    state.use_readv = true;
#if defined(AASTORE_URING_HAS_PROBE)
    const std::unique_ptr<io_uring_probe, decltype(&std::free)> probe(
        io_uring_get_probe_ring(&state.ring), &std::free);
    if (!probe) return true;
#if defined(AASTORE_URING_HAS_READ)
    if (io_uring_opcode_supported(probe.get(), IORING_OP_READ)) {
        state.use_readv = false;
        return true;
    }
#endif
    if (!io_uring_opcode_supported(probe.get(), IORING_OP_READV)) {
        unavailable_reason = "no usable io_uring READ or READV opcode";
        return false;
    }
#else
    (void)unavailable_reason;
#endif
    return true;
}

// 每个同步调用线程最多保留 16 MiB 读取缓冲；较大范围、
// 嵌套回调和多线程调用使用临时 ring。
constexpr size_t kMaxRetainedQueueDepth = 64;
thread_local ReadRing retained_ring;

// 一次 ring 借用的作用域保护，正常归还时保留资源，异常退出时销毁内核队列。
struct ReadRingLease {
    // 借用的本地或线程本地 ring，其生命周期须覆盖本保护器。
    ReadRing& state;
    // 本次分区是否正常读完；false 时析构先 reset 再解除 busy 标志。
    bool complete = false;

    // 标记 ring 正被当前调用占用，防止嵌套回调再次借用。
    explicit ReadRingLease(ReadRing& ring) : state(ring) { state.busy = true; }
    // 归还 ring；调用未正常完成时先销毁内核队列。
    ~ReadRingLease() {
        if (!complete) state.reset();
        state.busy = false;
    }
};

// 检查指定范围区间能否装入线程本地保留缓冲。
bool ranges_fit_retained_buffers(const std::vector<ByteRange>& ranges,
                                 size_t begin, size_t end,
                                 size_t maximum_slot_bytes) {
    for (size_t i = begin; i < end; ++i) {
        if (ranges[i].end - ranges[i].begin > maximum_slot_bytes) {
            return false;
        }
    }
    return true;
}

// 单个分区的 I/O 循环状态，串联槽位分配、请求提交、完成回调和短读续传。
// 全部可变状态仅由该 ring 的所属线程修改。
struct PartitionIoContext {
    // 借用的源文件描述符，通常指向 packed.bin，也可用于文本列。
    int descriptor = -1;
    // 全批按文件偏移排序的读取范围，执行期间保持不变。
    const std::vector<ByteRange>* ranges = nullptr;
    // 本 ring 所属线程编号，传给完成回调以选择线程统计槽。
    size_t worker_index = 0;
    // 本分区最后一个范围的下一下标，限定 ranges 中的处理区间。
    size_t partition_end = 0;
    // 是否使用线程本地保留 ring，决定缓冲扩容时的容量控制方式。
    bool reuse_ring = false;
    // 本线程独占使用的 ring 状态，不在工作线程之间共享。
    ReadRing* ring = nullptr;
    // 借用 ring 中的槽数组，SQE user_data 存放其下标。
    std::vector<ReadSlot>* slots = nullptr;
    // 可重新分配的槽位下标栈；只有回调结束后才把完整读取的槽放回。
    std::vector<size_t>* free_slots = nullptr;
    // 完整范围的同步处理回调，由本 ring 所属线程直接执行。
    RangeCompletion complete = nullptr;
    // 借用的回调上下文，具体类型由上层序列解码或文本复制逻辑决定。
    void* complete_context = nullptr;
    // 本分区独占的统计槽，分区结束后由调用方合并。
    IoUringStats* stats = nullptr;
    // 尚未首次分配槽位的下一个范围下标；短读续传不推进此计数。
    size_t next_range = 0;
    // 已提交但尚未消费 CQE 的请求数，不包含待提交 SQE 或正在执行的回调。
    size_t outstanding = 0;
    // 是否记录 submit/wait API 耗时与调用次数，基本请求统计始终保留。
    bool collect_diagnostics = false;
};

// 为槽位准备首次或短读续传请求，校验单次读取长度。
void prepare_partition_read(PartitionIoContext* ctx, size_t slot_index,
                            bool continuing) {
    ReadSlot& slot = (*ctx->slots)[slot_index];
    const ByteRange& range = (*ctx->ranges)[slot.range_index];
    const uint64_t range_bytes = range.end - range.begin;
    if (!continuing) {
        if (range_bytes > static_cast<uint64_t>(
                std::numeric_limits<unsigned>::max()) ||
            range_bytes > static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
            throw std::length_error("io_uring range exceeds one read request");
        }
        slot.completed = 0;
        if (ctx->reuse_ring && range_bytes > slot.buffer.capacity()) {
            decltype(slot.buffer) buffer(static_cast<size_t>(range_bytes));
            slot.buffer.swap(buffer);
        }
        slot.buffer.resize(static_cast<size_t>(range_bytes));
    }
    const uint64_t remaining = range_bytes - slot.completed;
    io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring->ring);
    if (sqe == nullptr) {
        throw std::runtime_error("io_uring submission queue is full");
    }
    const auto offset = static_cast<off_t>(range.begin + slot.completed);
#if defined(AASTORE_URING_HAS_READ)
    if (!ctx->ring->use_readv) {
        io_uring_prep_read(
            sqe, ctx->descriptor,
            slot.buffer.data() + static_cast<size_t>(slot.completed),
            static_cast<unsigned>(remaining), offset);
    } else
#endif
    {
        slot.vector.iov_base =
            slot.buffer.data() + static_cast<size_t>(slot.completed);
        slot.vector.iov_len = static_cast<size_t>(remaining);
        io_uring_prep_readv(sqe, ctx->descriptor, &slot.vector, 1, offset);
    }
    sqe->user_data = static_cast<unsigned long long>(slot_index);
}

// 提交全部已准备请求，重试暂时性错误并累计提交统计。
void submit_partition_reads(PartitionIoContext* ctx, size_t prepared) {
    size_t submitted_total = 0;
    size_t zero_submits = 0;
    while (submitted_total < prepared) {
        const DiagnosticTimer submit_timer(ctx->collect_diagnostics);
        const int submitted = io_uring_submit(&ctx->ring->ring);
        if (ctx->collect_diagnostics) {
            ctx->stats->submit_ns += submit_timer.elapsed_ns();
            ++ctx->stats->submit_calls;
        }
        if (submitted < 0) {
            if (submitted == -EINTR || submitted == -EAGAIN ||
                submitted == -EBUSY) {
                continue;
            }
            throw std::runtime_error(
                "io_uring_submit failed: " +
                std::string(std::strerror(-submitted)));
        }
        if (submitted == 0) {
            if (++zero_submits > 1024) {
                throw std::runtime_error("io_uring_submit made no progress");
            }
            continue;
        }
        zero_submits = 0;
        submitted_total += static_cast<size_t>(submitted);
        ctx->outstanding += static_cast<size_t>(submitted);
        ctx->stats->submissions += static_cast<size_t>(submitted);
        ctx->stats->max_outstanding = std::max<uint64_t>(
            ctx->stats->max_outstanding, ctx->outstanding);
    }
}

// 将空闲槽位分配给后续范围，并统一提交准备好的读取。
void fill_partition_queue(PartitionIoContext* ctx) {
    size_t prepared = 0;
    while (!ctx->free_slots->empty() &&
           ctx->next_range < ctx->partition_end) {
        const size_t slot_index = ctx->free_slots->back();
        ctx->free_slots->pop_back();
        (*ctx->slots)[slot_index].range_index = ctx->next_range;
        ++ctx->next_range;
        prepare_partition_read(ctx, slot_index, false);
        ++prepared;
    }
    if (prepared != 0) submit_partition_reads(ctx, prepared);
}

// 处理一个完成事件；短读安排续传，完整范围交给解码回调。
void process_partition_completion(PartitionIoContext* ctx,
                                  io_uring_cqe* completion,
                                  std::vector<size_t>* partial_slots) {
    const size_t slot_index = static_cast<size_t>(completion->user_data);
    const int result = completion->res;
    io_uring_cqe_seen(&ctx->ring->ring, completion);
    --ctx->outstanding;
    ++ctx->stats->completions;
    if (slot_index >= ctx->slots->size()) {
        throw std::runtime_error("io_uring returned an invalid slot id");
    }
    if (result < 0) {
        throw std::runtime_error(
            "io_uring read failed: " +
            std::string(std::strerror(-result)));
    }
    if (result == 0) throw std::runtime_error("short read from packed.bin");

    ReadSlot& slot = (*ctx->slots)[slot_index];
    slot.completed += static_cast<uint64_t>(result);
    const ByteRange& range = (*ctx->ranges)[slot.range_index];
    const uint64_t range_bytes = range.end - range.begin;
    if (slot.completed < range_bytes) {
        partial_slots->push_back(slot_index);
        return;
    }
    if (slot.completed != range_bytes) {
        throw std::runtime_error("io_uring read exceeded requested range");
    }
    ctx->complete(ctx->complete_context, ctx->worker_index, slot.range_index,
                  slot.buffer.data(), range_bytes);
    ctx->free_slots->push_back(slot_index);
}

// 用一个 ring 处理连续物理分区，协调读槽位复用、解码和异常收尾。
bool read_range_partition_io_uring(
        int descriptor,
        const std::vector<ByteRange>& ranges,
        size_t worker_index,
        size_t partition_begin,
        size_t partition_end,
        size_t queue_depth,
        bool reuse_ring,
        CooperativeWork work,
        void* work_context,
        RangeCompletion complete,
        void* complete_context,
        IoUringStats& stats,
        std::string& unavailable_reason,
        bool collect_diagnostics) {
    // 每个 ring 处理连续的物理区间。输入范围已按文件偏移排序，
    // 若使用交错步长分配，原本连续的范围就会被拆散，
    // 使顺序访问变为交错的随机读取。
    const size_t partition_size = partition_end - partition_begin;
    queue_depth = std::max<size_t>(1, std::min(queue_depth, partition_size));
    queue_depth = std::min<size_t>(
        queue_depth, static_cast<size_t>(std::numeric_limits<unsigned>::max()));

    const size_t retained_slot_bytes = kIoBufferBytes / queue_depth;
    reuse_ring = reuse_ring && !retained_ring.busy &&
        queue_depth <= kMaxRetainedQueueDepth &&
        ranges_fit_retained_buffers(
            ranges, partition_begin, partition_end, retained_slot_bytes);
    ReadRing local_ring;
    ReadRing& guard = reuse_ring ? retained_ring : local_ring;
    ReadRingLease lease(guard);
    const size_t capacity = queue_depth;
    if (guard.initialized && guard.capacity != capacity) {
        guard.reset();
        decltype(guard.slots) slots;
        guard.slots.swap(slots);
    }
    if (!guard.initialized) {
        io_uring_params params {};
        const int init_result = io_uring_queue_init_params(
            static_cast<unsigned>(capacity), &guard.ring, &params);
        if (init_result < 0) {
            unavailable_reason = std::strerror(-init_result);
            return false;
        }
        guard.initialized = true;
        guard.capacity = capacity;
        if (!select_read_opcode(guard, unavailable_reason)) return false;
        guard.slots.resize(capacity);
    }
    std::vector<ReadSlot>& slots = guard.slots;
    std::vector<size_t> free_slots;
    free_slots.reserve(queue_depth);
    for (size_t slot = queue_depth; slot > 0; --slot) {
        free_slots.push_back(slot - 1);
    }
    PartitionIoContext io{
        descriptor, &ranges, worker_index, partition_end, reuse_ring,
        &guard, &slots, &free_slots, complete, complete_context, &stats,
        partition_begin, 0, collect_diagnostics};
    try {
        fill_partition_queue(&io);
        bool work_pending = work != nullptr;
        std::vector<size_t> partial_slots;
        partial_slots.reserve(queue_depth);
        while (io.next_range < partition_end || io.outstanding != 0 ||
               work_pending) {
            fill_partition_queue(&io);
            if (work_pending) work_pending = work(work_context, worker_index);
            if (io.outstanding == 0) {
                if (io.next_range < partition_end) {
                    throw std::logic_error(
                        "io_uring has unread ranges without free slots");
                }
                if (work_pending) continue;
                break;
            }

            partial_slots.clear();
            io_uring_cqe* cqe = nullptr;
            int wait_result;
            if (work_pending) {
                // 前缀仍有任务时只探测 CQ，未完成的读取与后续前缀解码继续重叠。
                wait_result = io_uring_peek_cqe(&guard.ring, &cqe);
                if (wait_result == -EAGAIN) continue;
            } else {
                const DiagnosticTimer wait_timer(collect_diagnostics);
                do {
                    if (collect_diagnostics) {
                        ++stats.wait_calls;
                        if (io_uring_cq_ready(&guard.ring) == 0) ++stats.empty_waits;
                    }
                    wait_result = io_uring_wait_cqe(&guard.ring, &cqe);
                } while (wait_result == -EINTR);
                if (collect_diagnostics) stats.wait_ns += wait_timer.elapsed_ns();
            }
            if (wait_result < 0) {
                throw std::runtime_error(
                    "io_uring completion retrieval failed: " +
                    std::string(std::strerror(-wait_result)));
            }

            process_partition_completion(&io, cqe, &partial_slots);
            while (true) {
                cqe = nullptr;
                const int peek_result = io_uring_peek_cqe(&guard.ring, &cqe);
                if (peek_result == -EAGAIN) break;
                if (peek_result < 0) {
                    throw std::runtime_error(
                        "io_uring_peek_cqe failed: " +
                        std::string(std::strerror(-peek_result)));
                }
                process_partition_completion(&io, cqe, &partial_slots);
            }

            size_t prepared = 0;
            for (size_t slot_index : partial_slots) {
                prepare_partition_read(&io, slot_index, true);
                ++prepared;
            }
            if (prepared != 0) {
                submit_partition_reads(&io, prepared);
            }
        }
    } catch (...) {
        // 回调或读取失败后必须收尾在途 I/O，防止其继续写入下一批复用的缓冲；
        // 尚未提交的 SQE 随 ring 一起销毁。
        while (io.outstanding != 0) {
            io_uring_cqe* completion = nullptr;
            const int result = io_uring_wait_cqe(&guard.ring, &completion);
            if (result == -EINTR) continue;
            if (result < 0) break;
            io_uring_cqe_seen(&guard.ring, completion);
            --io.outstanding;
        }
        guard.reset();
        throw;
    }
    lease.complete = true;
    return true;
}
#endif

}  // namespace


#if defined(AASTORE_HAVE_IO_URING)
namespace {

// 多 ring 调度的共享参数，各线程只写自己的统计、失败原因和异常槽位。
struct RangeWorkerContext {
    // 借用的源文件描述符，各线程通过显式偏移读取，不共享文件游标。
    int descriptor = -1;
    // 本批完整的有序读取范围列表。
    const std::vector<ByteRange>* ranges = nullptr;
    // 长度为 worker_count + 1 的范围下标边界数组，每个线程处理相邻两项之间。
    const std::vector<size_t>* partition_bounds = nullptr;
    // 实际 I/O 工作线程数，包含调用线程；单线程时才尝试复用保留 ring。
    size_t worker_count = 0;
    // 每个 ring 的目标深度，进入分区后还会按分区范围数进一步缩小。
    size_t queue_depth = 0;
    // 每个完整范围都要执行的同步完成回调。
    RangeCompletion complete = nullptr;
    // 各工作线程共用的上层完成上下文，回调负责保证输出互不冲突。
    void* complete_context = nullptr;
    // 借用的逐线程统计数组，全部 join 后汇总。
    std::vector<IoUringStats>* stats = nullptr;
    // 各线程后端初始化或读取操作码不可用时记录的原因。
    std::vector<std::string>* reasons = nullptr;
    // 各线程捕获的异常，由调用方 join 后重新抛出。
    std::vector<std::exception_ptr>* errors = nullptr;
    // 各线程是否成功使用了 io_uring 路径，非零为成功。
    std::vector<uint8_t>* used = nullptr;
    // 可选的有限工作片回调，在 I/O 处理循环中执行，当前用于前缀解码。
    CooperativeWork work = nullptr;
    // 借用的协作任务共享上下文，通常含原子任务领取下标。
    void* work_context = nullptr;
    // 本批是否启用详细诊断，传递到每个 ring。
    bool collect_diagnostics = false;
};

// 执行一个物理分区的读取，并将异常保存到对应线程槽位。
void run_range_worker(RangeWorkerContext* ctx, size_t worker) noexcept {
    try {
        (*ctx->used)[worker] = read_range_partition_io_uring(
            ctx->descriptor, *ctx->ranges, worker,
            (*ctx->partition_bounds)[worker],
            (*ctx->partition_bounds)[worker + 1], ctx->queue_depth,
            ctx->worker_count == 1,
            ctx->work, ctx->work_context,
            ctx->complete, ctx->complete_context,
            (*ctx->stats)[worker], (*ctx->reasons)[worker],
            ctx->collect_diagnostics);
    } catch (...) {
        (*ctx->errors)[worker] = std::current_exception();
    }
}
}  // namespace
#endif

// 按累计字节划分有序范围并执行读取；初始化或读取操作码不可用时返回 false。
bool read_ranges_io_uring(int descriptor,
                          const std::vector<ByteRange>& ranges,
                          size_t worker_count,
                          size_t queue_depth,
                          CooperativeWork work,
                          void* work_context,
                          RangeCompletion complete,
                          void* complete_context,
                          IoUringStats& stats,
                          std::string& unavailable_reason,
                          bool collect_diagnostics) {
#if !defined(AASTORE_HAVE_IO_URING)
    (void)descriptor;
    (void)ranges;
    (void)worker_count;
    (void)queue_depth;
    (void)work;
    (void)work_context;
    (void)complete;
    (void)complete_context;
    (void)stats;
    (void)collect_diagnostics;
    unavailable_reason = "liburing was not available at build time";
    return false;
#else
    if (ranges.empty()) return true;
    worker_count = std::max<size_t>(
        1, std::min(worker_count, ranges.size()));
    queue_depth = std::max<size_t>(1, queue_depth);
    if (complete == nullptr) {
        throw std::invalid_argument("missing range completion callback");
    }

    // 按累计字节数分区，而不是只平衡范围个数。
    // 单条长序列的物理范围可能远大于邻近范围，
    // 只平衡个数会让某个 ring 在其他 ring 空闲后仍处理大量数据。
    // 分区边界保持单调，每个 ring 仍持有 packed 文件中的
    // 一个连续物理区间。
    std::vector<size_t> partition_bounds(worker_count + 1, 0);
    partition_bounds.back() = ranges.size();
    uint64_t remaining_bytes = 0;
    for (const ByteRange& range : ranges) {
        remaining_bytes += range.end - range.begin;
    }
    size_t partition_begin = 0;
    for (size_t worker = 0; worker + 1 < worker_count; ++worker) {
        const size_t remaining_workers = worker_count - worker;
        const size_t latest_end = ranges.size() -
            (remaining_workers - 1);
        const uint64_t target_bytes = remaining_bytes / remaining_workers;
        uint64_t assigned_bytes = 0;
        size_t partition_end = partition_begin;
        while (partition_end < latest_end &&
               (partition_end == partition_begin ||
                assigned_bytes < target_bytes)) {
            assigned_bytes += ranges[partition_end].end -
                ranges[partition_end].begin;
            ++partition_end;
        }
        partition_bounds[worker] = partition_begin;
        partition_bounds[worker + 1] = partition_end;
        partition_begin = partition_end;
        remaining_bytes -= assigned_bytes;
    }
    partition_bounds[worker_count - 1] = partition_begin;

    std::vector<IoUringStats> worker_stats(worker_count);
    std::vector<std::string> worker_reasons(worker_count);
    std::vector<std::exception_ptr> worker_errors(worker_count);
    std::vector<uint8_t> worker_used(worker_count, 0);
    RangeWorkerContext worker_context{
        descriptor, &ranges, &partition_bounds, worker_count,
        queue_depth, complete, complete_context, &worker_stats,
        &worker_reasons, &worker_errors, &worker_used,
        work, work_context, collect_diagnostics};

    std::vector<std::thread> workers;
    workers.reserve(worker_count - 1);
    try {
        for (size_t worker = 1; worker < worker_count; ++worker) {
            workers.emplace_back(run_range_worker, &worker_context, worker);
        }
    } catch (...) {
        for (std::thread& worker : workers) worker.join();
        throw;
    }
    run_range_worker(&worker_context, 0);
    for (std::thread& worker : workers) worker.join();
    for (const std::exception_ptr& error : worker_errors) {
        if (error) std::rethrow_exception(error);
    }
    bool all_used = true;
    for (size_t worker = 0; worker < worker_count; ++worker) {
        stats.submissions += worker_stats[worker].submissions;
        stats.completions += worker_stats[worker].completions;
        stats.wait_ns += worker_stats[worker].wait_ns;
        stats.max_outstanding += worker_stats[worker].max_outstanding;
        stats.submit_calls += worker_stats[worker].submit_calls;
        stats.submit_ns += worker_stats[worker].submit_ns;
        stats.wait_calls += worker_stats[worker].wait_calls;
        stats.empty_waits += worker_stats[worker].empty_waits;
        if (!worker_used[worker]) {
            all_used = false;
            if (unavailable_reason.empty()) {
                unavailable_reason = worker_reasons[worker];
            }
        }
    }
    return all_used;
#endif
}

}  // namespace aastore
