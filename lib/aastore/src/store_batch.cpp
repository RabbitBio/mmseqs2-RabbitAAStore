// 文件职责：实现随机批量读取的布局、排序、重复请求合并、前缀分流与结果回填。
// 调度 FullMemory、pread 或 io_uring 路径；后者将有限前缀解码与 CQ 处理交错执行。
#include "store_io.hpp"

#include "byte_range_planner.hpp"
#include "io_uring_backend.hpp"
#include "metadata.hpp"
#include "read_timer.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace aastore {
namespace {

// PartialMemory 中命中同一条序列的一组有序请求，解码一次后复制给重复请求。
struct CacheHitGroup {
    // 完整有序请求数组中该组的起始下标。
    size_t request_begin = 0;
    // 该组末尾的下一下标，不是序列 ID。
    size_t request_end = 0;
};

// PartialMemory 中同一条未命中序列的请求组，关联一条去重后的磁盘请求。
struct CacheMissGroup {
    // 完整有序请求数组中该组的起始下标。
    size_t request_begin = 0;
    // 完整有序请求数组中该组末尾的下一下标。
    size_t request_end = 0;
    // 该组首个请求在 misses 数组中的下标，供读完后复制重复结果。
    size_t miss_index = 0;
};

// 前缀命中解码阶段的线程与耗时统计；时间均为纳秒。
struct CacheHitDecodeStats {
    // 按可用线程数与命中组数取小值得到的工作线程数。
    uint64_t workers = 0;
    // 独立执行整个前缀解码阶段的墙钟耗时；io_uring 交错路径不单独记录它。
    uint64_t wall_ns = 0;
    // 各线程实际执行前缀解码及重复结果复制的耗时之和。
    uint64_t total_ns = 0;
    // 各线程累计前缀处理耗时中的最大值，不是整个阶段的墙钟耗时。
    uint64_t max_busy_ns = 0;
};

// 批量解码共享只读状态。线程只修改各自的统计槽和输出区。
struct BatchDecodeContext {
    // 借用 Store 中从文件偏移零开始的常驻 packed 前缀。
    const uint8_t* prefix_packed = nullptr;
    // 常驻前缀的有效字节数，用于校验解码区间。
    uint64_t prefix_packed_bytes = 0;
    // 完整请求列表，布局完成后 output_position 已是输出地址或字节偏移。
    SequenceRequests* requests = nullptr;
    // 交给磁盘范围规划的请求列表，前缀模式下仅含去重后的未命中请求。
    const SequenceRequests* read_requests = nullptr;
    // 借用本次结果对象；线程写各自输出位置，统计按阶段汇总。
    Batch* batch = nullptr;
    // true 时 output_position 是调用方地址；false 时是 batch->storage 内偏移。
    bool direct_output = false;
};

// 返回调用方输出地址或请求在批次连续输出区中的写入地址。
char* batch_request_output(const BatchDecodeContext* ctx,
                           const SequenceRequest& request) {
    if (ctx->direct_output) {
        return reinterpret_cast<char*>(request.output_position);
    }
    return ctx->batch->storage.data() + request.output_position;
}

// 前缀解码任务的共享状态，线程通过原子下标领取组，不另建常驻解码线程池。
struct CacheHitDecodeContext {
    // 借用输出布局、请求和前缀数据视图。
    BatchDecodeContext* decode = nullptr;
    // 本批前缀命中组列表，执行期间不修改其内容。
    std::vector<CacheHitGroup>* groups = nullptr;
    // 独立解码前缀时允许使用的最大线程数。
    size_t maximum_workers = 1;
    // 本次前缀阶段的汇总统计，在线程完成后发布到 BatchStats。
    CacheHitDecodeStats stats;
    // 下一待领取的命中组下标，所有线程用 fetch_add 取得不重复的任务。
    std::atomic<size_t> next_group{0};
    // 逐线程累计的前缀处理纳秒数，诊断关闭时为空。
    std::vector<uint64_t> decode_ns;
};

// 动态领取前缀命中组，解码并复制重复结果，达到字节或组数预算后让出执行权。
// 单组不拆分；确认没有更多组可领取时返回 false，否则保留后续调用机会。
bool decode_cache_hit_slice(void* opaque, size_t worker,
                            uint64_t byte_budget, size_t group_budget) {
    auto* ctx = static_cast<CacheHitDecodeContext*>(opaque);
    if (ctx->groups->empty()) return false;
    const bool diagnostics = ctx->decode->batch->stats.diagnostics_collected;
    const DiagnosticTimer decode_timer(diagnostics);
    bool more = true;
    while (byte_budget != 0 && group_budget-- != 0) {
        const size_t group_index = ctx->next_group.fetch_add(
            1, std::memory_order_relaxed);
        if (group_index >= ctx->groups->size()) {
            more = false;
            break;
        }
        const CacheHitGroup& group = (*ctx->groups)[group_index];
        const SequenceRequest& first =
            (*ctx->decode->requests)[group.request_begin];
        char* first_output = batch_request_output(ctx->decode, first);
        decode_request_from_extent(
            first, ctx->decode->prefix_packed, 0,
            ctx->decode->prefix_packed_bytes, first_output,
            !ctx->decode->direct_output);
        for (size_t i = group.request_begin + 1;
             i < group.request_end; ++i) {
            std::memcpy(
                batch_request_output(ctx->decode, (*ctx->decode->requests)[i]),
                first_output,
                static_cast<size_t>(first.length) +
                    (ctx->decode->direct_output ? 0 : 1));
        }
        const uint64_t references = group.request_end - group.request_begin;
        // 输出布局已校验包含重复请求的总大小，此处累计本组实际输出字节。
        const uint64_t output_bytes =
            (first.length + (ctx->decode->direct_output ? 0 : 1)) * references;
        byte_budget -= std::min(byte_budget, output_bytes);
    }
    if (diagnostics) ctx->decode_ns[worker] += decode_timer.elapsed_ns();
    return more;
}

// 不限制单轮工作量，持续领取前缀命中组，供全命中和 pread 前缀阶段使用。
void decode_cache_hit_worker(void* opaque, size_t worker) {
    decode_cache_hit_slice(opaque, worker,
                          std::numeric_limits<uint64_t>::max(),
                          std::numeric_limits<size_t>::max());
}

// 供 ring 所属线程处理约 256 KiB 输出或 256 个命中组，再返回 I/O 循环检查 CQ。
bool decode_cache_hit_work(void* opaque, size_t worker) {
    return decode_cache_hit_slice(opaque, worker, 256 * 1024, 256);
}

// 重置命中组领取位置，并按工作线程数量初始化解码计时槽位。
void prepare_cache_hit_decode(CacheHitDecodeContext* ctx, size_t workers) {
    ctx->next_group.store(0, std::memory_order_relaxed);
    ctx->decode_ns.assign(
        ctx->decode->batch->stats.diagnostics_collected ? workers : 0, 0);
    ctx->stats = {};
    ctx->stats.workers = std::min(workers, ctx->groups->size());
}

// 汇总各线程的前缀解码耗时，记录累计工作量与最忙线程耗时。
void finish_cache_hit_decode(CacheHitDecodeContext* ctx) {
    for (uint64_t value : ctx->decode_ns) {
        ctx->stats.total_ns += value;
        ctx->stats.max_busy_ns = std::max(ctx->stats.max_busy_ns, value);
    }
}

// 按命中组数量选择线程数并完成全部缓存命中解码。
void decode_cache_hits(CacheHitDecodeContext* ctx) {
    if (ctx->groups->empty()) return;
    const size_t worker_count =
        std::min(ctx->maximum_workers, ctx->groups->size());
    prepare_cache_hit_decode(ctx, worker_count);
    std::vector<std::exception_ptr> errors(worker_count);

    const DiagnosticTimer wall_timer(ctx->decode->batch->stats.diagnostics_collected);
    run_indexed_workers(worker_count, [&](size_t worker) noexcept {
        try {
            decode_cache_hit_worker(ctx, worker);
        } catch (...) {
            errors[worker] = std::current_exception();
        }
    });
    ctx->stats.wall_ns = wall_timer.elapsed_ns();
    rethrow_worker_errors(errors);
    finish_cache_hit_decode(ctx);
}

// 把缓存命中阶段的并行耗时与工作线程数发布到批次统计。
void publish_cache_hit_decode(BatchStats* target,
                              const CacheHitDecodeStats& source,
                              bool publish_wall) {
    target->workers = std::max(target->workers, source.workers);
    if (!target->diagnostics_collected) return;
    if (publish_wall) target->worker_wall_ns += source.wall_ns;
    target->cache_decode_ns += source.total_ns;
    target->decode_ns += source.total_ns;
    target->worker_max_busy_ns =
        std::max(target->worker_max_busy_ns, source.max_busy_ns);
}

// 并行散播缓存未命中组中已经解码完成的首份结果。
void scatter_cache_misses(BatchDecodeContext* decode,
                          const std::vector<CacheMissGroup>& groups,
                          const SequenceRequests& misses,
                          size_t maximum_workers) {
    if (groups.empty()) return;
    const size_t worker_count = std::min(maximum_workers, groups.size());
    std::vector<std::exception_ptr> errors(worker_count);
    std::atomic<size_t> next_group{0};
    run_indexed_workers(worker_count, [&](size_t worker) noexcept {
        try {
            while (true) {
                const size_t group_index = next_group.fetch_add(
                    1, std::memory_order_relaxed);
                if (group_index >= groups.size()) break;
                const CacheMissGroup& group = groups[group_index];
                const SequenceRequest& miss = misses[group.miss_index];
                const char* first_output =
                    batch_request_output(decode, miss);
                for (size_t i = group.request_begin + 1;
                     i < group.request_end; ++i) {
                    std::memcpy(
                        batch_request_output(
                            decode, (*decode->requests)[i]),
                        first_output,
                        static_cast<size_t>(miss.length) +
                            (decode->direct_output ? 0 : 1));
                }
            }
        } catch (...) {
            errors[worker] = std::current_exception();
        }
    });
    rethrow_worker_errors(errors);
}

// io_uring 范围完成回调借用的解码状态，将已读范围映射回其中的序列请求。
struct UringDecodeContext {
    // 输出位置和请求列表，由本次 batch 调用持有。
    BatchDecodeContext* decode = nullptr;
    // 完成事件中的 range_index 所对应的有序读取计划。
    const ReadPlan* plan = nullptr;
    // 借用的逐线程解码耗时数组，诊断开启时由各 ring 所属线程写自己的项。
    std::vector<uint64_t>* decode_ns = nullptr;
};

// 在 io_uring 范围完成时解码该范围内请求。
void decode_uring_range(void* opaque, size_t worker, size_t range_index,
                        const uint8_t* packed, uint64_t range_bytes) {
    UringDecodeContext* ctx = static_cast<UringDecodeContext*>(opaque);
    const ByteRange& range = ctx->plan->ranges[range_index];
    if (range_bytes != range.end - range.begin) {
        throw std::runtime_error("io_uring range size mismatch");
    }
    const bool diagnostics = ctx->decode->batch->stats.diagnostics_collected;
    const DiagnosticTimer decode_timer(diagnostics);
    for (size_t i = range.request_begin; i < range.request_end; ++i) {
        const SequenceRequest& request =
            (*ctx->decode->read_requests)[i];
        decode_request_from_extent(
            request, packed, range.begin, range.end,
            batch_request_output(ctx->decode, request),
            !ctx->decode->direct_output);
    }
    if (diagnostics) (*ctx->decode_ns)[worker] += decode_timer.elapsed_ns();
}

// 一个 pread 工作线程的阶段耗时，结束后统一汇总，单位纳秒。
struct PreadWorkerStats {
    // 本线程在定位读取函数中花费的累计时间。
    uint64_t read_ns = 0;
    // 本线程解码已读范围的累计时间，包含其协调的并行解码等待。
    uint64_t decode_ns = 0;
};

// 随机批读的布局准备结果，连接输入 ID、输出空间和后续调度。
struct BatchLayoutState {
    // 本批请求及输出定位信息；非 FullMemory 路径按序列 ID 排序。
    SequenceRequests requests;
    // 总输出字节数；返回 Batch 时每条序列计入一个 NUL，直接输出时不计。
    uint64_t arena_bytes = 0;
    // 将 options.threads 的零值归一化后的线程预算，不一定全部启动。
    size_t threads = 1;
};

// 生成批次请求、排序物理顺序并建立连续输出布局。
void prepare_batch_layout(const Metadata* metadata,
                          uint64_t sequence_count,
                          StorageMode storage_mode,
                          const std::vector<SequenceId>* ids,
                          const ReadOptions* options,
                          Batch* batch,
                          BatchLayoutState* state,
                          char* const* outputs) {
    const DiagnosticTimer layout_timer(options->collect_diagnostics);
    batch->stats.diagnostics_collected = options->collect_diagnostics;
    const bool direct_output = outputs != nullptr;
    const uint64_t terminator_bytes = direct_output ? 0 : 1;
    state->threads = std::max<size_t>(1, options->threads);
    state->requests.resize(ids->size());
    if (!direct_output) {
        batch->offsets.resize(ids->size());
        batch->lengths.resize(ids->size());
    }
    batch->stats.full_memory = storage_mode == StorageMode::FullMemory;
    batch->stats.prefix_memory = storage_mode == StorageMode::PartialMemory;
    batch->stats.requested_sequences = ids->size();

    const size_t prepare_workers = ids->size() >= 65536
        ? std::min(state->threads, ids->size())
        : size_t{1};
    std::vector<uint64_t> chunk_totals(prepare_workers, 0);
    std::vector<std::exception_ptr> prepare_errors(prepare_workers);
    run_indexed_workers(prepare_workers, [&](size_t worker) noexcept {
        try {
            const size_t begin = ids->size() * worker / prepare_workers;
            const size_t end =
                ids->size() * (worker + 1) / prepare_workers;
            uint64_t local_total = 0;
            for (size_t i = begin; i < end; ++i) {
                const SequenceId id = (*ids)[i];
                if (id >= sequence_count) {
                    throw std::out_of_range("sequence id out of range");
                }
                const uint64_t start = metadata->start_position(id);
                const uint64_t length =
                    metadata->start_position(id + 1) - start;
                if (length > std::numeric_limits<uint64_t>::max() - terminator_bytes ||
                    local_total > std::numeric_limits<uint64_t>::max() -
                        length - terminator_bytes) {
                    throw std::length_error("batch output arena overflow");
                }
                if (direct_output) {
                    if (length != 0 && outputs[i] == nullptr) {
                        throw std::invalid_argument("batch output is null");
                    }
                } else {
                    batch->lengths[i] = length;
                }
                state->requests[i] = SequenceRequest{id, i, start, length};
                local_total += length + terminator_bytes;
            }
            chunk_totals[worker] = local_total;
        } catch (...) {
            prepare_errors[worker] = std::current_exception();
        }
    });
    rethrow_worker_errors(prepare_errors);

    for (uint64_t chunk_total : chunk_totals) {
        if (state->arena_bytes >
            std::numeric_limits<uint64_t>::max() - chunk_total) {
            throw std::length_error("batch output arena overflow");
        }
        state->arena_bytes += chunk_total;
    }
    if (!direct_output && state->arena_bytes > static_cast<uint64_t>(
            std::numeric_limits<size_t>::max())) {
        throw std::length_error(
            "batch output arena exceeds address space");
    }

    if (storage_mode != StorageMode::FullMemory) {
        const DiagnosticTimer sort_timer(options->collect_diagnostics);
        if (!options->input_sorted) {
            sort_requests_by_sequence_id(
                state->requests, state->threads, sequence_count);
        }
        batch->stats.sort_ns = sort_timer.elapsed_ns();
    }

    batch->stats.requested_residues = state->arena_bytes -
        static_cast<uint64_t>(ids->size()) * terminator_bytes;
    if (direct_output) {
        run_indexed_workers(prepare_workers, [&](size_t worker) {
            const size_t begin = ids->size() * worker / prepare_workers;
            const size_t end = ids->size() * (worker + 1) / prepare_workers;
            for (size_t i = begin; i < end; ++i) {
                SequenceRequest& request = state->requests[i];
                request.output_position = reinterpret_cast<size_t>(
                    outputs[request.output_position]);
            }
        });
        batch->stats.layout_ns = layout_timer.elapsed_ns() - batch->stats.sort_ns;
        return;
    }

    std::vector<uint64_t> physical_totals(prepare_workers, 0);
    run_indexed_workers(prepare_workers, [&](size_t worker) {
        const size_t begin =
            state->requests.size() * worker / prepare_workers;
        const size_t end =
            state->requests.size() * (worker + 1) / prepare_workers;
        uint64_t total = 0;
        for (size_t i = begin; i < end; ++i) {
            total += state->requests[i].length + 1;
        }
        physical_totals[worker] = total;
    });

    std::vector<uint64_t> physical_bases(prepare_workers, 0);
    uint64_t cursor = 0;
    for (size_t worker = 0; worker < prepare_workers; ++worker) {
        physical_bases[worker] = cursor;
        cursor += physical_totals[worker];
    }
    if (cursor != state->arena_bytes) {
        throw std::runtime_error(
            "physical output layout size mismatch");
    }
    batch->storage.resize_uninitialized(
        static_cast<size_t>(state->arena_bytes));
    run_indexed_workers(prepare_workers, [&](size_t worker) {
        const size_t begin =
            state->requests.size() * worker / prepare_workers;
        const size_t end =
            state->requests.size() * (worker + 1) / prepare_workers;
        uint64_t output_offset = physical_bases[worker];
        for (size_t i = begin; i < end; ++i) {
            SequenceRequest& request = state->requests[i];
            const size_t original_index = request.output_position;
            batch->offsets[original_index] = output_offset;
            request.output_position = static_cast<size_t>(output_offset);
            if (request.length == 0) {
                batch->storage[static_cast<size_t>(output_offset)] = '\0';
            }
            output_offset += request.length + 1;
        }
    });

    batch->stats.layout_ns =
        layout_timer.elapsed_ns() - batch->stats.sort_ns;
    batch->stats.requested_residues =
        state->arena_bytes - static_cast<uint64_t>(ids->size());
}

// 固定前缀命中组与需要从文件读取的请求。
struct BatchCacheWork {
    // 前缀未命中的去重请求，每个重复 ID 只保留首个请求供读取和解码。
    SequenceRequests misses;
    // 可直接从常驻前缀解码的请求组。
    std::vector<CacheHitGroup> hits;
    // 未命中重复组与 misses 的对应关系，读完后用于回填其他输出位置。
    std::vector<CacheMissGroup> miss_groups;
    // 实际交给范围规划的请求；前缀模式指向 misses，否则借用完整请求数组。
    const SequenceRequests* read_requests = nullptr;
};

// 按固定前缀边界把请求划分为命中组和读取组。
void prepare_batch_cache(StorageMode storage_mode,
                         uint64_t prefix_packed_bytes,
                         SequenceRequests* requests,
                         BatchStats* stats,
                         BatchCacheWork* work) {
    if (storage_mode != StorageMode::PartialMemory) {
        work->read_requests = requests;
        return;
    }

    work->misses.reserve(requests->size());
    work->hits.reserve(requests->size());
    work->miss_groups.reserve(requests->size());

    size_t begin = 0;
    while (begin < requests->size()) {
        size_t end = begin + 1;
        while (end < requests->size() &&
               (*requests)[end].sequence_id ==
                   (*requests)[begin].sequence_id) {
            ++end;
        }
        const SequenceRequest& first = (*requests)[begin];
        if (first.length != 0) {
            const PackedByteExtent extent =
                packed_byte_extent(first);
            const uint64_t references = end - begin;
            if (extent.end <= prefix_packed_bytes) {
                stats->cache_hits += references;
                if (stats->diagnostics_collected) {
                    stats->cache_hit_residues += first.length * references;
                }
                work->hits.push_back(CacheHitGroup{begin, end});
            } else {
                stats->cache_misses += references;
                if (stats->diagnostics_collected) {
                    stats->cache_miss_residues += first.length * references;
                }
                work->misses.push_back(first);
                work->miss_groups.push_back(CacheMissGroup{
                    begin, end, work->misses.size() - 1});
            }
        }
        begin = end;
    }
    work->read_requests = &work->misses;
}

// 把字节范围规划结果复制到面向调用方的批次统计中。
void publish_read_plan(BatchStats* stats,
                       const ReadPlan* plan,
                       StorageMode storage_mode) {
    if (storage_mode == StorageMode::NoMemory) {
        stats->requested_residues = plan->requested_residues;
    }
    stats->ranges = plan->ranges.size();
    stats->read_bytes = plan->read_bytes;
    stats->max_range_bytes = plan->max_range_bytes;
    if (stats->diagnostics_collected) {
        stats->unique_packed_bytes = plan->unique_packed_bytes;
        stats->merged_gaps = plan->merged_gaps;
        stats->free_merges = plan->free_merges;
        stats->overread_bytes = plan->read_bytes - plan->unique_packed_bytes;
    }
}

// 尝试使用 io_uring 执行批次读取，自动模式不可用时返回 false。
bool execute_uring_batch(int packed_descriptor,
                         const ReadOptions* options,
                         size_t threads,
                         const ReadPlan* plan,
                         BatchDecodeContext* decode,
                         CacheHitDecodeContext* cache_hits,
                         const std::vector<CacheMissGroup>* miss_groups,
                         const SequenceRequests* misses,
                         BatchStats* stats) {
    if (options->io_backend == IoBackend::Pread) return false;

    IoUringStats uring_stats;
    std::string unavailable_reason;
    const DiagnosticTimer wall_timer(options->collect_diagnostics);
    const size_t uring_workers = std::min(threads, plan->ranges.size());
    prepare_cache_hit_decode(cache_hits, uring_workers);
    size_t queue_depth = std::max<size_t>(1, options->io_queue_depth);
    if (options->io_buffer_bytes != 0 && plan->max_range_bytes != 0) {
        const uint64_t depth_from_buffer = std::max<uint64_t>(
            1, options->io_buffer_bytes /
                   plan->max_range_bytes / uring_workers);
        queue_depth = std::min<uint64_t>(queue_depth, depth_from_buffer);
    }

    std::vector<uint64_t> decode_ns(options->collect_diagnostics ? uring_workers : 0, 0);
    UringDecodeContext uring_decode_ctx;
    uring_decode_ctx.decode = decode;
    uring_decode_ctx.plan = plan;
    uring_decode_ctx.decode_ns = &decode_ns;

    const bool used_uring = read_ranges_io_uring(
        packed_descriptor, plan->ranges, uring_workers, queue_depth,
        cache_hits->groups->empty() ? nullptr : decode_cache_hit_work, cache_hits,
        decode_uring_range, &uring_decode_ctx,
        uring_stats, unavailable_reason, options->collect_diagnostics);
    if (!used_uring) {
        if (options->io_backend == IoBackend::IoUring) {
            throw std::runtime_error(
                "io_uring unavailable: " + unavailable_reason);
        }
        return false;
    }

    const uint64_t wall_ns = wall_timer.elapsed_ns();
    finish_cache_hit_decode(cache_hits);
    publish_cache_hit_decode(stats, cache_hits->stats, false);
    stats->io_uring = true;
    stats->io_workers = uring_workers;
    stats->decode_workers = uring_workers;
    stats->io_queue_depth = uring_stats.max_outstanding;
    stats->io_submissions = uring_stats.submissions;
    stats->io_completions = uring_stats.completions;
    stats->io_submit_calls = uring_stats.submit_calls;
    stats->io_submit_ns = uring_stats.submit_ns;
    stats->io_wait_calls = uring_stats.wait_calls;
    stats->io_empty_waits = uring_stats.empty_waits;
    stats->workers = std::max<uint64_t>(stats->workers, uring_workers);
    stats->worker_wall_ns += wall_ns;
    stats->worker_max_busy_ns =
        std::max(stats->worker_max_busy_ns, wall_ns);
    stats->read_ns += uring_stats.wait_ns;
    for (uint64_t value : decode_ns) stats->decode_ns += value;
    scatter_cache_misses(
        decode, *miss_groups, *misses, threads);
    return true;
}

// 使用同步 pread 工作线程执行规划后的全部物理范围。
void execute_pread_batch(int packed_descriptor,
                         size_t threads,
                         const ReadPlan* plan,
                         BatchDecodeContext* decode,
                         CacheHitDecodeContext* cache_hits,
                         const std::vector<CacheMissGroup>* miss_groups,
                         const SequenceRequests* misses,
                         BatchStats* stats) {
    decode_cache_hits(cache_hits);
    publish_cache_hit_decode(stats, cache_hits->stats, true);

    const bool parallel_single_range = plan->ranges.size() == 1 &&
        threads > 1 && decode->read_requests->size() >= 65536;
    const size_t worker_count = std::min(threads, plan->ranges.size());
    stats->workers = std::max<uint64_t>(
        stats->workers,
        parallel_single_range ? threads : worker_count);

    const bool diagnostics = stats->diagnostics_collected;
    std::vector<PreadWorkerStats> worker_stats(diagnostics ? worker_count : 0);
    std::vector<std::exception_ptr> errors(worker_count);
    std::atomic<size_t> next_range{0};
    const DiagnosticTimer wall_timer(diagnostics);
    run_indexed_workers(worker_count, [&](size_t worker) noexcept {
        try {
            std::vector<uint8_t, NoInitAllocator<uint8_t>> buffer;
            while (true) {
                const size_t range_index = next_range.fetch_add(
                    1, std::memory_order_relaxed);
                if (range_index >= plan->ranges.size()) break;
                const ByteRange& range = plan->ranges[range_index];
                const uint64_t range_bytes = range.end - range.begin;
                if (range_bytes > static_cast<uint64_t>(
                        std::numeric_limits<size_t>::max())) {
                    throw std::length_error(
                        "packed byte range exceeds address space");
                }
                const size_t needed = static_cast<size_t>(range_bytes);
                if (needed > buffer.size()) buffer.resize(needed);

                const DiagnosticTimer read_timer(diagnostics);
                if (parallel_single_range) {
                    pread_exact_parallel(
                        packed_descriptor, buffer.data(), range_bytes,
                        range.begin, threads);
                } else {
                    pread_exact(packed_descriptor, buffer.data(),
                                range_bytes, range.begin);
                }
                if (diagnostics) worker_stats[worker].read_ns += read_timer.elapsed_ns();

                const DiagnosticTimer decode_timer(diagnostics);
                if (parallel_single_range) {
                    constexpr size_t kRequestGrain = 4096;
                    std::vector<std::exception_ptr> decode_errors(
                        threads);
                    std::atomic<size_t> next_request{range.request_begin};
                    run_indexed_workers(
                        threads, [&](size_t decode_worker) noexcept {
                            try {
                                while (true) {
                                    const size_t begin =
                                        next_request.fetch_add(
                                            kRequestGrain,
                                            std::memory_order_relaxed);
                                    if (begin >= range.request_end) break;
                                    const size_t end = std::min(
                                        range.request_end,
                                        begin + kRequestGrain);
                                    for (size_t i = begin; i < end; ++i) {
                                        const SequenceRequest& request =
                                            (*decode->read_requests)[i];
                                        decode_request_from_extent(
                                            request, buffer.data(),
                                            range.begin, range.end,
                                            batch_request_output(
                                                decode, request),
                                            !decode->direct_output);
                                    }
                                }
                            } catch (...) {
                                decode_errors[decode_worker] =
                                    std::current_exception();
                            }
                        });
                    rethrow_worker_errors(decode_errors);
                } else {
                    for (size_t i = range.request_begin;
                         i < range.request_end; ++i) {
                        const SequenceRequest& request =
                            (*decode->read_requests)[i];
                        decode_request_from_extent(
                            request, buffer.data(), range.begin, range.end,
                            batch_request_output(decode, request),
                            !decode->direct_output);
                    }
                }
                if (diagnostics) {
                    worker_stats[worker].decode_ns += decode_timer.elapsed_ns();
                }
            }
        } catch (...) {
            errors[worker] = std::current_exception();
        }
    });
    stats->worker_wall_ns += wall_timer.elapsed_ns();
    rethrow_worker_errors(errors);
    for (const PreadWorkerStats& worker : worker_stats) {
        stats->read_ns += worker.read_ns;
        stats->decode_ns += worker.decode_ns;
        stats->worker_max_busy_ns = std::max(
            stats->worker_max_busy_ns,
            worker.read_ns + worker.decode_ns);
    }
    scatter_cache_misses(decode, *miss_groups, *misses, threads);
}

// 按请求数量划分常驻数据解码任务，直接写入结果位置而不排序或规划磁盘读取。
void decode_full_memory(const StoreReadContext& store,
                        const SequenceRequests& requests,
                        size_t thread_count,
                        BatchDecodeContext* decode) {
    if (requests.empty()) return;
    Batch& batch = *decode->batch;
    const size_t worker_count = requests.size() < 4096
        ? size_t{1}
        : std::min(thread_count, requests.size());
    batch.stats.workers = worker_count;

    const bool diagnostics = batch.stats.diagnostics_collected;
    std::vector<uint64_t> decode_ns(diagnostics ? worker_count : 0, 0);
    std::vector<std::exception_ptr> errors(worker_count);

    const DiagnosticTimer wall_timer(diagnostics);
    run_indexed_workers(worker_count, [&](size_t worker) noexcept {
        const DiagnosticTimer decode_timer(diagnostics);
        try {
            const size_t begin =
                requests.size() * worker / worker_count;
            const size_t end =
                requests.size() * (worker + 1) / worker_count;
            for (size_t i = begin; i < end; ++i) {
                const SequenceRequest& request = requests[i];
                decode_request_from_extent(
                    request, store.full_packed, 0,
                    store.full_packed_bytes,
                    batch_request_output(decode, request),
                    !decode->direct_output);
            }
        } catch (...) {
            errors[worker] = std::current_exception();
        }
        if (diagnostics) decode_ns[worker] = decode_timer.elapsed_ns();
    });
    batch.stats.worker_wall_ns = wall_timer.elapsed_ns();
    rethrow_worker_errors(errors);
    for (uint64_t value : decode_ns) {
        batch.stats.decode_ns += value;
        batch.stats.worker_max_busy_ns = std::max(
            batch.stats.worker_max_busy_ns, value);
    }
}

}  // namespace

// 组织一次随机批读，依次准备布局、筛出前缀命中、规划未命中范围并执行后端。
void fill_batch_from_store(const StoreReadContext& store,
                           const std::vector<SequenceId>& ids,
                           const ReadOptions& options,
                           Batch& batch,
                           char* const* outputs) {
    BatchLayoutState layout;
    prepare_batch_layout(
        store.metadata, store.metadata->sequence_count(),
        store.storage_mode, &ids, &options, &batch, &layout, outputs);
    BatchDecodeContext decode_ctx;
    decode_ctx.prefix_packed = store.prefix_packed;
    decode_ctx.prefix_packed_bytes = store.prefix_packed_bytes;
    decode_ctx.requests = &layout.requests;
    decode_ctx.batch = &batch;
    decode_ctx.direct_output = outputs != nullptr;
    if (store.storage_mode == StorageMode::FullMemory) {
        decode_full_memory(store, layout.requests, layout.threads, &decode_ctx);
        return;
    }

    BatchCacheWork cache;
    prepare_batch_cache(
        store.storage_mode,
        store.prefix_packed_bytes, &layout.requests, &batch.stats, &cache);

    decode_ctx.read_requests = cache.read_requests;

    CacheHitDecodeContext cache_hit_ctx;
    cache_hit_ctx.decode = &decode_ctx;
    cache_hit_ctx.groups = &cache.hits;
    cache_hit_ctx.maximum_workers = layout.threads;

    const DiagnosticTimer plan_timer(options.collect_diagnostics);
    const ReadPlan plan = plan_byte_ranges(*cache.read_requests, options);
    batch.stats.range_plan_ns = plan_timer.elapsed_ns();
    publish_read_plan(&batch.stats, &plan, store.storage_mode);

    if (plan.ranges.empty()) {
        decode_cache_hits(&cache_hit_ctx);
        publish_cache_hit_decode(
            &batch.stats, cache_hit_ctx.stats, true);
        return;
    }

    if (execute_uring_batch(
            store.packed_descriptor, &options, layout.threads, &plan,
            &decode_ctx, &cache_hit_ctx, &cache.miss_groups,
            &cache.misses, &batch.stats)) {
        return;
    }
    execute_pread_batch(
        store.packed_descriptor, layout.threads, &plan, &decode_ctx,
        &cache_hit_ctx, &cache.miss_groups, &cache.misses, &batch.stats);
}

}  // namespace aastore
