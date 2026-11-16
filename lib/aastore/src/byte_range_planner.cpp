#include "byte_range_planner.hpp"

#include "aastore/store.hpp"
#include "thread_helpers.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <thread>
#include <utility>

namespace aastore {
namespace {

uint64_t packed_bytes_for_residues(uint64_t residues) {
    return (residues * 5 + 7) / 8;
}

uint64_t saturating_add(uint64_t left, uint64_t right) {
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

template <typename Function>
void run_parallel(size_t worker_count, Function function) {
    std::vector<std::thread> workers;
    ThreadJoinGuard join_guard(workers);
    workers.reserve(worker_count - 1);
    for (size_t worker = 1; worker < worker_count; ++worker) {
        workers.emplace_back([&function, worker] { function(worker); });
    }
    function(size_t{0});
    join_guard.join_all();
}

template <typename Context>
void run_parallel(size_t worker_count,
                  void (*function)(Context*, size_t),
                  Context* context) {
    run_parallel(worker_count, [function, context](size_t worker) {
        function(context, worker);
    });
}

void summarize_ranges(ReadPlan& plan) {
    plan.read_bytes = 0;
    plan.max_range_bytes = 0;
    for (const ByteRange& range : plan.ranges) {
        const uint64_t bytes = range.end - range.begin;
        plan.read_bytes = saturating_add(plan.read_bytes, bytes);
        plan.max_range_bytes = std::max(plan.max_range_bytes, bytes);
    }
}

ReadPlan plan_sparse_ranges_serial(const SequenceRequests& requests,
                                   const ReadOptions& options) {
    ReadPlan plan;
    plan.ranges.reserve(std::min<size_t>(requests.size(), 65536));
    uint64_t covered_end = 0;
    bool have_coverage = false;

    for (size_t i = 0; i < requests.size(); ++i) {
        const SequenceRequest& request = requests[i];
        plan.requested_residues += request.length;
        if (request.length == 0) continue;
        const PackedByteExtent extent = packed_byte_extent(request);

        if (options.collect_diagnostics &&
            (!have_coverage || extent.end > covered_end)) {
            const uint64_t uncovered_begin = have_coverage
                ? std::max(extent.begin, covered_end)
                : extent.begin;
            plan.unique_packed_bytes += extent.end - uncovered_begin;
            covered_end = extent.end;
            have_coverage = true;
        }

        const bool overlaps = !plan.ranges.empty() &&
            extent.begin <= plan.ranges.back().end;
        const uint64_t gap = plan.ranges.empty() || overlaps
            ? 0
            : extent.begin - plan.ranges.back().end;
        const uint64_t merged_end = plan.ranges.empty()
            ? extent.end
            : std::max(plan.ranges.back().end, extent.end);
        const bool contained = !plan.ranges.empty() &&
            merged_end == plan.ranges.back().end;
        const bool free_merge = !plan.ranges.empty() &&
            (overlaps || extent.begin == plan.ranges.back().end);
        const uint64_t max_range_bytes = free_merge
            ? options.max_exact_merged_range_bytes
            : options.max_merged_range_bytes;
        const bool within_size_limit = plan.ranges.empty() || contained ||
            max_range_bytes == 0 ||
            merged_end - plan.ranges.back().begin <= max_range_bytes;
        if (!plan.ranges.empty() && within_size_limit &&
            (free_merge || gap <= options.merge_gap_bytes)) {
            ByteRange& range = plan.ranges.back();
            if (options.collect_diagnostics) {
                if (free_merge) ++plan.free_merges;
                if (gap != 0) ++plan.merged_gaps;
            }
            range.end = merged_end;
            range.request_end = i + 1;
        } else {
            plan.ranges.push_back(
                ByteRange{extent.begin, extent.end, i, i + 1});
        }
    }
    return plan;
}

struct PlannerExtent {
    uint64_t begin;
    uint64_t end;
};

using PlannerExtents =
    std::vector<PlannerExtent, NoInitAllocator<PlannerExtent>>;
using RequestIndices =
    std::vector<size_t, NoInitAllocator<size_t>>;

ReadPlan plan_sparse_ranges_parallel(const SequenceRequests& requests,
                                     const ReadOptions& options,
                                     size_t worker_count) {
    ReadPlan plan;
    worker_count = std::min(worker_count, requests.size());
    std::vector<size_t> valid_counts(worker_count, 0);
    std::vector<uint64_t> residue_totals(worker_count, 0);
    run_parallel(worker_count, [&](size_t worker) {
        const size_t begin =
            requests.size() * worker / worker_count;
        const size_t end =
            requests.size() * (worker + 1) / worker_count;
        size_t valid = 0;
        uint64_t residues = 0;
        for (size_t i = begin; i < end; ++i) {
            residues = saturating_add(residues, requests[i].length);
            valid += requests[i].length != 0;
        }
        valid_counts[worker] = valid;
        residue_totals[worker] = residues;
    });

    std::vector<size_t> valid_bases(worker_count, 0);
    size_t valid_count = 0;
    for (size_t worker = 0; worker < worker_count; ++worker) {
        valid_bases[worker] = valid_count;
        valid_count += valid_counts[worker];
        plan.requested_residues = saturating_add(
            plan.requested_residues, residue_totals[worker]);
    }
    if (valid_count == 0) return plan;

    PlannerExtents extents(valid_count);
    RequestIndices request_indices;
    if (valid_count != requests.size()) {
        request_indices.resize(valid_count);
    }
    run_parallel(worker_count, [&](size_t worker) {
        const size_t begin =
            requests.size() * worker / worker_count;
        const size_t end =
            requests.size() * (worker + 1) / worker_count;
        size_t output = valid_bases[worker];
        for (size_t i = begin; i < end; ++i) {
            const SequenceRequest& request = requests[i];
            if (request.length == 0) continue;
            const PackedByteExtent extent = packed_byte_extent(request);
            extents[output] = PlannerExtent{extent.begin, extent.end};
            if (!request_indices.empty()) request_indices[output] = i;
            ++output;
        }
    });

    worker_count = std::min(worker_count, valid_count);
    if (options.collect_diagnostics) {
        std::vector<uint64_t> unique_totals(worker_count, 0);
        run_parallel(worker_count, [&](size_t worker) {
            const size_t begin = valid_count * worker / worker_count;
            const size_t end = valid_count * (worker + 1) / worker_count;
            uint64_t unique = 0;
            for (size_t i = begin; i < end; ++i) {
                const PlannerExtent& extent = extents[i];
                if (i == 0) {
                    unique = extent.end - extent.begin;
                    continue;
                }
                const uint64_t previous_end = extents[i - 1].end;
                if (extent.end > previous_end) {
                    unique = saturating_add(
                        unique,
                        extent.end - std::max(extent.begin, previous_end));
                }
            }
            unique_totals[worker] = unique;
        });

        for (size_t worker = 0; worker < worker_count; ++worker) {
            plan.unique_packed_bytes = saturating_add(
                plan.unique_packed_bytes, unique_totals[worker]);
        }
    }

    plan.ranges.reserve(std::min<size_t>(valid_count, 65536));
    for (size_t i = 0; i < valid_count; ++i) {
        const PlannerExtent& extent = extents[i];
        const size_t request_index = request_indices.empty()
            ? i
            : request_indices[i];
        if (plan.ranges.empty()) {
            plan.ranges.push_back(ByteRange{
                extent.begin, extent.end, request_index, request_index + 1});
            continue;
        }

        ByteRange& previous = plan.ranges.back();
        const bool overlaps = extent.begin <= previous.end;
        const uint64_t gap = overlaps ? 0 : extent.begin - previous.end;
        const uint64_t merged_end = std::max(previous.end, extent.end);
        const bool contained = merged_end == previous.end;
        const bool free_merge = overlaps || extent.begin == previous.end;
        const uint64_t max_range_bytes = free_merge
            ? options.max_exact_merged_range_bytes
            : options.max_merged_range_bytes;
        const bool within_size_limit = contained || max_range_bytes == 0 ||
            merged_end - previous.begin <= max_range_bytes;
        if (within_size_limit &&
            (free_merge || gap <= options.merge_gap_bytes)) {
            if (options.collect_diagnostics) {
                if (free_merge) ++plan.free_merges;
                if (gap != 0) ++plan.merged_gaps;
            }
            previous.end = merged_end;
            previous.request_end = request_index + 1;
        } else {
            plan.ranges.push_back(ByteRange{
                extent.begin, extent.end, request_index, request_index + 1});
        }
    }
    return plan;
}

constexpr unsigned kRadixBits = 11;
constexpr size_t kRadix = size_t{1} << kRadixBits;
constexpr uint32_t kRadixMask = static_cast<uint32_t>(kRadix - 1);

struct CompactKey {
    uint32_t sequence_id;
    uint32_t request_index;
};

static_assert(sizeof(CompactKey) == 8);

using RadixCounts = std::vector<std::array<size_t, kRadix>>;

struct RadixContext {
    SequenceRequests* requests = nullptr;
    SequenceRequests* scratch = nullptr;
    SequenceRequests* sorted_requests = nullptr;
    std::vector<CompactKey>* keys = nullptr;
    std::vector<CompactKey>* scratch_keys = nullptr;
    RadixCounts* counts = nullptr;
    RadixCounts* positions = nullptr;
    size_t thread_count = 1;
    unsigned shift = 0;
    bool compact = false;
    bool final_pass = false;
};

bool sequence_request_less(const SequenceRequest& left,
                           const SequenceRequest& right) {
    return left.sequence_id < right.sequence_id;
}

void count_radix_buckets(RadixContext* ctx, size_t worker) {
    std::array<size_t, kRadix>& local = (*ctx->counts)[worker];
    local.fill(0);
    const size_t begin =
        ctx->requests->size() * worker / ctx->thread_count;
    const size_t end =
        ctx->requests->size() * (worker + 1) / ctx->thread_count;
    for (size_t i = begin; i < end; ++i) {
        const uint32_t key = ctx->compact
            ? (*ctx->keys)[i].sequence_id
            : static_cast<uint32_t>((*ctx->requests)[i].sequence_id);
        ++local[(key >> ctx->shift) & kRadixMask];
    }
}

void compute_radix_positions(RadixContext* ctx) {
    size_t sum = 0;
    for (size_t bucket = 0; bucket < kRadix; ++bucket) {
        size_t bucket_total = 0;
        for (size_t worker = 0; worker < ctx->thread_count; ++worker) {
            (*ctx->positions)[worker][bucket] = sum + bucket_total;
            bucket_total += (*ctx->counts)[worker][bucket];
        }
        sum += bucket_total;
    }
}

void initialize_compact_keys(RadixContext* ctx, size_t worker) {
    const size_t begin =
        ctx->requests->size() * worker / ctx->thread_count;
    const size_t end =
        ctx->requests->size() * (worker + 1) / ctx->thread_count;
    for (size_t i = begin; i < end; ++i) {
        (*ctx->keys)[i] = CompactKey{
            static_cast<uint32_t>((*ctx->requests)[i].sequence_id),
            static_cast<uint32_t>(i)};
    }
}

void scatter_compact_keys(RadixContext* ctx, size_t worker) {
    std::array<size_t, kRadix>& local = (*ctx->positions)[worker];
    const size_t begin =
        ctx->requests->size() * worker / ctx->thread_count;
    const size_t end =
        ctx->requests->size() * (worker + 1) / ctx->thread_count;
    for (size_t i = begin; i < end; ++i) {
        const CompactKey key = (*ctx->keys)[i];
        const size_t bucket =
            (key.sequence_id >> ctx->shift) & kRadixMask;
        const size_t output = local[bucket]++;
        if (ctx->final_pass) {
            (*ctx->sorted_requests)[output] =
                (*ctx->requests)[key.request_index];
        } else {
            (*ctx->scratch_keys)[output] = key;
        }
    }
}

void scatter_sequence_requests(RadixContext* ctx, size_t worker) {
    std::array<size_t, kRadix>& local = (*ctx->positions)[worker];
    const size_t begin =
        ctx->requests->size() * worker / ctx->thread_count;
    const size_t end =
        ctx->requests->size() * (worker + 1) / ctx->thread_count;
    for (size_t i = begin; i < end; ++i) {
        const SequenceRequest request = (*ctx->requests)[i];
        const size_t bucket =
            (static_cast<uint32_t>(request.sequence_id) >> ctx->shift) &
            kRadixMask;
        (*ctx->scratch)[local[bucket]++] = request;
    }
}

}  // namespace

PackedByteExtent packed_byte_extent(const SequenceRequest& request) {
    if (request.length == 0) return {};
    const uint64_t aligned_start = request.start & ~uint64_t{7};
    const uint64_t end_residue = request.start + request.length;
    return {aligned_start / 8 * 5,
            packed_bytes_for_residues(end_residue)};
}

void sort_requests_by_sequence_id(SequenceRequests& requests,
                                  size_t thread_count,
                                  uint64_t sequence_count) {
    if (requests.size() <= 1) return;
    if (sequence_count == 0 || sequence_count - 1 >
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        std::sort(requests.begin(), requests.end(), sequence_request_less);
        return;
    }

    thread_count = requests.size() >= 65536
        ? std::min(std::max<size_t>(1, thread_count), requests.size())
        : size_t{1};
    RadixCounts counts(thread_count);
    RadixCounts positions(thread_count);
    RadixContext ctx;
    ctx.requests = &requests;
    ctx.counts = &counts;
    ctx.positions = &positions;
    ctx.thread_count = thread_count;
    const bool use_compact_keys = requests.size() >= 4000000 &&
        requests.size() <= static_cast<size_t>(
            std::numeric_limits<uint32_t>::max());
    if (use_compact_keys) {
        std::vector<CompactKey> keys(requests.size());
        std::vector<CompactKey> scratch_keys(requests.size());
        SequenceRequests sorted_requests(requests.size());
        ctx.keys = &keys;
        ctx.scratch_keys = &scratch_keys;
        ctx.sorted_requests = &sorted_requests;
        ctx.compact = true;
        run_parallel(thread_count, initialize_compact_keys, &ctx);
        for (unsigned shift = 0; shift < 32; shift += kRadixBits) {
            ctx.shift = shift;
            ctx.final_pass = shift + kRadixBits >= 32;
            run_parallel(thread_count, count_radix_buckets, &ctx);
            compute_radix_positions(&ctx);
            run_parallel(thread_count, scatter_compact_keys, &ctx);
            if (!ctx.final_pass) keys.swap(scratch_keys);
        }
        requests.swap(sorted_requests);
        return;
    }

    SequenceRequests scratch(requests.size());
    ctx.scratch = &scratch;
    ctx.compact = false;
    for (unsigned shift = 0; shift < 32; shift += kRadixBits) {
        ctx.shift = shift;
        run_parallel(thread_count, count_radix_buckets, &ctx);
        compute_radix_positions(&ctx);
        run_parallel(thread_count, scatter_sequence_requests, &ctx);
        requests.swap(scratch);
    }
}

ReadPlan plan_byte_ranges(const SequenceRequests& requests,
                          const ReadOptions& options) {
    const size_t worker_count = requests.size() >= 65536
        ? std::min(std::max<size_t>(1, options.threads), requests.size())
        : size_t{1};
    ReadPlan plan = worker_count == 1
        ? plan_sparse_ranges_serial(requests, options)
        : plan_sparse_ranges_parallel(requests, options, worker_count);

    summarize_ranges(plan);
    return plan;
}

}  // namespace aastore
