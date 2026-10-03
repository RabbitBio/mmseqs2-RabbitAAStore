// 文件职责：验证建库、元数据、随机及顺序读取、直接输出和文本列等公开接口。
// 覆盖多种驻留模式、线程数、异常输入及批次边界，核对结果内容和输出缓冲边界。
#include "aastore/store.hpp"
#include "../src/store_io.hpp"
#if defined(AASTORE_TEST_IO_URING)
#include "../src/io_uring_backend.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

// 记录失败断言并输出说明，不中断后续测试。
void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct ConcurrentGetContext {
    aastore::Store* store = nullptr;
    const std::vector<std::string>* expected = nullptr;
    std::array<bool, 8>* results = nullptr;
};

// 使用线程独立字符串重复读取序列，并记录是否全部匹配。
void run_concurrent_get(ConcurrentGetContext* ctx,
                        size_t worker) noexcept {
    try {
        std::string output;
        for (size_t i = 0; i < ctx->expected->size() * 3; ++i) {
            const size_t id = (i + worker) % ctx->expected->size();
            ctx->store->get_into(id, output);
            if (output != (*ctx->expected)[id]) return;
        }
        (*ctx->results)[worker] = true;
    } catch (...) {
        (*ctx->results)[worker] = false;
    }
}

// 判断所有线程结果是否均为成功。
bool all_true(const bool* values, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!values[i]) return false;
    }
    return true;
}

// 在调用线程抛异常，其他线程仍须结束；两种任务接口均保留原异常。
void check_worker_exception_cleanup() {
    struct Context {
        std::atomic<size_t> completed{0};
    };
    auto worker = [](Context* context, size_t index) {
        if (index == 0) throw std::runtime_error("caller task failed");
        context->completed.fetch_add(1, std::memory_order_relaxed);
    };
    for (bool use_context : {false, true}) {
        Context context;
        bool rejected = false;
        try {
            if (use_context) {
                aastore::run_indexed_workers(4, +worker, &context);
            } else {
                aastore::run_indexed_workers(4, [&](size_t index) {
                    worker(&context, index);
                });
            }
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == "caller task failed";
        }
        expect(rejected && context.completed.load(std::memory_order_relaxed) == 3,
               "caller task exceptions join all workers and preserve the original error");
    }
}

// 核对诊断标记、关闭时的零值，以及非空读取启用后的阶段计时。
void check_diagnostic_stats(const aastore::BatchStats& stats, bool enabled) {
    expect(stats.diagnostics_collected == enabled, "read reports its diagnostic setting");
    if (!enabled) {
        using S = aastore::BatchStats;
        static constexpr uint64_t S::* fields[] = {
            &S::cache_hit_residues, &S::cache_miss_residues, &S::unique_packed_bytes,
            &S::merged_gaps, &S::free_merges, &S::overread_bytes,
            &S::io_submit_calls, &S::io_submit_ns, &S::io_wait_calls, &S::io_empty_waits,
            &S::layout_ns, &S::sort_ns, &S::range_plan_ns, &S::worker_wall_ns,
            &S::worker_max_busy_ns, &S::read_ns, &S::decode_ns, &S::cache_decode_ns
        };
        for (auto field : fields) {
            expect(stats.*field == 0, "disabled read diagnostics remain zero");
        }
    } else if (stats.requested_residues != 0) {
        expect(stats.layout_ns > 0 && stats.worker_wall_ns > 0 && stats.decode_ns > 0,
               "enabled diagnostics measure layout, workers and decode");
        if (stats.cache_hits != 0) {
            expect(stats.cache_decode_ns > 0, "enabled diagnostics measure prefix decode");
        }
        if (stats.prefix_memory && (stats.cache_hits != 0 || stats.cache_misses != 0)) {
            expect(stats.cache_hit_residues + stats.cache_miss_residues ==
                       stats.requested_residues,
                   "prefix diagnostics account for residues including duplicate IDs");
        }
        if (stats.io_uring) {
            expect(stats.io_submit_calls > 0 && stats.io_submit_ns > 0 &&
                       stats.io_empty_waits <= stats.io_wait_calls,
                   "enabled diagnostics measure io_uring submissions and waits");
        }
    }
}

// 比较不依赖运行时调度的基本统计，诊断开关不应改变它们。
void check_basic_stats(const aastore::BatchStats& left, const aastore::BatchStats& right) {
    using S = aastore::BatchStats;
    static constexpr uint64_t S::* fields[] = {
        &S::requested_sequences, &S::requested_residues, &S::cache_hits, &S::cache_misses,
        &S::ranges, &S::read_bytes, &S::max_range_bytes,
        &S::workers, &S::io_workers, &S::decode_workers, &S::io_submissions, &S::io_completions
    };
    expect(left.full_memory == right.full_memory && left.prefix_memory == right.prefix_memory &&
               left.io_uring == right.io_uring, "diagnostics preserve backend and storage flags");
    for (auto field : fields) {
        expect(left.*field == right.*field, "diagnostics preserve basic read statistics");
    }
}

// 用已知区间验证串行/并行规划，诊断开关不改变范围及缓冲预算。
void check_planner_diagnostics() {
    for (size_t copies : {size_t{1}, size_t{14000}}) {
        aastore::SequenceRequests requests;
        const std::array<uint64_t, 5> starts{0, 1, 2, 16, 24};
        const std::array<uint64_t, 5> lengths{1, 1, 0, 8, 8};
        for (size_t id = 0; id < starts.size(); ++id) {
            for (size_t copy = 0; copy < copies; ++copy) {
                requests.push_back({id, requests.size(), starts[id], lengths[id]});
            }
        }
        for (size_t threads : {size_t{1}, size_t{8}}) {
            for (bool diagnostics : {false, true}) {
                aastore::ReadOptions options;
                options.threads = threads;
                options.collect_diagnostics = diagnostics;
                const auto plan = aastore::plan_byte_ranges(requests, options);
                expect(plan.ranges.size() == 1 && plan.ranges[0].begin == 0 &&
                           plan.ranges[0].end == 20 && plan.ranges[0].request_begin == 0 &&
                           plan.ranges[0].request_end == requests.size(),
                       "diagnostics preserve serial and parallel range boundaries");
                expect(plan.requested_residues == 18 * copies &&
                           plan.read_bytes == 20 && plan.max_range_bytes == 20,
                       "diagnostics preserve basic planner counts and buffer sizing");
                expect(plan.unique_packed_bytes == (diagnostics ? 12 : 0) &&
                           plan.merged_gaps == (diagnostics ? 1 : 0) &&
                           plan.free_merges == (diagnostics ? 4 * copies - 2 : 0),
                       "optional planner counts handle duplicates, shared bytes and empty sequences");
            }
        }
    }
}

// 对同一批次开关诊断各读一次，检查内容及基本统计一致，返回未启用诊断的结果。
aastore::Batch check_batch_diagnostics(const aastore::Store& store,
                                       const std::vector<aastore::SequenceId>& ids,
                                       aastore::ReadOptions options) {
    options.collect_diagnostics = false;
    auto plain = store.get(ids, options);
    options.collect_diagnostics = true;
    const auto measured = store.get(ids, options);
    check_diagnostic_stats(plain.stats, false);
    check_diagnostic_stats(measured.stats, true);
    check_basic_stats(plain.stats, measured.stats);
    for (size_t i = 0; i < ids.size(); ++i) {
        expect(plain.view(i) == measured.view(i), "diagnostics preserve batch content and order");
    }
    return plain;
}

// 使用精确长度的输出区重复读取，检查顺序、重复 ID、空序列和缓冲边界。
void check_batch_get_into(const aastore::Store& store,
                           const std::vector<std::string>& expected,
                           const std::vector<aastore::SequenceId>& ids,
                           const aastore::ReadOptions& options) {
    std::vector<std::vector<char>> buffers(ids.size());
    std::vector<char*> outputs(ids.size());
    uint64_t residues = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        const size_t length = expected[ids[i]].size();
        buffers[i].resize(length + 2);
        outputs[i] = length == 0 ? nullptr : buffers[i].data() + 1;
        residues += length;
    }
    aastore::BatchStats baseline;
    for (size_t round = 0; round < 4; ++round) {
        auto reads = options;
        reads.collect_diagnostics = round >= 2;
        for (auto& buffer : buffers) std::fill(buffer.begin(), buffer.end(), '#');
        const auto stats = round % 2 == 0
            ? store.get_into(ids, outputs, reads)
            : store.get_into(ids, outputs.data(), outputs.size(), reads);
        check_diagnostic_stats(stats, reads.collect_diagnostics);
        if (round == 0) baseline = stats;
        else check_basic_stats(baseline, stats);
        expect(stats.requested_sequences == ids.size() &&
                   stats.requested_residues == residues &&
                   stats.workers <= std::max<size_t>(1, options.threads),
               "direct batch reports request statistics within its thread budget");
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto& reference = expected[ids[i]];
            expect(std::equal(reference.begin(), reference.end(),
                              buffers[i].begin() + 1) &&
                       buffers[i].front() == '#' && buffers[i].back() == '#',
                   "direct batch preserves content and writes no terminator or guard bytes");
        }
    }
}

// 用逆序排列的独立缓冲读取连续区间，检查两个重载、输出复用和空序列边界。
void check_sequential_get_into(const aastore::Store& store,
                                const std::vector<std::string>& expected,
                                aastore::SequenceId begin,
                                aastore::SequenceId end,
                                const aastore::ReadOptions& options) {
    const size_t count = static_cast<size_t>(end - begin);
    std::vector<std::vector<char>> buffers(count);
    std::vector<char*> outputs(count);
    uint64_t residues = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t length = expected[begin + i].size();
        auto& buffer = buffers[count - i - 1];
        buffer.resize(length + 2);
        outputs[i] = length == 0 ? nullptr : buffer.data() + 1;
        residues += length;
    }
    aastore::BatchStats baseline;
    for (size_t round = 0; round < 4; ++round) {
        auto reads = options;
        reads.collect_diagnostics = round >= 2;
        for (auto& buffer : buffers) std::fill(buffer.begin(), buffer.end(), '#');
        const auto stats = round % 2 == 0
            ? store.sequential_get_into(begin, end, outputs, reads)
            : store.sequential_get_into(begin, end, outputs.data(), outputs.size(), reads);
        check_diagnostic_stats(stats, reads.collect_diagnostics);
        if (round == 0) baseline = stats;
        else check_basic_stats(baseline, stats);
        expect(stats.requested_sequences == count &&
                   stats.requested_residues == residues &&
                   stats.workers <= std::max<size_t>(1, options.threads) &&
                   stats.full_memory == (store.storage_mode() == aastore::StorageMode::FullMemory) &&
                   stats.prefix_memory == (store.storage_mode() == aastore::StorageMode::PrefixMemory),
               "direct sequential reports interval statistics within its thread budget");
        for (size_t i = 0; i < count; ++i) {
            const auto& reference = expected[begin + i];
            const auto& buffer = buffers[count - i - 1];
            expect(std::equal(reference.begin(), reference.end(), buffer.begin() + 1) &&
                       buffer.front() == '#' && buffer.back() == '#',
                   "direct sequential preserves scattered outputs and writes no guard or terminator");
        }
    }
    auto reads = options;
    reads.collect_diagnostics = false;
    const auto plain = store.sequential_get(begin, end, reads);
    reads.collect_diagnostics = true;
    const auto measured = store.sequential_get(begin, end, reads);
    check_diagnostic_stats(plain.stats, false);
    check_diagnostic_stats(measured.stats, true);
    check_basic_stats(plain.stats, measured.stats);
    for (size_t i = 0; i < count; ++i) {
        expect(plain.view(i) == expected[begin + i] && measured.view(i) == plain.view(i),
               "diagnostics preserve sequential batch content");
    }
}

struct TempDir {
    // 创建带进程 ID 的专用测试目录，清理同名旧测试残留。
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("aastore_byte_range_test_" + std::to_string(::getpid()))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    // 尽力删除测试目录，析构时不传播清理错误。
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    // 返回测试临时目录路径的只读引用。
    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path path_;
};

// 偶数记录使 AAPCMP3 的块基址不按八字节对齐，仍须正确打开并读取全部边界。
void check_even_record_metadata(const std::filesystem::path& root) {
    const auto fasta = root / "even_records.fa";
    const auto path = root / "even_records_store";
    {
        std::ofstream output(fasta);
        output << ">one\nACDEFG\n>two\nMKT\n";
    }
    aastore::build_fasta(fasta, path);
    aastore::Store store(path);
    expect(store.size() == 2 && store.total_residues() == 9 &&
               store.sequence_length(0) == 6 && store.sequence_length(1) == 3,
           "even-record metadata preserves sequence boundaries");
    const auto batch = store.get({1, 0});
    expect(batch[0] == "MKT" && batch[1] == "ACDEFG",
           "even-record metadata supports reordered sequence reads");
}

// 让多个线程切入同一条长序列，并覆盖重复边界、前缀命中和跨调用暂存区复用。
void check_sequential_into_partitions(const std::filesystem::path& root,
                                      bool concurrent) {
    const auto fasta = root / (concurrent ? "sequential_concurrency.fa"
                                         : "sequential_partitions.fa");
    const auto path = root / (concurrent ? "sequential_concurrency_store"
                                        : "sequential_partitions_store");
    const std::string alphabet = "ACDEFGHIKLMNPQRSTVWY";
    std::vector<std::string> expected{
        "", "ACD", "", "", std::string(9 * 1024 * 1024 + 17, 'A'),
        "", "TVWY", "", "Q", "", ""};
    for (size_t i = 0; i < expected[4].size(); ++i) {
        expected[4][i] = alphabet[i % alphabet.size()];
    }
    {
        std::ofstream output(fasta);
        for (size_t i = 0; i < expected.size(); ++i) {
            output << '>' << i << '\n' << expected[i] << '\n';
        }
    }
    aastore::build_fasta(fasta, path);
    aastore::Store disk_store(path);
    for (uint64_t budget : {uint64_t{0}, uint64_t{1}, disk_store.packed_bytes() / 2,
                            disk_store.packed_bytes()}) {
        aastore::StoreOptions store_options;
        store_options.storage_mode = budget == 0 ? aastore::StorageMode::ByteRange
            : budget == disk_store.packed_bytes() ? aastore::StorageMode::FullMemory
            : aastore::StorageMode::PrefixMemory;
        store_options.cache_bytes = budget;
        aastore::Store source(path, store_options);
        if (!concurrent) {
            for (size_t threads : {size_t{0}, size_t{1}, size_t{2}, size_t{4}, size_t{16}}) {
                aastore::ReadOptions reads;
                reads.threads = threads;
                check_sequential_get_into(source, expected, 0, source.size(), reads);
                check_sequential_get_into(source, expected, 2, source.size() - 2, reads);
                check_sequential_get_into(source, expected, 6, source.size(), reads);
                check_sequential_get_into(source, expected, 2, 4, reads);
            }
            continue;
        }
        std::array<bool, 4> concurrent_ok{};
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < concurrent_ok.size(); ++worker) {
            workers.emplace_back([&, worker]() {
                try {
                    std::string output(expected[4].size(), '#');
                    std::vector<char*> outputs{output.data()};
                    aastore::ReadOptions reads;
                    reads.threads = 1;
                    source.sequential_get_into(4, 5, outputs, reads);
                    concurrent_ok[worker] = output == expected[4];
                } catch (...) {
                    concurrent_ok[worker] = false;
                }
            });
        }
        for (auto& worker : workers) worker.join();
        expect(all_true(concurrent_ok.data(), concurrent_ok.size()),
               "direct sequential reads use independent internal buffers across callers");
    }
}

#if defined(AASTORE_TEST_IO_URING)
// 真实创建 ring 并读取小文件；只有初始化不可用可跳过，I/O 和回调错误继续失败。
bool probe_io_uring() {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
        std::tmpfile(), &std::fclose);
    if (!file) throw std::runtime_error("cannot create io_uring probe fixture");
    struct Probe {
        std::array<uint8_t, 4> expected{{17, 31, 63, 127}};
        bool completed = false;
    } probe;
    if (std::fwrite(probe.expected.data(), 1, probe.expected.size(), file.get()) !=
            probe.expected.size() || std::fflush(file.get()) != 0) {
        throw std::runtime_error("cannot write io_uring probe fixture");
    }
    const auto complete = [](void* opaque, size_t worker, size_t index,
                             const uint8_t* data, uint64_t size) {
        auto& state = *static_cast<Probe*>(opaque);
        if (worker != 0 || index != 0 || size != state.expected.size() ||
            !std::equal(state.expected.begin(), state.expected.end(), data)) {
            throw std::runtime_error("io_uring probe returned invalid data");
        }
        state.completed = true;
    };
    aastore::IoUringStats stats;
    std::string reason;
    const std::vector<aastore::ByteRange> ranges{
        {0, probe.expected.size(), 0, 1}};
    if (!aastore::read_ranges_io_uring(
            ::fileno(file.get()), ranges, 1, 1, nullptr, nullptr,
            complete, &probe, stats, reason)) {
        std::cout << "SKIP: explicit io_uring scenarios unavailable: " << reason
                  << "; pread and Auto scenarios continue\n";
        return false;
    }
    if (!probe.completed || stats.submissions != 1 || stats.completions != 1) {
        throw std::runtime_error("io_uring probe did not complete its read");
    }
    return true;
}

// 混合前缀命中与磁盘请求，验证小队列下的协作解码、长序列、重复 ID 和输出顺序。
void check_prefix_slices(const std::filesystem::path& root) {
    const auto fasta = root / "prefix_slices.fa";
    const auto path = root / "prefix_slices_store";
    std::vector<std::string> expected;
    uint64_t residues = 0;
    {
        std::ofstream output(fasta);
        for (size_t i = 0; i < 4096; ++i) {
            expected.emplace_back(i == 8 ? 1200000 : 1024 + i % 7,
                                  "ACDEFGHIKLMNPQRSTVWY"[i % 20]);
            residues += expected.back().size();
            output << ">" << i << '\n' << expected.back() << '\n';
        }
    }
    aastore::build_fasta(fasta, path);
    aastore::StoreOptions store_options;
    store_options.storage_mode = aastore::StorageMode::PrefixMemory;
    store_options.cache_bytes = (residues * 5 + 7) / 8 / 2;
    aastore::Store store(path, store_options);
    std::vector<aastore::SequenceId> ids;
    for (size_t i = expected.size(); i > 0; i -= 2) ids.push_back(i - 2);
    ids.insert(ids.end(), {8, 8, 4094, 4094});
    for (size_t threads : {size_t{1}, size_t{4}, size_t{16}}) {
        aastore::ReadOptions options;
        options.io_backend = aastore::IoBackend::IoUring;
        options.threads = threads;
        options.io_queue_depth = 1;
        options.merge_gap_bytes = 0;
        const auto batch = check_batch_diagnostics(store, ids, options);
        expect(batch.stats.cache_hits > 256 && batch.stats.ranges > 64 &&
                   batch.stats.io_completions == batch.stats.ranges &&
                   batch.stats.io_queue_depth <= threads,
               "prefix slicing covers repeated refills at depth one");
        for (size_t i = 0; i < ids.size(); ++i) {
            expect(batch.view(i) == expected[ids[i]],
                   "prefix slices preserve large groups, duplicates and output order");
        }
        check_batch_get_into(store, expected, ids, options);
    }
}
#endif

constexpr size_t kLongTestResidues = 6553;
constexpr uint64_t kPrefixCacheBytes = 4096;

// 每次运行创建独立建库夹具，各组无需依赖其他测试先执行。
struct StoreFixture {
    StoreFixture() {
        fasta = temp.path() / "proteins.fa";
        const std::string long_filler(kLongTestResidues, 'A');
        const std::string long_unaligned(kLongTestResidues + 17, 'W');
        std::string oversized_line(4 * 1024 * 1024 + 17, 'C');
        constexpr std::string_view oversized_alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ*-";
        for (size_t i = 0; i < oversized_line.size(); ++i) {
            oversized_line[i] = oversized_alphabet[i % oversized_alphabet.size()];
        }
        {
            std::ofstream output(fasta);
            output << ">empty\n"
                   << ">p0 description\nac de\tfg?i\n"
                   << ">filler1\n" << long_filler << "\n"
                   << ">p1\nKLMNPQRS\n"
                   << ">filler2\n" << long_filler << "\n"
                   << ">p2\nTVWYBXZJ\n"
                   << ">long\nACDEFGHIKLMNPQRSTVWY*--OU\n"
                   << ">cross\n" << long_unaligned << "\n"
                   << ">oversized\n" << oversized_line;
        }
        store_path = temp.path() / "store";
        built =
            aastore::build_fasta(fasta, store_path);
        expected = {
            "", "ACDEFGXI", long_filler, "KLMNPQRS", long_filler,
            "TVWYBXZJ", "ACDEFGHIKLMNPQRSTVWY*--OU", long_unaligned,
            oversized_line};
        expected_names = {
            "empty", "p0", "filler1", "p1", "filler2", "p2", "long",
            "cross", "oversized"};
        expected_descriptions = {
            "", "description", "", "", "", "", "", "", ""};
    }

    void check_builder();
    void check_random(bool io_uring_available);
    void check_sequential();
    void check_text();
    void check_concurrency(bool io_uring_available);

    TempDir temp;
    std::filesystem::path fasta;
    std::filesystem::path store_path;
    aastore::BuildStats built;
    std::vector<std::string> expected;
    std::vector<std::string> expected_names;
    std::vector<std::string> expected_descriptions;
};

// 驻留选项供相关组各自打开 Store，不共享运行时状态。
aastore::StoreOptions full_store_options() {
    aastore::StoreOptions options;
    options.storage_mode = aastore::StorageMode::FullMemory;
    options.threads = 2;
    return options;
}

aastore::StoreOptions prefix_store_options() {
    aastore::StoreOptions options;
    options.storage_mode = aastore::StorageMode::PrefixMemory;
    options.cache_bytes = kPrefixCacheBytes;
    options.threads = 2;
    return options;
}

// 建库格式、可选文本列、覆盖和空输入。
void StoreFixture::check_builder() {
    check_even_record_metadata(temp.path());
    expect(built.sequences == expected.size(), "sequence count");
    expect(built.name_bytes == 43 && built.description_bytes == 11,
           "annotation byte counts");
    expect(std::filesystem::exists(store_path / "meta.bin"),
           "original meta.bin exists");
    expect(std::filesystem::exists(store_path / "packed.bin"),
           "packed.bin exists");
    expect(std::filesystem::exists(store_path / "names.bin") &&
               std::filesystem::exists(store_path / "names.meta") &&
               std::filesystem::exists(store_path / "descriptions.bin") &&
               std::filesystem::exists(store_path / "descriptions.meta"),
           "compact annotation columns exist");
    {
        std::ifstream meta(store_path / "meta.bin", std::ios::binary);
        char magic[8] = {};
        meta.read(magic, sizeof(magic));
        expect(std::string(magic, 7) == "AAPCMP3",
               "builder writes current continuous compact metadata");
    }
    const std::filesystem::path rejected_store = temp.path() / "rejected_store";
    std::filesystem::create_directories(rejected_store);
    std::filesystem::copy_file(
        store_path / "meta.bin", rejected_store / "meta.bin");
    {
        std::fstream meta(rejected_store / "meta.bin",
                          std::ios::binary | std::ios::in | std::ios::out);
        meta.seekp(6);
        meta.put('2');
    }
    bool non_current_metadata_rejected = false;
    try {
        aastore::Store rejected(rejected_store);
    } catch (const std::runtime_error&) {
        non_current_metadata_rejected = true;
    }
    expect(non_current_metadata_rejected,
           "store rejects metadata formats other than AAPCMP3");
    for (const std::string& metadata_name :
         {std::string("names.meta"), std::string("descriptions.meta")}) {
        std::ifstream metadata(store_path / metadata_name, std::ios::binary);
        char magic[8] = {};
        metadata.read(magic, sizeof(magic));
        expect(std::string(magic, 7) == "AATEXT1",
               metadata_name + " uses compact text offsets");
    }

    const std::filesystem::path sequence_only_path =
        temp.path() / "sequence_only_store";
    aastore::BuildOptions sequence_only_options;
    sequence_only_options.write_names = false;
    sequence_only_options.write_descriptions = false;
    const aastore::BuildStats sequence_only_built =
        aastore::build_fasta(
            fasta, sequence_only_path, sequence_only_options);
    aastore::Store sequence_only_store(sequence_only_path);
    expect(sequence_only_built.name_bytes == 0 &&
               sequence_only_built.description_bytes == 0 &&
               !sequence_only_store.has_names() &&
               !sequence_only_store.has_descriptions() &&
               !std::filesystem::exists(sequence_only_path / "names.bin") &&
               !std::filesystem::exists(sequence_only_path / "names.meta") &&
               !std::filesystem::exists(
                   sequence_only_path / "descriptions.bin") &&
               !std::filesystem::exists(
                   sequence_only_path / "descriptions.meta") &&
               sequence_only_store.get(1).view(0) == expected[1],
           "sequence-only build omits both annotation columns");
    bool annotation_rejected = false;
    try {
        (void)sequence_only_store.batch_get_records(
            {1}, aastore::RecordFields::Name);
    } catch (const std::runtime_error&) {
        annotation_rejected = true;
    }
    expect(annotation_rejected,
           "sequence-only stores report missing annotations on demand");

    const std::filesystem::path names_only_path =
        temp.path() / "names_only_store";
    aastore::BuildOptions names_only_options;
    names_only_options.write_descriptions = false;
    const aastore::BuildStats names_only_built =
        aastore::build_fasta(
            fasta, names_only_path, names_only_options);
    aastore::Store names_only_store(names_only_path);
    const aastore::RecordBatch selected_names =
        names_only_store.batch_get_records(
            {1}, aastore::RecordFields::Name);
    expect(names_only_built.name_bytes == 43 &&
               names_only_built.description_bytes == 0 &&
               names_only_store.has_names() &&
               !names_only_store.has_descriptions() &&
               selected_names.name_view(0) == "p0" &&
               std::filesystem::exists(names_only_path / "names.bin") &&
               !std::filesystem::exists(
                   names_only_path / "descriptions.bin"),
           "name-only build exposes only the name column");

    const std::filesystem::path descriptions_only_path =
        temp.path() / "descriptions_only_store";
    aastore::BuildOptions descriptions_only_options;
    descriptions_only_options.write_names = false;
    descriptions_only_options.encode_threads = 4;
    const aastore::BuildStats descriptions_only_built =
        aastore::build_fasta(
            fasta, descriptions_only_path, descriptions_only_options);
    aastore::Store descriptions_only_store(descriptions_only_path);
    const aastore::RecordBatch selected_descriptions =
        descriptions_only_store.batch_get_records(
            {1}, aastore::RecordFields::Description);
    expect(descriptions_only_built.name_bytes == 0 &&
               descriptions_only_built.description_bytes == 11 &&
               !descriptions_only_store.has_names() &&
               descriptions_only_store.has_descriptions() &&
               selected_descriptions.description_view(0) == "description" &&
               !std::filesystem::exists(
                   descriptions_only_path / "names.bin") &&
               std::filesystem::exists(
                   descriptions_only_path / "descriptions.bin"),
           "description-only build exposes only the description column");

    bool zero_build_threads_rejected = false;
    try {
        aastore::BuildOptions invalid_build_options;
        invalid_build_options.encode_threads = 0;
        (void)aastore::build_fasta(
            fasta, temp.path() / "invalid_threads_store",
            invalid_build_options);
    } catch (const std::invalid_argument&) {
        zero_build_threads_rejected = true;
    }
    expect(zero_build_threads_rejected,
           "builder rejects zero encoding threads");

    names_only_options.overwrite = true;
    names_only_options.write_names = false;
    (void)aastore::build_fasta(
        fasta, names_only_path, names_only_options);
    aastore::Store overwritten_sequence_only(names_only_path);
    expect(!overwritten_sequence_only.has_names() &&
               !overwritten_sequence_only.has_descriptions() &&
               !std::filesystem::exists(names_only_path / "names.bin") &&
               !std::filesystem::exists(names_only_path / "names.meta"),
           "overwrite removes annotation columns disabled by the new build");

    const std::filesystem::path empty_fasta = temp.path() / "empty.fa";
    {
        std::ofstream output(empty_fasta);
    }
    const std::filesystem::path empty_store_path =
        temp.path() / "empty_store";
    const aastore::BuildStats empty_built =
        aastore::build_fasta(empty_fasta, empty_store_path);
    aastore::Store empty_store(empty_store_path);
    expect(empty_built.sequences == 0 && empty_store.size() == 0 &&
               empty_store.has_names() &&
               empty_store.has_descriptions() &&
               empty_store.batch_get_records({}).empty(),
           "zero-record stores retain valid compact text columns");

    bool overwrite_rejected = false;
    try {
        (void)aastore::build_fasta(fasta, store_path);
    } catch (const std::runtime_error&) {
        overwrite_rejected = true;
    }
    expect(overwrite_rejected, "overwrite is explicit");

}

// 随机读取、批次布局、规划、驻留模式及直接输出校验。
void StoreFixture::check_random([[maybe_unused]] bool io_uring_available) {
    aastore::ByteArena moved_from_arena;
    moved_from_arena.resize_uninitialized(1024);
    aastore::ByteArena moved_arena(std::move(moved_from_arena));
    moved_from_arena.resize_uninitialized(512);
    expect(moved_arena.size() == 1024 && moved_arena.data() != nullptr &&
               moved_from_arena.size() == 512 &&
               moved_from_arena.data() != nullptr,
           "byte arena remains reusable after move");

#if defined(AASTORE_TEST_IO_URING)
    if (io_uring_available) check_prefix_slices(temp.path());
#endif
    aastore::Store store(store_path);
    expect(!aastore::ReadOptions{}.collect_diagnostics, "read diagnostics default to disabled");
    expect(aastore::ReadOptions{}.merge_gap_bytes == 4096 &&
               aastore::ReadOptions{}.max_merged_range_bytes == 16384 &&
               aastore::ReadOptions{}.max_exact_merged_range_bytes ==
                   1024 * 1024,
           "default reader keeps the 4 KiB/16 KiB sparse plan");
    expect(store.size() == expected.size(), "opened sequence count");
    expect(store.has_names() && store.has_descriptions(),
           "opened name and description columns");
    for (size_t i = 0; i < expected.size(); ++i) {
        expect(store.get(i).view(0) == expected[i],
               "single get " + std::to_string(i));
        expect(store.sequence_length(i) == expected[i].size(),
               "sequence length " + std::to_string(i));
    }
    const std::vector<aastore::SequenceId> ids = {6, 1, 0, 3, 5, 1};
    const aastore::Batch batch = store.get(ids);
    expect(batch.size() == ids.size(), "batch size");
    expect(!batch.empty(), "batch is not empty");
    static_assert(std::is_same_v<decltype(batch[0]), std::string>,
                  "the default batch accessor returns an owning string");
    for (size_t i = 0; i < ids.size(); ++i) {
        expect(batch[i] == expected[ids[i]],
               "batch preserves request order " + std::to_string(i));
        expect(batch.storage[batch.offsets[i] + batch.lengths[i]] == '\0',
               "batch string terminator");
    }
    std::string owned_sequence;
    {
        const aastore::Batch temporary = store.get({1});
        owned_sequence = temporary[0];
    }
    expect(owned_sequence == expected[1],
           "owning accessor survives batch destruction");
    expect(batch.view(0) == expected[ids[0]],
           "explicit view accessor remains zero-copy");

    std::vector<aastore::SequenceId> planner_ids(70000);
    const std::vector<aastore::SequenceId> planner_pattern = {5, 1, 0, 6, 3};
    for (size_t i = 0; i < planner_ids.size(); ++i) {
        planner_ids[i] = planner_pattern[i % planner_pattern.size()];
    }
    aastore::ReadOptions serial_plan_options;
    serial_plan_options.collect_diagnostics = true;
    serial_plan_options.io_backend = aastore::IoBackend::Pread;
    serial_plan_options.threads = 1;
    aastore::ReadOptions parallel_plan_options = serial_plan_options;
    parallel_plan_options.threads = 8;
    const aastore::Batch serial_plan_batch =
        store.get(planner_ids, serial_plan_options);
    const aastore::Batch parallel_plan_batch =
        store.get(planner_ids, parallel_plan_options);
    expect(serial_plan_batch.stats.ranges ==
               parallel_plan_batch.stats.ranges &&
               serial_plan_batch.stats.merged_gaps ==
                   parallel_plan_batch.stats.merged_gaps &&
               serial_plan_batch.stats.free_merges ==
                   parallel_plan_batch.stats.free_merges &&
               serial_plan_batch.stats.read_bytes ==
                   parallel_plan_batch.stats.read_bytes &&
               serial_plan_batch.stats.unique_packed_bytes ==
                   parallel_plan_batch.stats.unique_packed_bytes &&
               serial_plan_batch.stats.overread_bytes ==
                   parallel_plan_batch.stats.overread_bytes &&
               serial_plan_batch.stats.max_range_bytes ==
                   parallel_plan_batch.stats.max_range_bytes,
           "parallel planner matches the serial sparse plan");
    for (size_t i : {size_t{0}, size_t{1}, size_t{2},
                     planner_ids.size() / 2, planner_ids.size() - 1}) {
        expect(parallel_plan_batch.view(i) == expected[planner_ids[i]] &&
                   parallel_plan_batch.view(i) == serial_plan_batch.view(i),
               "parallel planner preserves sequence " + std::to_string(i));
    }
    for (size_t mode = 0; mode < 2; ++mode) {
        aastore::ReadOptions serial_options = serial_plan_options;
        if (mode == 0) {
            serial_options.merge_gap_bytes = 0;
            serial_options.max_merged_range_bytes = 0;
        } else {
            serial_options.merge_gap_bytes = 4096;
            serial_options.max_merged_range_bytes = 4096;
        }
        aastore::ReadOptions parallel_options = serial_options;
        parallel_options.threads = 8;
        const aastore::Batch serial_result =
            store.get(planner_ids, serial_options);
        const aastore::Batch parallel_result =
            store.get(planner_ids, parallel_options);
        expect(serial_result.stats.ranges == parallel_result.stats.ranges &&
                   serial_result.stats.merged_gaps ==
                       parallel_result.stats.merged_gaps &&
                   serial_result.stats.free_merges ==
                       parallel_result.stats.free_merges &&
                   serial_result.stats.read_bytes ==
                       parallel_result.stats.read_bytes &&
                   serial_result.stats.unique_packed_bytes ==
                       parallel_result.stats.unique_packed_bytes &&
                   serial_result.stats.overread_bytes ==
                       parallel_result.stats.overread_bytes &&
                   serial_result.stats.max_range_bytes ==
                       parallel_result.stats.max_range_bytes,
               "parallel planner matches serial mode " +
                   std::to_string(mode));
    }

#ifdef AASTORE_TEST_IO_URING
    if (io_uring_available) {
        aastore::ReadOptions uring_options;
        uring_options.collect_diagnostics = true;
        uring_options.io_backend = aastore::IoBackend::IoUring;
        uring_options.threads = 3;
        uring_options.io_queue_depth = 4;
        uring_options.merge_gap_bytes = 0;
        const aastore::Batch uring_batch = store.get(ids, uring_options);
        for (size_t i = 0; i < ids.size(); ++i) {
            expect(uring_batch.view(i) == expected[ids[i]],
                   "io_uring batch preserves request order " +
                       std::to_string(i));
        }
        expect(uring_batch.stats.io_uring &&
                   uring_batch.stats.io_submissions != 0 &&
                   uring_batch.stats.io_completions != 0 &&
                   uring_batch.stats.io_workers == 3 &&
                   uring_batch.stats.decode_workers == 3 &&
                   uring_batch.stats.workers == 3 &&
                   uring_batch.stats.io_queue_depth <= 12 &&
                   uring_batch.stats.io_submit_calls != 0 &&
                   uring_batch.stats.io_empty_waits <= uring_batch.stats.io_wait_calls,
               "explicit io_uring backend completes bounded requests");
    }
#endif

    // 对齐后 p0 与 p1 的起始位置相差一个 4 KiB packed 页。
    aastore::ReadOptions sparse_options;
    sparse_options.collect_diagnostics = true;
    const aastore::Batch merged = store.get({1, 3}, sparse_options);
    expect(merged.stats.ranges == 1, "4 KiB gap merges");
    expect(merged.stats.merged_gaps == 1, "merged gap is counted");

    aastore::ReadOptions exact_options = sparse_options;
    exact_options.merge_gap_bytes = 0;
    const aastore::Batch exact = store.get({1, 3}, exact_options);
    expect(exact.stats.ranges == 2, "runtime zero gap keeps extents separate");

    aastore::ReadOptions capped_options = sparse_options;
    capped_options.max_merged_range_bytes = 4096;
    const aastore::Batch capped = store.get({1, 3}, capped_options);
    expect(capped.stats.ranges == 2,
           "maximum range prevents an oversized chained merge");

    // p2 再向后一个 packed 页，与 p0 的间隔超过 4 KiB。
    const aastore::Batch separate =
        store.get({1, 5}, sparse_options);
    expect(separate.stats.ranges == 2, "gap above 4 KiB stays separate");

    const aastore::Batch reordered = store.get({5, 1}, sparse_options);
    expect(reordered.stats.ranges == 2 &&
               reordered.view(0) == expected[5] &&
               reordered.view(1) == expected[1],
           "separate ranges preserve unsorted request order");

    aastore::StoreOptions full_options;
    full_options.storage_mode = aastore::StorageMode::FullMemory;
    full_options.threads = 2;
    aastore::Store full_store(store_path, full_options);
    expect(full_store.storage_mode() == aastore::StorageMode::FullMemory,
           "full-memory storage mode");
    expect(full_store.resident_bytes() >= full_store.packed_bytes(),
           "full-memory mode retains all packed data");
    const aastore::Batch full_batch = full_store.get(ids);
    for (size_t i = 0; i < ids.size(); ++i) {
        expect(full_batch.view(i) == expected[ids[i]],
               "full-memory batch preserves request order " +
                   std::to_string(i));
    }
    expect(full_batch.stats.full_memory &&
               full_batch.stats.sort_ns == 0 &&
               full_batch.stats.range_plan_ns == 0 &&
               full_batch.stats.ranges == 0 &&
               full_batch.stats.read_bytes == 0 &&
               full_batch.stats.read_ns == 0,
           "full-memory batch bypasses sort, planning, and I/O");
    for (size_t i = 0; i < expected.size(); ++i) {
        expect(full_store.get(i).view(0) == expected[i],
               "full-memory single get " + std::to_string(i));
    }
    for (const aastore::Store* reader : {&store, &full_store}) {
        std::string output;
        for (size_t i = expected.size(); i-- != 0;) {
            reader->get_into(i, output);
            expect(output == expected[i] && output.c_str()[output.size()] == '\0',
                   "single get_into reuses output across lengths and empty records");
        }
        bool invalid_rejected = false;
        try {
            reader->get_into(reader->size(), output);
        } catch (const std::out_of_range&) {
            invalid_rejected = true;
        }
        expect(invalid_rejected, "single get_into rejects invalid IDs");
    }
    aastore::StoreOptions prefix_options;
    prefix_options.storage_mode = aastore::StorageMode::PrefixMemory;
    prefix_options.cache_bytes = kPrefixCacheBytes;
    prefix_options.threads = 2;
    aastore::Store prefix_store(store_path, prefix_options);
    expect(prefix_store.storage_mode() == aastore::StorageMode::PrefixMemory &&
               prefix_store.resident_bytes() == kPrefixCacheBytes,
           "prefix-memory mode retains an immutable packed prefix");
    const std::vector<aastore::SequenceId> prefix_ids = {3, 1, 1, 5};
    const aastore::Batch prefix_batch = prefix_store.get(prefix_ids);
    for (size_t i = 0; i < prefix_ids.size(); ++i) {
        expect(prefix_batch.view(i) == expected[prefix_ids[i]],
               "prefix-memory batch preserves order " +
                   std::to_string(i));
    }
    expect(prefix_batch.stats.prefix_memory &&
               prefix_batch.stats.cache_hits == 2 &&
               prefix_batch.stats.cache_misses == 2 &&
               prefix_batch.stats.read_bytes != 0,
           "prefix-memory mode reads only the nonresident requests");
    const std::vector<aastore::SequenceId> mixed_prefix_ids =
        {7, 1, 2, 7, 0, 1, 5};
    for (aastore::IoBackend backend :
         {aastore::IoBackend::Pread, aastore::IoBackend::Auto}) {
        aastore::ReadOptions options;
        options.io_backend = backend;
        options.threads = 3;
        for (size_t round = 0; round < 3; ++round) {
            const aastore::Batch mixed =
                prefix_store.get(mixed_prefix_ids, options);
            for (size_t i = 0; i < mixed_prefix_ids.size(); ++i) {
                expect(mixed.view(i) == expected[mixed_prefix_ids[i]],
                       "prefix reads preserve duplicates and boundary-crossing sequences");
            }
            expect(mixed.stats.cache_hits == 2 &&
                       mixed.stats.cache_misses == 4 &&
                       mixed.stats.read_bytes != 0,
                   "repeated reads retain fixed prefix hit and miss sets");
            expect(prefix_store.resident_bytes() == kPrefixCacheBytes,
                   "nonresident reads do not grow or replace the prefix");
        }
    }
    const aastore::Batch prefix_hits = prefix_store.get({1, 0, 1});
    expect(prefix_hits.view(0) == expected[1] &&
               prefix_hits.view(1).empty() &&
               prefix_hits.view(2) == expected[1] &&
               prefix_hits.stats.cache_hits == 2 &&
               prefix_hits.stats.cache_misses == 0 &&
               prefix_hits.stats.read_bytes == 0,
           "fully resident prefix batches bypass file reads");

    for (const aastore::Store* source : {&store, &prefix_store, &full_store}) {
        for (const auto backend : {aastore::IoBackend::Pread, aastore::IoBackend::Auto}) {
            for (const bool diagnostics : {false, true}) {
                aastore::ReadOptions reads;
                reads.io_backend = backend;
                reads.threads = 8;
                reads.collect_diagnostics = diagnostics;
                for (const aastore::SequenceId id : {0, 1, 5, 7}) {
                    const auto single = source->get(id, reads);
                    const auto listed = source->get({id}, reads);
                    expect(single.size() == 1 && single.view(0) == expected[id] &&
                               single.view(0) == listed.view(0) &&
                               single.storage.size() == expected[id].size() + 1 &&
                               single.storage.data()[single.offsets[0] + single.lengths[0]] == '\0',
                           "single get returns an owning one-element terminated Batch");
                    expect(single.stats.requested_sequences == 1 &&
                               single.stats.requested_residues == expected[id].size() &&
                               single.stats.workers <= 1 &&
                               single.stats.read_bytes == listed.stats.read_bytes &&
                               single.stats.io_uring == listed.stats.io_uring,
                           "single and one-element list get share read options and statistics");
                    check_diagnostic_stats(single.stats, diagnostics);
                }
                expect(source->get({}, reads).empty(),
                       "empty initializer list reads no sequence, not ID zero");
            }
        }
        std::array<char, 16> guard;
        guard.fill('#');
        std::vector<char*> outputs{guard.data(), guard.data() + 8};
        bool count_rejected = false;
        try {
            source->get_into({1}, outputs);
        } catch (const std::invalid_argument&) {
            count_rejected = true;
        }
        bool array_rejected = false;
        try {
            source->get_into({1}, nullptr, 1);
        } catch (const std::invalid_argument&) {
            array_rejected = true;
        }
        bool slot_rejected = false;
        char* null_output = nullptr;
        try {
            source->get_into({1}, &null_output, 1);
        } catch (const std::invalid_argument&) {
            slot_rejected = true;
        }
        bool id_rejected = false;
        try {
            source->get_into({1, source->size()}, outputs);
        } catch (const std::out_of_range&) {
            id_rejected = true;
        }
        expect(count_rejected && array_rejected && slot_rejected && id_rejected &&
                   std::all_of(guard.begin(), guard.end(), [](char value) {
                       return value == '#';
                   }),
               "direct batch rejects invalid arrays and IDs before writing");
        const auto empty_stats = source->get_into({}, nullptr, 0);
        const auto zero_length_stats = source->get_into({0}, &null_output, 1);
        expect(empty_stats.requested_sequences == 0 && empty_stats.ranges == 0 &&
                   zero_length_stats.requested_sequences == 1 &&
                   zero_length_stats.requested_residues == 0 &&
                   zero_length_stats.ranges == 0,
               "direct batch accepts empty requests and null outputs for empty sequences");

    }
    const aastore::Batch empty = store.get({});
    expect(empty.empty() && empty.stats.ranges == 0,
           "empty batch");
    for (const aastore::Store* reader :
         {&store, &full_store, &prefix_store}) {
        std::string scratch;
        for (size_t i = expected.size(); i-- != 0;) {
            reader->get_into(i, scratch);
            expect(scratch == expected[i],
                   "single string reads preserve content");
        }
    }
    bool rejected = false;
    try {
        (void)store.get(store.size());
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    expect(rejected, "out-of-range id rejected");

}

// 连续区间、散布输出、长序列分区和驻留边界。
void StoreFixture::check_sequential() {
    check_sequential_into_partitions(temp.path(), false);
    aastore::Store store(store_path);
    aastore::Store full_store(store_path, full_store_options());
    aastore::Store prefix_store(store_path, prefix_store_options());
    aastore::ReadOptions sequential_options;
    sequential_options.threads = 8;
    const aastore::Batch sequential = store.sequential_get(
        0, store.size(), sequential_options);
    bool sequential_matches =
        sequential.size() == expected.size() &&
        sequential.storage.size() == built.residues &&
        sequential.stats.requested_residues == built.residues;
    for (size_t i = 0; i < expected.size(); ++i) {
        sequential_matches = sequential_matches &&
            sequential.view(i) == expected[i];
    }
    expect(sequential_matches,
           "sequential get decodes one continuous ordered arena");

    const aastore::Batch sequential_partial = store.sequential_get(
        1, store.size() - 1, sequential_options);
    bool sequential_partial_matches =
        sequential_partial.size() == expected.size() - 2;
    for (size_t i = 0; i < sequential_partial.size(); ++i) {
        sequential_partial_matches = sequential_partial_matches &&
            sequential_partial.view(i) == expected[i + 1];
    }
    expect(sequential_partial_matches,
           "sequential get preserves partial range boundaries");

    check_sequential_get_into(store, expected, 0, store.size(), sequential_options);
    check_sequential_get_into(store, expected, 1, store.size() - 1, sequential_options);

    for (const aastore::Store* source : {&store, &prefix_store, &full_store}) {
        for (size_t threads : {size_t{0}, size_t{1}, size_t{2}, size_t{8}}) {
            aastore::ReadOptions options;
            options.threads = threads;
            check_sequential_get_into(*source, expected, 0, source->size(), options);
            check_sequential_get_into(*source, expected, 1, source->size() - 1, options);
            check_sequential_get_into(*source, expected, source->size(), source->size(), options);
            check_sequential_get_into(*source, expected, 0, 1, options);
        }

        std::array<char, 16> guard;
        guard.fill('#');
        std::vector<char*> outputs{guard.data(), guard.data() + 8};
        char* null_output = nullptr;
        bool sequential_count_rejected = false;
        try {
            source->sequential_get_into(1, 2, outputs);
        } catch (const std::invalid_argument&) {
            sequential_count_rejected = true;
        }
        bool sequential_array_rejected = false;
        try {
            source->sequential_get_into(1, 2, nullptr, 1);
        } catch (const std::invalid_argument&) {
            sequential_array_rejected = true;
        }
        bool sequential_slot_rejected = false;
        std::vector<char*> bad_outputs{guard.data(), nullptr};
        try {
            source->sequential_get_into(1, 3, bad_outputs);
        } catch (const std::invalid_argument&) {
            sequential_slot_rejected = true;
        }
        bool sequential_range_rejected = false;
        try {
            source->sequential_get_into(2, 1, outputs);
        } catch (const std::out_of_range&) {
            sequential_range_rejected = true;
        }
        bool sequential_end_rejected = false;
        try {
            source->sequential_get_into(source->size(), source->size() + 1, outputs);
        } catch (const std::out_of_range&) {
            sequential_end_rejected = true;
        }
        expect(sequential_count_rejected && sequential_array_rejected &&
                   sequential_slot_rejected && sequential_range_rejected && sequential_end_rejected &&
                   std::all_of(guard.begin(), guard.end(), [](char value) { return value == '#'; }),
               "direct sequential validates all outputs and interval bounds before writing");
        const auto sequential_empty = source->sequential_get_into(source->size(), source->size(), nullptr, 0);
        const auto sequential_zero = source->sequential_get_into(0, 1, &null_output, 1);
        expect(sequential_empty.requested_sequences == 0 && sequential_empty.ranges == 0 &&
                   sequential_zero.requested_sequences == 1 && sequential_zero.requested_residues == 0,
               "direct sequential accepts empty intervals and null slots for empty sequences");
    }
    const aastore::Batch prefix_sequential = prefix_store.sequential_get(
        0, prefix_store.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        expect(prefix_sequential.view(i) == expected[i],
               "prefix sequential read crosses the resident boundary");
    }

}

// 记录字段选择、文本列统计及未请求字段的访问错误。
void StoreFixture::check_text() {
    aastore::Store store(store_path);
    aastore::Store full_store(store_path, full_store_options());
    const std::vector<aastore::SequenceId> ids = {6, 1, 0, 3, 5, 1};
    const aastore::RecordBatch records = store.batch_get_records(ids);
    expect(records.size() == ids.size() && !records.empty() &&
               records.fields() == aastore::RecordFields::All,
           "record batch contains all requested fields");
    for (size_t i = 0; i < ids.size(); ++i) {
        expect(records.name_view(i) == expected_names[ids[i]],
               "record batch name " + std::to_string(i));
        expect(records.description_view(i) ==
                   expected_descriptions[ids[i]],
               "record batch description " + std::to_string(i));
        expect(records.sequence_view(i) == expected[ids[i]],
               "record batch sequence " + std::to_string(i));
    }
    expect(records.names.stats.requested_strings == ids.size() &&
               records.names.stats.requested_bytes ==
                   expected_names[6].size() + expected_names[1].size() +
                       expected_names[0].size() + expected_names[3].size() +
                       expected_names[5].size() + expected_names[1].size(),
           "name column reports independent batch statistics");

    const aastore::RecordBatch names_only = store.batch_get_records(
        ids, aastore::RecordFields::Name);
    expect(names_only.names.size() == ids.size() &&
               names_only.descriptions.empty() &&
               names_only.sequences.empty(),
           "field mask avoids unused sequence and description reads");
    bool missing_field_rejected = false;
    try {
        (void)names_only.sequence_view(0);
    } catch (const std::logic_error&) {
        missing_field_rejected = true;
    }
    expect(missing_field_rejected,
           "record accessor rejects a field that was not requested");
    const aastore::RecordBatch full_records = full_store.batch_get_records(
        ids, aastore::RecordFields::Name |
                 aastore::RecordFields::Sequence);
    expect(full_records.name_view(0) == expected_names[ids[0]] &&
               full_records.sequence_view(0) == expected[ids[0]] &&
               full_records.sequences.stats.full_memory,
           "record batch reuses the selected sequence storage mode");
    const aastore::RecordBatch empty_records = store.batch_get_records({});
    expect(empty_records.empty() && empty_records.names.empty() &&
               empty_records.descriptions.empty() &&
               empty_records.sequences.empty(),
           "empty record batch");

    const auto text_fields = aastore::RecordFields::Name |
        aastore::RecordFields::Description;
    for (unsigned flags = 0; flags < 4; ++flags) {
        const auto path = temp.path() / ("resident_text_" + std::to_string(flags));
        std::filesystem::copy(store_path, path,
                              std::filesystem::copy_options::recursive);
        aastore::StoreOptions opening;
        opening.resident_names = (flags & 1) != 0;
        opening.resident_descriptions = (flags & 2) != 0;
        aastore::Store loaded(path, opening);
        const uint64_t resident_bytes =
            (opening.resident_names ? built.name_bytes : 0) +
            (opening.resident_descriptions ? built.description_bytes : 0);
        expect(loaded.resident_text_bytes() == resident_bytes,
               "resident text bytes count only enabled columns");

        for (const auto backend : {aastore::IoBackend::Pread, aastore::IoBackend::Auto}) {
            aastore::ReadOptions reading;
            reading.io_backend = backend;
            reading.threads = 4;
            const auto plain = loaded.batch_get_records(ids, text_fields, reading);
            reading.collect_diagnostics = true;
            const auto measured = loaded.batch_get_records(ids, text_fields, reading);
            const std::array<const aastore::TextBatch*, 2> plain_columns{
                &plain.names, &plain.descriptions};
            const std::array<const aastore::TextBatch*, 2> measured_columns{
                &measured.names, &measured.descriptions};
            for (size_t column = 0; column < plain_columns.size(); ++column) {
                const auto& before = plain_columns[column]->stats;
                const auto& after = measured_columns[column]->stats;
                expect(before.unique_bytes == 0 && before.merged_gaps == 0 &&
                           before.overread_bytes == 0 && after.unique_bytes > 0,
                       "text amplification diagnostics are optional for resident and disk columns");
                expect(before.resident == after.resident && before.io_uring == after.io_uring &&
                           before.requested_strings == after.requested_strings &&
                           before.requested_bytes == after.requested_bytes &&
                           before.ranges == after.ranges && before.read_bytes == after.read_bytes &&
                           before.max_range_bytes == after.max_range_bytes,
                       "text diagnostics preserve basic counts, backend and range sizes");
                expect(after.resident ? after.overread_bytes == 0 :
                           after.read_bytes == after.unique_bytes + after.overread_bytes,
                       "enabled text diagnostics report overread without underflow");
                for (size_t i = 0; i < ids.size(); ++i) {
                    expect(plain_columns[column]->view(i) == measured_columns[column]->view(i),
                           "text diagnostics preserve content, order and duplicates");
                }
            }
        }

        // 等长覆盖测试副本，区分自有缓冲与仍依赖源文件的只读映射。
        for (const char* filename : {"names.bin", "descriptions.bin"}) {
            const auto data_path = path / filename;
            const std::string replacement(
                static_cast<size_t>(std::filesystem::file_size(data_path)), '#');
            std::fstream output(data_path,
                                std::ios::binary | std::ios::in | std::ios::out);
            output.write(replacement.data(),
                         static_cast<std::streamsize>(replacement.size()));
            output.close();
            expect(static_cast<bool>(output), "overwrite test text data in place");
        }
        for (const auto backend : {aastore::IoBackend::Pread, aastore::IoBackend::Auto}) {
            aastore::ReadOptions reading;
            reading.io_backend = backend;
            reading.threads = 4;
            const auto result = loaded.batch_get_records(ids, text_fields, reading);
            expect(result.names.stats.resident == opening.resident_names &&
                       result.descriptions.stats.resident == opening.resident_descriptions,
                   "text columns independently report owned resident data");
            expect(!opening.resident_names ||
                       (result.names.stats.read_bytes == 0 && !result.names.stats.io_uring),
                   "resident names bypass file reads");
            expect(!opening.resident_descriptions ||
                       (result.descriptions.stats.read_bytes == 0 &&
                        !result.descriptions.stats.io_uring),
                   "resident descriptions bypass file reads");
            for (size_t i = 0; i < ids.size(); ++i) {
                const std::string name = opening.resident_names
                    ? expected_names[ids[i]] : std::string(expected_names[ids[i]].size(), '#');
                const std::string description = opening.resident_descriptions
                    ? expected_descriptions[ids[i]]
                    : std::string(expected_descriptions[ids[i]].size(), '#');
                expect(result.name_view(i) == name,
                       "resident name snapshot preserves input order and duplicates");
                expect(result.description_view(i) == description,
                       "resident description snapshot preserves empty entries and duplicates");
            }
        }
    }

    for (const bool no_records : {false, true}) {
        const auto path = temp.path() / (no_records ? "empty_text" : "empty_descriptions");
        const auto input_path = path.string() + ".fa";
        {
            std::ofstream input(input_path);
            if (!no_records) input << ">onlyname\nAC\n";
        }
        aastore::build_fasta(input_path, path);
        aastore::StoreOptions opening;
        opening.resident_names = true;
        opening.resident_descriptions = true;
        aastore::Store loaded(path, opening);
        const std::vector<aastore::SequenceId> selected = no_records
            ? std::vector<aastore::SequenceId>{} : std::vector<aastore::SequenceId>{0};
        const auto result = loaded.batch_get_records(selected, text_fields);
        expect(loaded.resident_text_bytes() == (no_records ? 0 : 8) &&
                   result.size() == selected.size() && result.names.stats.resident &&
                   result.descriptions.stats.resident,
               "empty resident text columns are valid");
        if (!no_records) {
            expect(result.name_view(0) == "onlyname" && result.description_view(0).empty(),
                   "resident reads handle a completely empty description column");
        }
    }
}

// 跨调用并发、总线程预算、并行直接输出和异常清理。
void StoreFixture::check_concurrency([[maybe_unused]] bool io_uring_available) {
    check_worker_exception_cleanup();
    check_sequential_into_partitions(temp.path(), true);
    aastore::Store store(store_path);
    aastore::Store full_store(store_path, full_store_options());
    aastore::Store prefix_store(store_path, prefix_store_options());
    std::array<bool, 8> concurrent_get_ok{};
    std::vector<std::thread> get_workers;
    ConcurrentGetContext get_ctx{
        &full_store, &expected, &concurrent_get_ok};
    for (size_t worker = 0; worker < concurrent_get_ok.size(); ++worker) {
        get_workers.emplace_back(run_concurrent_get, &get_ctx, worker);
    }
    for (std::thread& worker : get_workers) worker.join();
    expect(all_true(concurrent_get_ok.data(), concurrent_get_ok.size()),
           "full-memory get_into shares only immutable data across threads");

    const std::vector<aastore::SequenceId> mixed_prefix_ids = {7, 1, 2, 7, 0, 1, 5};
    std::vector<aastore::IoBackend> backends{
        aastore::IoBackend::Pread, aastore::IoBackend::Auto};
#ifdef AASTORE_TEST_IO_URING
    if (io_uring_available) backends.push_back(aastore::IoBackend::IoUring);
#endif
    const std::vector<std::vector<aastore::SequenceId>> thread_workloads = {
        mixed_prefix_ids, {1, 0, 1}, {3, 5, 7}, {}};
    for (aastore::IoBackend backend : backends) {
        for (size_t threads : {size_t{0}, size_t{1}, size_t{2}, size_t{8}}) {
            aastore::ReadOptions options;
            options.threads = threads;
            options.io_backend = backend;
            options.merge_gap_bytes = 0;
            options.io_queue_depth = 2;
            const size_t budget = std::max<size_t>(1, threads);
            for (aastore::Store* source : {&store, &prefix_store, &full_store}) {
                for (const auto& requested : thread_workloads) {
                    const auto batch = check_batch_diagnostics(*source, requested, options);
                    check_batch_get_into(*source, expected, requested, options);
                    expect(batch.stats.workers <= budget,
                           "batch workers stay within the total thread budget");
                    for (size_t i = 0; i < requested.size(); ++i) {
                        expect(batch.view(i) == expected[requested[i]],
                               "thread counts preserve batch contents and order");
                    }
                    if (batch.stats.io_uring) {
                        expect(batch.stats.io_workers == batch.stats.decode_workers &&
                                   batch.stats.workers == batch.stats.io_workers &&
                                   batch.stats.io_workers <= budget,
                               "ring owners also decode prefix and disk data");
                    }
                }
            }
        }
    }
    std::vector<aastore::SequenceId> planner_ids(70000);
    const std::vector<aastore::SequenceId> planner_pattern = {5, 1, 0, 6, 3};
    for (size_t i = 0; i < planner_ids.size(); ++i) {
        planner_ids[i] = planner_pattern[i % planner_pattern.size()];
    }
    aastore::ReadOptions parallel_plan_options;
    parallel_plan_options.io_backend = aastore::IoBackend::Pread;
    parallel_plan_options.threads = 8;
    for (const aastore::Store* source : {&store, &prefix_store, &full_store}) {
        check_batch_get_into(*source, expected, planner_ids, parallel_plan_options);
    }
}

}  // namespace

// 不带参数保留完整套件；CTest 按组运行，允许单独重现某一类失败。
int main(int argc, char* argv[]) {
    const std::array<std::string_view, 5> groups{
        "builder", "random", "sequential", "text", "concurrency"};
    std::string_view selected;
    if (argc != 1) {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: aastore_test [--group <builder|random|sequential|text|concurrency>]\n";
            return 0;
        }
        if (argc != 3 || std::string_view(argv[1]) != "--group" ||
            std::find(groups.begin(), groups.end(), std::string_view(argv[2])) == groups.end()) {
            std::cerr << "Usage: aastore_test [--group <builder|random|sequential|text|concurrency>]\n";
            return 2;
        }
        selected = argv[2];
    }

    bool io_uring_available = false;
#if defined(AASTORE_TEST_IO_URING)
    if (selected.empty() || selected == "random" || selected == "concurrency") {
        io_uring_available = probe_io_uring();
    }
#endif
    StoreFixture fixture;
    for (const auto group : groups) {
        if (!selected.empty() && selected != group) continue;
        std::cout << "aastore test group: " << group << '\n';
        if (group == "builder") fixture.check_builder();
        else if (group == "random") {
            check_planner_diagnostics();
            fixture.check_random(io_uring_available);
        }
        else if (group == "sequential") fixture.check_sequential();
        else if (group == "text") fixture.check_text();
        else fixture.check_concurrency(io_uring_available);
    }
    if (failures != 0) {
        std::cerr << "aastore test: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "aastore test: ok\n";
    return 0;
}
