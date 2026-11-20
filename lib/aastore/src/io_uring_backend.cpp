#include "io_uring_backend.hpp"
#include "read_timer.hpp"

#include <algorithm>
#include <cerrno>
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
struct ReadSlot {
    size_t range_index = 0;
    uint64_t completed = 0;
    std::vector<uint8_t, NoInitAllocator<uint8_t>> buffer;
    iovec vector {};
};

struct ReadRing {
    io_uring ring {};
    bool initialized = false;
    bool busy = false;
    bool use_readv = true;
    size_t capacity = 0;
    std::vector<ReadSlot> slots;

    void reset() noexcept {
        if (initialized) io_uring_queue_exit(&ring);
        initialized = false;
        capacity = 0;
    }

    ~ReadRing() { reset(); }
};

bool select_read_opcode(ReadRing& state, std::string& unavailable_reason) {
    state.use_readv = true;
#if defined(AASTORE_URING_HAS_PROBE)
    // liburing may use its own allocator, including in no-libc builds.
    const std::unique_ptr<io_uring_probe, decltype(&io_uring_free_probe)> probe(
        io_uring_get_probe_ring(&state.ring), &io_uring_free_probe);
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

constexpr size_t kMaxRetainedQueueDepth = 64;
thread_local ReadRing retained_ring;

struct ReadRingLease {
    ReadRing& state;
    bool complete = false;

    explicit ReadRingLease(ReadRing& ring) : state(ring) { state.busy = true; }
    ~ReadRingLease() {
        if (!complete) state.reset();
        state.busy = false;
    }
};

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

struct PartitionIoContext {
    int descriptor = -1;
    const std::vector<ByteRange>* ranges = nullptr;
    size_t worker_index = 0;
    size_t partition_end = 0;
    bool reuse_ring = false;
    ReadRing* ring = nullptr;
    std::vector<ReadSlot>* slots = nullptr;
    std::vector<size_t>* free_slots = nullptr;
    RangeCompletion complete = nullptr;
    void* complete_context = nullptr;
    IoUringStats* stats = nullptr;
    size_t next_range = 0;
    size_t outstanding = 0;
    bool collect_diagnostics = false;
};

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

struct RangeWorkerContext {
    int descriptor = -1;
    const std::vector<ByteRange>* ranges = nullptr;
    const std::vector<size_t>* partition_bounds = nullptr;
    size_t worker_count = 0;
    size_t queue_depth = 0;
    RangeCompletion complete = nullptr;
    void* complete_context = nullptr;
    std::vector<IoUringStats>* stats = nullptr;
    std::vector<std::string>* reasons = nullptr;
    std::vector<std::exception_ptr>* errors = nullptr;
    std::vector<uint8_t>* used = nullptr;
    CooperativeWork work = nullptr;
    void* work_context = nullptr;
    bool collect_diagnostics = false;
};

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
