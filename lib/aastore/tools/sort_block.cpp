// 文件职责：按序列长度对指定 ID 块排序，再批量读取记录并输出 FASTA。
// 支持比较排序或并行基数排序，并可将后续块读取与当前块输出重叠。
#include "aastore/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Arguments {
    std::filesystem::path store;
    std::filesystem::path output;
    size_t block_size = 100000;
    size_t threads = 8;
    size_t sort_threads = 0;
    bool descending = true;
    bool all_blocks = false;
    bool async_output = true;
};

struct LengthId {
    uint64_t length = 0;
    aastore::SequenceId id = 0;
};

// 输出按序列长度排序导出工具的用法。
void print_usage(std::ostream& output) {
    output << "usage: aastore_sort_block STORE OUTPUT [OPTIONS]\n"
              "  --block-size N      number of records in this block (default: 100000)\n"
              "  --threads N         AAStore read threads (default: 8)\n"
              "  --sort-threads N    global sort threads (default: threads)\n"
              "  --ascending         shortest records first (default: longest first)\n"
              "  --all               process every sorted block (default: first block only)\n"
              "  --sync-output       disable fetch/write overlap\n";
}

// 解析非零规模参数并校验不超出 size_t 范围。
size_t parse_size(const std::string& value, const char* option) {
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0 ||
        parsed > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument(std::string("invalid value for ") +
                                    option + ": " + value);
    }
    return static_cast<size_t>(parsed);
}

// 解析数据库、输出路径、排序方向和块读取选项。
Arguments parse_arguments(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage(std::cout);
        std::exit(0);
    }
    if (argc < 3) {
        throw std::invalid_argument("expected STORE and OUTPUT paths");
    }

    Arguments arguments;
    arguments.store = argv[1];
    arguments.output = argv[2];
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        const bool needs_value =
            option == "--block-size" || option == "--threads" ||
            option == "--sort-threads";
        std::string value;
        if (needs_value) {
            if (++index >= argc) {
                throw std::invalid_argument(
                    "missing value for " + option);
            }
            value = argv[index];
        }
        if (option == "--block-size") {
            arguments.block_size = parse_size(value, "--block-size");
        } else if (option == "--threads") {
            arguments.threads = parse_size(value, "--threads");
        } else if (option == "--sort-threads") {
            arguments.sort_threads = parse_size(value, "--sort-threads");
        } else if (option == "--ascending") {
            arguments.descending = false;
        } else if (option == "--all") {
            arguments.all_blocks = true;
        } else if (option == "--sync-output") {
            arguments.async_output = false;
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    return arguments;
}

// 计算从指定时刻开始经过的毫秒数。
double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               Clock::now() - start).count();
}

struct WorstFirst {
    // 保存长度排序方向，供固定容量候选堆使用。
    explicit WorstFirst(bool descending) : descending_(descending) {}

    // 按长度和 ID 比较候选，使优先队列顶部为当前最差的保留项。
    bool operator()(const LengthId& left, const LengthId& right) const {
        if (left.length != right.length) {
            return descending_ ? left.length > right.length
                               : left.length < right.length;
        }
        return left.id < right.id;
    }

    bool descending_;
};

// 判断左侧候选是否应优先输出；长度相同时按 ID 递增。
bool better(const LengthId& left, const LengthId& right, bool descending) {
    if (left.length != right.length) {
        return descending ? left.length > right.length
                          : left.length < right.length;
    }
    return left.id < right.id;
}

struct BetterCompare {
    bool descending = true;

    // 将保存的排序方向应用到长度和 ID 比较。
    bool operator()(const LengthId& left, const LengthId& right) const {
        return better(left, right, descending);
    }
};

constexpr unsigned kRadixBits = 11;
constexpr size_t kRadix = size_t{1} << kRadixBits;
constexpr uint64_t kRadixMask = static_cast<uint64_t>(kRadix - 1);
using Histogram = std::array<size_t, kRadix>;

// 返回当前基数排序字段；长度降序通过按位取反转换为升序键。
uint64_t radix_key(const LengthId& value, bool length_pass,
                   bool descending) {
    if (!length_pass) return value.id;
    return descending ? ~value.length : value.length;
}

using SortWorker = void (*)(void* context, size_t worker);

// 执行索引化排序任务，调用线程承担第零个分片并等待其他线程。
void run_sort_workers(size_t worker_count, SortWorker function,
                      void* context) {
    std::vector<std::thread> workers;
    workers.reserve(worker_count - 1);
    for (size_t worker = 1; worker < worker_count; ++worker) {
        workers.emplace_back(function, context, worker);
    }
    function(context, 0);
    for (std::thread& worker : workers) worker.join();
}

struct RadixContext {
    std::vector<LengthId>* values = nullptr;
    std::vector<LengthId>* scratch = nullptr;
    std::vector<Histogram>* counts = nullptr;
    std::vector<Histogram>* positions = nullptr;
    size_t worker_count = 1;
    unsigned shift = 0;
    bool length_pass = false;
    bool descending = true;
};

// 统计当前线程分片在指定基数位段上的桶计数。
void count_radix_worker(void* opaque, size_t worker) {
    RadixContext* ctx = static_cast<RadixContext*>(opaque);
    Histogram& histogram = (*ctx->counts)[worker];
    histogram.fill(0);
    const size_t begin =
        ctx->values->size() * worker / ctx->worker_count;
    const size_t end =
        ctx->values->size() * (worker + 1) / ctx->worker_count;
    for (size_t index = begin; index < end; ++index) {
        const uint64_t key = radix_key(
            (*ctx->values)[index], ctx->length_pass, ctx->descending);
        ++histogram[(key >> ctx->shift) & kRadixMask];
    }
}

// 按预计算的桶起点散布当前线程分片，线程输出区互不重叠。
void scatter_radix_worker(void* opaque, size_t worker) {
    RadixContext* ctx = static_cast<RadixContext*>(opaque);
    Histogram& output_positions = (*ctx->positions)[worker];
    const size_t begin =
        ctx->values->size() * worker / ctx->worker_count;
    const size_t end =
        ctx->values->size() * (worker + 1) / ctx->worker_count;
    for (size_t index = begin; index < end; ++index) {
        const uint64_t key = radix_key(
            (*ctx->values)[index], ctx->length_pass, ctx->descending);
        const size_t bucket = (key >> ctx->shift) & kRadixMask;
        (*ctx->scratch)[output_positions[bucket]++] =
            (*ctx->values)[index];
    }
}

// 按长度和 ID 排序，小规模使用比较排序，大规模使用并行基数排序。
void radix_sort_length_id(std::vector<LengthId>& values,
                          size_t thread_count,
                          bool descending) {
    if (values.size() <= 1) return;

    constexpr size_t kMinimumParallelItems = 1 << 20;

    thread_count = std::max<size_t>(1, thread_count);
    if (values.size() < kMinimumParallelItems) {
        std::sort(
            values.begin(), values.end(), BetterCompare{descending});
        return;
    }
    thread_count = std::min(thread_count, values.size());

    std::vector<LengthId> scratch(values.size());
    std::vector<Histogram> counts(thread_count);
    std::vector<Histogram> positions(thread_count);
    RadixContext ctx;
    ctx.values = &values;
    ctx.scratch = &scratch;
    ctx.counts = &counts;
    ctx.positions = &positions;
    ctx.worker_count = thread_count;
    ctx.descending = descending;

    // LSD 基数排序先处理 ID，再处理主键长度，保证同长度时按 ID 排序。
    // 每个 uint64_t 字段用六轮 11 位分桶即可覆盖。
    for (unsigned pass = 0; pass < 12; ++pass) {
        ctx.length_pass = pass >= 6;
        ctx.shift = (pass % 6) * kRadixBits;
        run_sort_workers(
            thread_count, count_radix_worker, &ctx);

        size_t bucket_base = 0;
        for (size_t bucket = 0; bucket < kRadix; ++bucket) {
            size_t thread_base = bucket_base;
            for (size_t worker = 0; worker < thread_count; ++worker) {
                positions[worker][bucket] = thread_base;
                thread_base += counts[worker][bucket];
            }
            bucket_base = thread_base;
        }

        run_sort_workers(
            thread_count, scatter_radix_worker, &ctx);
        values.swap(scratch);
    }
}

// 按记录顺序将名称、可选描述和序列写为 FASTA。
void write_records(std::ofstream* output,
                   const aastore::RecordBatch& records) {
    for (size_t index = 0; index < records.size(); ++index) {
        const std::string_view name = records.name_view(index);
        const std::string_view description =
            records.description_view(index);
        const std::string_view sequence = records.sequence_view(index);
        output->put('>');
        output->write(
            name.data(), static_cast<std::streamsize>(name.size()));
        if (!description.empty()) {
            output->put(' ');
            output->write(
                description.data(),
                static_cast<std::streamsize>(description.size()));
        }
        output->put('\n');
        output->write(
            sequence.data(),
            static_cast<std::streamsize>(sequence.size()));
        output->put('\n');
    }
}

struct FetchedBlock {
    aastore::RecordBatch records;
    double fetch_ms = 0;
};

// 读取排序结果中的一个块，并返回记录批次和读取耗时。
FetchedBlock fetch_block(
        aastore::Store* store,
        const std::vector<LengthId>* selected,
        size_t begin,
        size_t block_size,
        aastore::RecordFields fields,
        const aastore::ReadOptions* options) {
    const size_t end =
        std::min(begin + block_size, selected->size());
    std::vector<aastore::SequenceId> ids;
    ids.reserve(end - begin);
    for (size_t index = begin; index < end; ++index) {
        ids.push_back((*selected)[index].id);
    }
    const auto fetch_start = Clock::now();
    aastore::RecordBatch records =
        store->batch_get_records(ids, fields, *options);
    return FetchedBlock{
        std::move(records), elapsed_ms(fetch_start)};
}

struct FetchContext {
    aastore::Store* store = nullptr;
    const std::vector<LengthId>* selected = nullptr;
    size_t begin = 0;
    size_t block_size = 0;
    aastore::RecordFields fields = aastore::RecordFields::None;
    const aastore::ReadOptions* options = nullptr;
    FetchedBlock* result = nullptr;
    std::exception_ptr error;
};

// 在后台读取指定块，把异常保存给调用线程。
void fetch_worker(FetchContext* ctx) noexcept {
    try {
        *ctx->result = fetch_block(
            ctx->store, ctx->selected, ctx->begin, ctx->block_size,
            ctx->fields, ctx->options);
    } catch (...) {
        ctx->error = std::current_exception();
    }
}

// 设置下一块起点并启动后台读取线程。
void start_fetch(FetchContext* ctx, size_t begin,
                 std::thread* thread) {
    ctx->begin = begin;
    ctx->error = nullptr;
    *thread = std::thread(fetch_worker, ctx);
}

}  // namespace

// 选择或排序序列 ID，分块读取并导出 FASTA，按配置重叠读取和写出。
int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        const auto total_start = Clock::now();
        aastore::StoreOptions store_options;
        store_options.resident_names = true;
        store_options.resident_descriptions = true;
        const auto open_start = Clock::now();
        aastore::Store store(arguments.store, store_options);
        const double open_ms = elapsed_ms(open_start);
        if (!store.has_names() || !store.has_descriptions()) {
            throw std::runtime_error(
                "single-block FASTA output requires names and descriptions");
        }
        if (store.size() == 0) {
            throw std::runtime_error("cannot sort an empty store");
        }

        const size_t store_size = static_cast<size_t>(store.size());
        const size_t wanted = std::min<size_t>(arguments.block_size, store_size);
        std::vector<LengthId> selected;
        const auto scan_start = Clock::now();
        if (arguments.all_blocks) {
            selected.resize(store_size);
            for (aastore::SequenceId id = 0; id < store.size(); ++id) {
                selected[static_cast<size_t>(id)] =
                    LengthId{store.sequence_length(id), id};
            }
        } else {
            std::priority_queue<LengthId, std::vector<LengthId>, WorstFirst> top(
                WorstFirst(arguments.descending));
            for (aastore::SequenceId id = 0; id < store.size(); ++id) {
                const LengthId candidate{store.sequence_length(id), id};
                if (top.size() < wanted) {
                    top.push(candidate);
                } else if (better(candidate, top.top(), arguments.descending)) {
                    top.pop();
                    top.push(candidate);
                }
            }
            selected.reserve(top.size());
            while (!top.empty()) {
                selected.push_back(top.top());
                top.pop();
            }
        }
        const double scan_ms = elapsed_ms(scan_start);

        const auto sort_start = Clock::now();
        const size_t sort_threads = arguments.sort_threads == 0
            ? arguments.threads
            : arguments.sort_threads;
        radix_sort_length_id(
            selected, sort_threads, arguments.descending);
        const double sort_ms = elapsed_ms(sort_start);

        aastore::ReadOptions read_options;
        read_options.threads = arguments.threads;
        const aastore::RecordFields fields =
            aastore::RecordFields::Name |
            aastore::RecordFields::Description |
            aastore::RecordFields::Sequence;

        std::ofstream output(arguments.output,
                             std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create output: " +
                                     arguments.output.string());
        }

        double fetch_ms = 0;
        double write_ms = 0;
        uint64_t output_sequences = 0;
        uint64_t total_sequence_ranges = 0;
        uint64_t total_sequence_read_bytes = 0;
        uint64_t total_name_ranges = 0;
        uint64_t total_description_ranges = 0;
        size_t blocks = 0;
        FetchedBlock pending_result;
        FetchContext fetch_ctx;
        fetch_ctx.store = &store;
        fetch_ctx.selected = &selected;
        fetch_ctx.block_size = wanted;
        fetch_ctx.fields = fields;
        fetch_ctx.options = &read_options;
        fetch_ctx.result = &pending_result;
        std::thread fetch_thread;
        if (arguments.async_output) {
            start_fetch(&fetch_ctx, 0, &fetch_thread);
        }
        try {
            for (size_t begin = 0; begin < selected.size(); begin += wanted) {
            const size_t end = std::min(begin + wanted, selected.size());
            FetchedBlock fetched;
            if (arguments.async_output) {
                fetch_thread.join();
                if (fetch_ctx.error) {
                    std::rethrow_exception(fetch_ctx.error);
                }
                fetched = std::move(pending_result);
            } else {
                fetched = fetch_block(
                    &store, &selected, begin, wanted, fields,
                    &read_options);
            }
            if (arguments.async_output && end < selected.size()) {
                start_fetch(&fetch_ctx, end, &fetch_thread);
            }

            aastore::RecordBatch& records = fetched.records;
            fetch_ms += fetched.fetch_ms;
            const auto block_write_start = Clock::now();
            write_records(&output, records);
            write_ms += elapsed_ms(block_write_start);
            output_sequences += records.size();
            total_sequence_ranges += records.sequences.stats.ranges;
            total_sequence_read_bytes += records.sequences.stats.read_bytes;
            total_name_ranges += records.names.stats.ranges;
            total_description_ranges += records.descriptions.stats.ranges;
            ++blocks;
            if (!arguments.all_blocks) break;
            }
        } catch (...) {
            if (fetch_thread.joinable()) fetch_thread.join();
            throw;
        }
        output.close();
        if (!output) throw std::runtime_error("failed writing output");

        const uint64_t first_length = selected.empty() ? 0 : selected.front().length;
        const uint64_t last_length = selected.empty() ? 0 : selected.back().length;
        std::cout << "store=" << arguments.store << '\n'
                  << "output=" << arguments.output << '\n'
                  << "store_sequences=" << store.size() << '\n'
                  << "output_sequences=" << output_sequences << '\n'
                  << "blocks=" << blocks << '\n'
                  << "block_size=" << arguments.block_size << '\n'
                  << "all_blocks=" << std::boolalpha << arguments.all_blocks
                  << '\n'
                  << "sort_order=" << (arguments.descending ? "descending" :
                                                    "ascending") << '\n'
                  << "first_length=" << first_length << '\n'
                  << "last_length=" << last_length << '\n'
                  << "threads=" << arguments.threads << '\n'
                  << "sort_threads=" << sort_threads << '\n'
                  << "async_output=" << std::boolalpha
                  << arguments.async_output << '\n'
                  << "resident_text_bytes=" << store.resident_text_bytes() << '\n'
                  << "open_ms=" << open_ms << '\n'
                  << "scan_ms=" << scan_ms << '\n'
                  << "sort_ms=" << sort_ms << '\n'
                  << "fetch_ms=" << fetch_ms << '\n'
                  << "write_ms=" << write_ms << '\n'
                  << "sequence_ranges=" << total_sequence_ranges << '\n'
                  << "sequence_read_bytes=" << total_sequence_read_bytes << '\n'
                  << "names_resident=true\n"
                  << "descriptions_resident=true\n"
                  << "name_ranges=" << total_name_ranges << '\n'
                  << "description_ranges=" << total_description_ranges << '\n'
                  << "end_to_end_ms=" << elapsed_ms(total_start) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        print_usage(std::cerr);
        return 2;
    }
}
