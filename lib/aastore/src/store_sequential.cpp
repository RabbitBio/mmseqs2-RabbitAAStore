// 文件职责：读取连续 ID 区间，按残基位置分片并直接解码到 Batch 或逐序列输出区。
// packed 暂存缓冲由内部有界复用，从常驻数据或连续 pread 取得编码数据。
#include "store_io.hpp"

#include "metadata.hpp"
#include "read_timer.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <vector>

namespace aastore {
namespace {

// 计算线程的输出边界，并对齐到 8 个 residue 的编码边界。
uint64_t sequential_worker_boundary(uint64_t residue_begin,
                                    uint64_t residue_count,
                                    size_t worker_count,
                                    size_t worker) {
    if (worker == 0) return 0;
    if (worker == worker_count) return residue_count;
    const uint64_t quotient = residue_count / worker_count;
    const uint64_t remainder = residue_count % worker_count;
    const uint64_t approximate = quotient * worker +
        std::min<uint64_t>(worker, remainder);
    const uint64_t absolute = residue_begin + approximate;
    return (absolute & ~uint64_t{7}) - residue_begin;
}

// 连续 ID 读取的整体字符区间及分片规模，不存放序列数据或文件资源。
struct SequentialReadState {
    // 第一条请求序列在全库字符流中的起点。
    uint64_t residue_begin = 0;
    // 整个请求区间的字符总数，也是无分隔符输出区的字节数。
    uint64_t residue_count = 0;
    // 根据数据量和线程预算选定的分片数；没有字符时保持为零。
    size_t worker_count = 0;
};

using PackedBuffer = std::vector<uint8_t, NoInitAllocator<uint8_t>>;
constexpr size_t kRetainedSequentialBytes = 64ULL * 1024ULL * 1024ULL;
// 调用线程持有暂存区，各工作线程只使用自己的槽位；全部 join 后才允许下一次复用。
thread_local std::vector<PackedBuffer> retained_buffers;

// 校验连续 ID 区间并建立无分隔符的连续 residue 输出布局。
void prepare_sequential_layout(const Metadata* metadata,
                               uint64_t sequence_count,
                               StorageMode storage_mode,
                               SequenceId first_sequence,
                               SequenceId end_sequence,
                               const ReadOptions* options,
                               Batch* batch,
                               char* const* outputs,
                               SequentialReadState* state) {
    if (first_sequence > end_sequence ||
        end_sequence > sequence_count) {
        throw std::out_of_range(
            "sequential sequence range out of range");
    }

    const DiagnosticTimer layout_timer(options->collect_diagnostics);
    batch->stats = {};
    batch->stats.diagnostics_collected = options->collect_diagnostics;
    const uint64_t count_u64 = end_sequence - first_sequence;
    if (count_u64 > static_cast<uint64_t>(
            std::numeric_limits<size_t>::max())) {
        throw std::length_error(
            "sequential sequence count exceeds address space");
    }
    const size_t count = static_cast<size_t>(count_u64);
    batch->offsets.resize(count);
    batch->lengths.resize(count);
    batch->stats.full_memory = storage_mode == StorageMode::FullMemory;
    batch->stats.prefix_memory = storage_mode == StorageMode::PartialMemory;
    batch->stats.requested_sequences = count;

    state->residue_begin = metadata->start_position(first_sequence);
    const uint64_t residue_end = metadata->start_position(end_sequence);
    state->residue_count = residue_end - state->residue_begin;
    batch->stats.requested_residues = state->residue_count;
    uint64_t previous = state->residue_begin;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t next = metadata->start_position(
            first_sequence + static_cast<SequenceId>(i) + 1);
        batch->offsets[i] = previous - state->residue_begin;
        batch->lengths[i] = next - previous;
        if (outputs != nullptr && next != previous && outputs[i] == nullptr) {
            throw std::invalid_argument("sequential output is null for a nonempty sequence");
        }
        previous = next;
    }
    if (previous != residue_end) {
        throw std::runtime_error("inconsistent sequential metadata");
    }
    if (state->residue_count > static_cast<uint64_t>(
            std::numeric_limits<size_t>::max())) {
        throw std::length_error(
            "sequential output exceeds address space");
    }
    if (outputs == nullptr) {
        batch->storage.resize_uninitialized(
            static_cast<size_t>(state->residue_count));
    }
    batch->stats.layout_ns = layout_timer.elapsed_ns();
    if (state->residue_count == 0) return;

    const size_t requested_workers = std::max<size_t>(1, options->threads);
    constexpr uint64_t kMinResiduesPerWorker = 4ULL * 1024ULL * 1024ULL;
    const size_t useful_workers = static_cast<size_t>(
        (state->residue_count + kMinResiduesPerWorker - 1) /
        kMinResiduesPerWorker);
    state->worker_count = std::max<size_t>(
        1, std::min(requested_workers, useful_workers));
}

// 复用调用线程的 packed 缓冲，总保留容量不超过 64 MiB；更大读取使用临时缓冲。
std::vector<PackedBuffer>& prepare_sequential_buffers(
        const StoreReadContext& store,
        const SequentialReadState& state,
        std::vector<PackedBuffer>& temporary) {
    std::vector<size_t> sizes(state.worker_count, 0);
    uint64_t total_bytes = 0;
    for (size_t worker = 0; worker < state.worker_count; ++worker) {
        const uint64_t begin = sequential_worker_boundary(
            state.residue_begin, state.residue_count, state.worker_count, worker);
        const uint64_t end = sequential_worker_boundary(
            state.residue_begin, state.residue_count, state.worker_count, worker + 1);
        if (begin == end) continue;
        const PackedByteExtent extent = packed_byte_extent(
            SequenceRequest{0, 0, state.residue_begin + begin, end - begin});
        if (store.storage_mode == StorageMode::FullMemory ||
            (store.storage_mode == StorageMode::PartialMemory &&
             extent.end <= store.prefix_packed_bytes)) {
            continue;
        }
        const uint64_t bytes = extent.end - extent.begin;
        if (bytes > std::numeric_limits<size_t>::max()) {
            throw std::length_error("sequential packed range exceeds address space");
        }
        sizes[worker] = static_cast<size_t>(bytes);
        total_bytes += bytes;
    }
    if (total_bytes == 0) return temporary;

    auto& buffers = total_bytes <= kRetainedSequentialBytes
        ? retained_buffers : temporary;
    buffers.resize(state.worker_count);
    if (&buffers == &retained_buffers) {
        uint64_t capacity = 0;
        for (size_t worker = 0; worker < state.worker_count; ++worker) {
            capacity += std::max(buffers[worker].capacity(), sizes[worker]);
        }
        if (capacity > kRetainedSequentialBytes) {
            buffers = std::vector<PackedBuffer>(state.worker_count);
        }
    }
    for (size_t worker = 0; worker < state.worker_count; ++worker) {
        if (sizes[worker] > buffers[worker].capacity()) {
            PackedBuffer buffer(sizes[worker]);
            buffers[worker].swap(buffer);
        }
        buffers[worker].resize(sizes[worker]);
    }
    return buffers;
}

// 将已读分片与序列边界相交，直接解码到逐序列输出地址；长序列可跨线程分片。
void decode_sequential_outputs(const SequentialReadState& state,
                               const Batch& batch,
                               const SequenceRequest& request,
                               const uint8_t* packed,
                               uint64_t packed_begin,
                               uint64_t packed_end,
                               char* const* outputs) {
    uint64_t position = request.start - state.residue_begin;
    const uint64_t end = position + request.length;
    const auto first = std::upper_bound(
        batch.offsets.begin(), batch.offsets.end(), position);
    size_t index = static_cast<size_t>(first - batch.offsets.begin() - 1);
    while (position < end) {
        const uint64_t sequence_begin = batch.offsets[index];
        const uint64_t sequence_end = sequence_begin + batch.lengths[index];
        const uint64_t next = std::min(end, sequence_end);
        if (position < next) {
            const SequenceRequest part{
                0, 0, state.residue_begin + position, next - position};
            decode_request_from_extent(
                part, packed, packed_begin, packed_end,
                outputs[index] + static_cast<size_t>(position - sequence_begin), false);
            position = next;
        }
        ++index;
    }
}

// 汇总顺序读取各线程的读取、解码和范围统计。
void publish_sequential_stats(
        const SequentialReadState* state,
        uint64_t worker_wall_ns,
        const std::vector<uint64_t>* read_ns,
        const std::vector<uint64_t>* decode_ns,
        const std::vector<uint64_t>* read_bytes,
        BatchStats* stats) {
    stats->workers = state->worker_count;
    stats->io_workers = state->worker_count;
    stats->decode_workers = state->worker_count;
    stats->ranges = state->worker_count;
    stats->worker_wall_ns = worker_wall_ns;
    for (size_t worker = 0; worker < state->worker_count; ++worker) {
        if (stats->diagnostics_collected) {
            stats->read_ns += (*read_ns)[worker];
            stats->decode_ns += (*decode_ns)[worker];
        }
        stats->read_bytes += (*read_bytes)[worker];
        stats->max_range_bytes = std::max(
            stats->max_range_bytes, (*read_bytes)[worker]);
    }
    if (stats->diagnostics_collected) {
        const SequenceRequest full_request{
            0, 0, state->residue_begin, state->residue_count};
        const PackedByteExtent full_extent = packed_byte_extent(full_request);
        stats->unique_packed_bytes = full_extent.end - full_extent.begin;
        stats->overread_bytes =
            stats->read_bytes > stats->unique_packed_bytes
                ? stats->read_bytes - stats->unique_packed_bytes
                : 0;
    }
}

}  // namespace

// 准备连续输出布局，各线程读取并解码自己的残基分片，最后汇总统计与异常。
void sequential_get_from_store(const StoreReadContext& store,
                               SequenceId first_sequence,
                               SequenceId end_sequence,
                               const ReadOptions& options,
                               Batch& batch,
                               char* const* outputs) {
    SequentialReadState state;
    prepare_sequential_layout(
        store.metadata, store.metadata->sequence_count(),
        store.storage_mode, first_sequence, end_sequence,
        &options, &batch, outputs, &state);
    if (state.residue_count == 0) return;

    const bool diagnostics = options.collect_diagnostics;
    const DiagnosticTimer buffers_timer(diagnostics);
    std::vector<PackedBuffer> temporary_buffers;
    auto& buffers = prepare_sequential_buffers(store, state, temporary_buffers);
    batch.stats.layout_ns += buffers_timer.elapsed_ns();
    std::vector<uint64_t> read_ns(diagnostics ? state.worker_count : 0, 0);
    std::vector<uint64_t> decode_ns(diagnostics ? state.worker_count : 0, 0);
    std::vector<uint64_t> read_bytes(state.worker_count, 0);
    std::vector<std::exception_ptr> errors(state.worker_count);

    const DiagnosticTimer worker_timer(diagnostics);
    run_indexed_workers(state.worker_count, [&](size_t worker) noexcept {
        try {
            const uint64_t output_begin = sequential_worker_boundary(
                state.residue_begin, state.residue_count,
                state.worker_count, worker);
            const uint64_t output_end = sequential_worker_boundary(
                state.residue_begin, state.residue_count,
                state.worker_count, worker + 1);
            if (output_begin == output_end) return;
            const SequenceRequest request{
                first_sequence, 0, state.residue_begin + output_begin,
                output_end - output_begin};
            const PackedByteExtent extent = packed_byte_extent(request);

            const uint8_t* packed = nullptr;
            uint64_t packed_begin = 0;
            uint64_t packed_end = 0;
            if (store.storage_mode == StorageMode::FullMemory) {
                if (extent.end > store.full_packed_bytes) {
                    throw std::runtime_error(
                        "sequential range escaped full memory");
                }
                packed = store.full_packed;
                packed_end = store.full_packed_bytes;
            } else if (store.storage_mode == StorageMode::PartialMemory &&
                       extent.end <= store.prefix_packed_bytes) {
                packed = store.prefix_packed;
                packed_end = store.prefix_packed_bytes;
            } else {
                const uint64_t bytes = extent.end - extent.begin;
                if (bytes > static_cast<uint64_t>(
                        std::numeric_limits<size_t>::max())) {
                    throw std::length_error(
                        "sequential packed range exceeds address space");
                }
                PackedBuffer& local_packed = buffers[worker];
                const DiagnosticTimer read_timer(diagnostics);
                pread_exact(store.packed_descriptor, local_packed.data(),
                            bytes, extent.begin);
                if (diagnostics) read_ns[worker] = read_timer.elapsed_ns();
                read_bytes[worker] = bytes;
                packed = local_packed.data();
                packed_begin = extent.begin;
                packed_end = extent.end;
            }

            const DiagnosticTimer decode_timer(diagnostics);
            if (outputs == nullptr) {
                decode_request_from_extent(
                    request, packed, packed_begin, packed_end,
                    batch.storage.data() + output_begin, false);
            } else {
                decode_sequential_outputs(
                    state, batch, request, packed, packed_begin, packed_end, outputs);
            }
            if (diagnostics) decode_ns[worker] = decode_timer.elapsed_ns();
        } catch (...) {
            errors[worker] = std::current_exception();
        }
    });
    const uint64_t worker_wall_ns = worker_timer.elapsed_ns();
    rethrow_worker_errors(errors);
    publish_sequential_stats(
        &state, worker_wall_ns, &read_ns, &decode_ns, &read_bytes,
        &batch.stats);
}

}  // namespace aastore
