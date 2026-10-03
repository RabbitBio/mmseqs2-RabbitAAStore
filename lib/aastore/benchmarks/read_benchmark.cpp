// 文件职责：生成随机或顺序请求，比较不同驻留模式和 I/O 后端的读取成本。
// 支持按字节分批、文件冷缓存验证，并输出耗时、内存、I/O 及有序结果校验值。
#include "aastore/store.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Arguments {
    std::filesystem::path store;
    uint64_t requests = 1000000;
    uint64_t rounds = 1;
    uint64_t seed = 12345;
    long double sample_percent = 0;
    bool requests_set = false;
    uint64_t batch_bytes = 0;
    bool sequential = false;
    bool fast_sequential = false;
    std::filesystem::path ids_file;
    size_t threads = 8;
    size_t queue_depth = 64;
    aastore::IoBackend io_backend = aastore::IoBackend::Auto;
    bool drop_cache = false;
    int64_t memory_mb = 0;
    bool memory_mb_set = false;
    uint64_t memory_bytes = 0;
    bool memory_bytes_set = false;
    uint64_t io_buffer_bytes = aastore::kIoBufferBytes;
    uint64_t merge_gap_bytes = aastore::kByteRangeMergeGapBytes;
    uint64_t max_range_bytes = aastore::kMaxMergedRangeBytes;
    uint64_t max_exact_range_bytes = aastore::kMaxExactMergedRangeBytes;
};

// 解析 uint64_t 参数，并要求整个输入字符串均可转换。
uint64_t parse_u64(const char* text, const char* name) {
    size_t consumed = 0;
    const std::string value(text);
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    const uint64_t result = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return result;
}

// 解析有符号 64 位参数，供含负值约定的内存选项使用。
int64_t parse_i64(const char* text, const char* name) {
    size_t consumed = 0;
    const std::string value(text);
    const int64_t result = std::stoll(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return result;
}

// 读取选项后的参数值，缺失时报告选项名称。
const char* next_argument(int argc, char** argv, int* index,
                          const char* name) {
    ++*index;
    if (*index >= argc) {
        throw std::invalid_argument(
            std::string("missing value for ") + name);
    }
    return argv[*index];
}

// 解析读取基准的请求生成、存储模式、I/O 后端和范围规划选项。
Arguments parse_arguments(int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument(
            "usage: aastore_bench STORE [--requests N] [--seed N] "
            "[--rounds N] [--threads N] [--drop-cache] [--memory-mb N] "
            "[--memory-bytes N] "
            "[--sample-percent P] [--batch-bytes N] "
            "[--sequential] [--fast-sequential] "
            "[--ids-file PATH] "
            "[--full-memory] [--io-backend auto|pread|uring] "
            "[--queue-depth N] "
            "[--io-buffer-bytes N] "
            "[--merge-gap N] [--max-range N] "
            "[--max-exact-range N]");
    }
    Arguments arguments;
    arguments.store = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "--requests") {
            arguments.requests_set = true;
            arguments.requests = parse_u64(
                next_argument(argc, argv, &i, "--requests"), "requests");
        } else if (option == "--sample-percent") {
            const std::string value(next_argument(argc, argv, &i, "--sample-percent"));
            size_t consumed = 0;
            arguments.sample_percent = std::stold(value, &consumed);
            if (consumed != value.size() || !std::isfinite(arguments.sample_percent) ||
                arguments.sample_percent <= 0 || arguments.sample_percent > 100) {
                throw std::invalid_argument("sample percent must be in (0, 100]");
            }
        } else if (option == "--batch-bytes") {
            arguments.batch_bytes = parse_u64(
                next_argument(argc, argv, &i, "--batch-bytes"), "batch bytes");
            if (arguments.batch_bytes == 0) {
                throw std::invalid_argument("batch bytes must be positive");
            }
        } else if (option == "--rounds") {
            arguments.rounds = parse_u64(
                next_argument(argc, argv, &i, "--rounds"), "rounds");
        } else if (option == "--seed") {
            arguments.seed = parse_u64(
                next_argument(argc, argv, &i, "--seed"), "seed");
        } else if (option == "--sequential") {
            arguments.sequential = true;
        } else if (option == "--fast-sequential") {
            arguments.sequential = true;
            arguments.fast_sequential = true;
        } else if (option == "--ids-file") {
            arguments.ids_file =
                next_argument(argc, argv, &i, "--ids-file");
        } else if (option == "--threads") {
            arguments.threads = static_cast<size_t>(
                parse_u64(next_argument(
                    argc, argv, &i, "--threads"), "threads"));
        } else if (option == "--queue-depth") {
            arguments.queue_depth = static_cast<size_t>(
                parse_u64(next_argument(
                    argc, argv, &i, "--queue-depth"), "queue depth"));
        } else if (option == "--io-buffer-bytes") {
            arguments.io_buffer_bytes = parse_u64(
                next_argument(argc, argv, &i, "--io-buffer-bytes"),
                "I/O buffer bytes");
        } else if (option == "--io-backend") {
            const std::string backend(
                next_argument(argc, argv, &i, "--io-backend"));
            if (backend == "auto") {
                arguments.io_backend = aastore::IoBackend::Auto;
            } else if (backend == "pread") {
                arguments.io_backend = aastore::IoBackend::Pread;
            } else if (backend == "uring" || backend == "io_uring") {
                arguments.io_backend = aastore::IoBackend::IoUring;
            } else {
                throw std::invalid_argument(
                    "I/O backend must be auto, pread, or uring");
            }
        } else if (option == "--drop-cache") {
            arguments.drop_cache = true;
        } else if (option == "--memory-mb") {
            arguments.memory_mb_set = true;
            arguments.memory_mb = parse_i64(
                next_argument(argc, argv, &i, "--memory-mb"),
                "memory budget");
        } else if (option == "--full-memory") {
            arguments.memory_mb_set = true;
            arguments.memory_mb = -1;
        } else if (option == "--memory-bytes") {
            arguments.memory_bytes_set = true;
            arguments.memory_bytes = parse_u64(
                next_argument(argc, argv, &i, "--memory-bytes"), "memory bytes");
        } else if (option == "--merge-gap") {
            arguments.merge_gap_bytes =
                parse_u64(next_argument(
                    argc, argv, &i, "--merge-gap"), "merge gap");
        } else if (option == "--max-range") {
            arguments.max_range_bytes =
                parse_u64(next_argument(
                    argc, argv, &i, "--max-range"), "maximum range");
        } else if (option == "--max-exact-range") {
            arguments.max_exact_range_bytes = parse_u64(
                next_argument(argc, argv, &i, "--max-exact-range"),
                "maximum exact range");
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    arguments.threads = std::max<size_t>(1, arguments.threads);
    arguments.queue_depth = std::max<size_t>(1, arguments.queue_depth);
    if (arguments.rounds == 0) {
        throw std::invalid_argument("rounds must be greater than zero");
    }
    if (arguments.rounds > static_cast<uint64_t>(
            std::numeric_limits<size_t>::max())) {
        throw std::length_error("too many rounds");
    }
    if (arguments.memory_mb < -1) {
        throw std::invalid_argument("memory budget must be -1 or nonnegative");
    }
    if (arguments.memory_mb_set && arguments.memory_bytes_set) {
        throw std::invalid_argument("memory bytes cannot be combined with memory-mb or full-memory");
    }
    if (arguments.sample_percent != 0 &&
        (arguments.requests_set || arguments.sequential || !arguments.ids_file.empty())) {
        throw std::invalid_argument(
            "sample percent cannot be combined with requests, sequential, or ids-file");
    }
    if (arguments.batch_bytes != 0 && arguments.fast_sequential) {
        throw std::invalid_argument("batch bytes is for the random batch interface");
    }
    if (arguments.drop_cache && arguments.rounds != 1) {
        throw std::invalid_argument("cold runs require rounds=1 and a new process per repeat");
    }
    return arguments;
}

// 返回基准支持的存储模式标签，未匹配模式返回 unknown。
const char* storage_mode_name(aastore::StorageMode mode) {
    switch (mode) {
        case aastore::StorageMode::ByteRange: return "byte_range";
        case aastore::StorageMode::PrefixMemory: return "prefix_memory";
        case aastore::StorageMode::FullMemory: return "full_memory";
    }
    return "unknown";
}

// 返回命令行请求的 I/O 后端标签。
const char* requested_io_backend_name(aastore::IoBackend backend) {
    switch (backend) {
        case aastore::IoBackend::Auto: return "auto";
        case aastore::IoBackend::Pread: return "pread";
        case aastore::IoBackend::IoUring: return "io_uring";
    }
    return "unknown";
}

// 根据本批统计给出实际使用的 I/O 后端标签。
const char* active_io_backend_name(const aastore::BatchStats& stats) {
    if (stats.ranges == 0) return "none";
    return stats.io_uring ? "io_uring" : "pread";
}

// 推进 SplitMix64 状态并返回可复现的伪随机整数。
uint64_t splitmix64(uint64_t& state) {
    uint64_t value = (state += 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

// 生成顺序循环或伪随机 ID 列表，用于可复现的读取负载。
std::vector<aastore::SequenceId> make_ids(uint64_t size,
                                          uint64_t count,
                                          uint64_t seed,
                                          bool sequential) {
    if (size == 0 && count != 0) throw std::runtime_error("empty store");
    if (count > static_cast<uint64_t>(
            std::numeric_limits<size_t>::max())) {
        throw std::length_error("too many requests");
    }
    std::vector<aastore::SequenceId> ids(static_cast<size_t>(count));
    if (sequential) {
        for (uint64_t i = 0; i < count; ++i) {
            ids[static_cast<size_t>(i)] = i % size;
        }
        return ids;
    }
    uint64_t state = seed;
    for (aastore::SequenceId& id : ids) id = splitmix64(state) % size;
    return ids;
}

// 从文本文件读取 ID 列表，格式错误时报告异常。
std::vector<aastore::SequenceId> read_ids_file(
        const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open ids file " + path.string());
    }
    std::vector<aastore::SequenceId> ids;
    uint64_t id = 0;
    while (input >> id) ids.push_back(id);
    if (!input.eof()) {
        throw std::invalid_argument("invalid sequence id in " + path.string());
    }
    return ids;
}

// 使用 Floyd 算法无放回抽样后打乱请求顺序，仅需与抽样数成正比的内存。
std::vector<aastore::SequenceId> sample_ids(uint64_t size, long double percent,
                                           uint64_t seed) {
    const uint64_t count = size == 0 ? 0 : std::max<uint64_t>(
        1, static_cast<uint64_t>(std::floor(size * percent / 100)));
    std::vector<aastore::SequenceId> ids;
    ids.reserve(static_cast<size_t>(count));
    std::unordered_set<uint64_t> selected;
    selected.reserve(static_cast<size_t>(count));
    std::mt19937_64 random(seed);
    for (uint64_t j = size - count; j < size; ++j) {
        const uint64_t candidate = std::uniform_int_distribution<uint64_t>(0, j)(random);
        const uint64_t id = selected.insert(candidate).second ? candidate : j;
        selected.insert(id);
        ids.push_back(id);
    }
    std::shuffle(ids.begin(), ids.end(), random);
    return ids;
}

// 通过 mmap 和 mincore 查询文件的驻留页数，不读取映射内容以免主动预热。
uint64_t resident_pages(int descriptor, size_t bytes) {
    if (bytes == 0) return 0;
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) throw std::runtime_error("cannot get page size");
    void* mapping = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) throw std::runtime_error("cannot map cache probe");
    uint64_t resident = 0;
    try {
        const size_t pages = bytes / page_size + (bytes % page_size != 0);
        std::vector<unsigned char> residency(std::min<size_t>(pages, 1024 * 1024));
        for (size_t first = 0; first < pages; first += residency.size()) {
            const size_t count = std::min(residency.size(), pages - first);
            const size_t offset = first * page_size;
            if (::mincore(static_cast<char*>(mapping) + offset,
                          std::min(bytes - offset, count * page_size), residency.data()) != 0) {
                throw std::runtime_error("mincore failed during cold-cache verification");
            }
            for (size_t i = 0; i < count; ++i) resident += (residency[i] & 1);
        }
    } catch (...) {
        ::munmap(mapping, bytes);
        throw;
    }
    ::munmap(mapping, bytes);
    return resident;
}

// 打开 Store 前同步并请求驱逐指定文件缓存，再验证驻留页数，不清理其他文件。
void drop_file_cache(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return;
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open cache target " + path.string());
    }
#ifdef POSIX_FADV_DONTNEED
    try {
        struct stat status {};
        if (::fstat(descriptor, &status) != 0 || ::fsync(descriptor) != 0) {
            throw std::runtime_error("cannot sync cache target " + path.string());
        }
        bool cold = false;
        for (size_t attempt = 0; attempt < 3; ++attempt) {
            const int result = ::posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED);
            if (result != 0) {
                throw std::runtime_error("cannot drop cache for " + path.string() +
                                         ": " + std::strerror(result));
            }
            if (resident_pages(descriptor, static_cast<size_t>(status.st_size)) == 0) {
                cold = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!cold) throw std::runtime_error("file is still cached: " + path.string());
    } catch (...) {
        ::close(descriptor);
        throw;
    }
    ::close(descriptor);
#else
    ::close(descriptor);
    throw std::runtime_error("cold-cache eviction is not supported");
#endif
}

// 计算从指定时刻到现在经过的毫秒数。
double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
}

// Linux 进程存储读取计数，包含预读；与应用请求的 read_bytes 分开报告。
bool storage_read_bytes(uint64_t& bytes) {
    std::ifstream input("/proc/self/io");
    std::string key;
    uint64_t value = 0;
    while (input >> key >> value) {
        if (key == "read_bytes:") {
            bytes = value;
            return true;
        }
    }
    return false;
}

// 将 timeval 中的秒和微秒统一换算为毫秒。
double time_ms(const timeval& value) {
    return value.tv_sec * 1000.0 + value.tv_usec / 1000.0;
}

constexpr uint64_t kHashBasis = 14695981039346656037ULL;

// 将一个字节累入 FNV-1a 64 位校验值。
void hash_byte(uint64_t& hash, uint8_t byte) {
    hash = (hash ^ byte) * 1099511628211ULL;
}

// 按小端字节序将一个 64 位整数累入校验值，避免依赖机器端序。
void hash_number(uint64_t& hash, uint64_t number) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash_byte(hash, static_cast<uint8_t>(number >> shift));
    }
}

// 合并批次统计：累计计数和耗时、保留峰值，并汇总实际使用过的模式标志。
void accumulate(aastore::BatchStats& total, const aastore::BatchStats& part) {
    using S = aastore::BatchStats;
    static constexpr uint64_t S::* sums[] = {
        &S::requested_sequences, &S::requested_residues,
        &S::cache_hits, &S::cache_misses, &S::cache_hit_residues,
        &S::cache_miss_residues, &S::unique_packed_bytes, &S::ranges,
        &S::merged_gaps, &S::free_merges,
        &S::read_bytes, &S::overread_bytes, &S::io_submissions,
        &S::io_completions, &S::io_submit_calls, &S::io_submit_ns,
        &S::io_wait_calls, &S::io_empty_waits, &S::layout_ns, &S::sort_ns,
        &S::range_plan_ns, &S::worker_wall_ns, &S::worker_max_busy_ns,
        &S::read_ns, &S::decode_ns, &S::cache_decode_ns
    };
    static constexpr uint64_t S::* maxima[] = {
        &S::max_range_bytes, &S::workers, &S::io_workers,
        &S::decode_workers, &S::io_queue_depth
    };
    for (auto member : sums) total.*member += part.*member;
    for (auto member : maxima) total.*member = std::max(total.*member, part.*member);
    total.full_memory |= part.full_memory;
    total.prefix_memory |= part.prefix_memory;
    total.io_uring |= part.io_uring;
    total.diagnostics_collected |= part.diagnostics_collected;
}

}  // namespace

// 执行配置的读取基准轮次，并输出吞吐量、内存和 I/O 统计。
int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        if (arguments.drop_cache) {
            drop_file_cache(arguments.store / "packed.bin");
            drop_file_cache(arguments.store / "meta.bin");
        }

        const auto total_start = Clock::now();
        uint64_t open_storage_before = 0, open_storage_after = 0;
        const bool have_open_before = storage_read_bytes(open_storage_before);
        const auto open_start = Clock::now();
        aastore::StoreOptions store_options;
        if (arguments.memory_mb < 0) {
            store_options.storage_mode = aastore::StorageMode::FullMemory;
        } else if (arguments.memory_bytes_set && arguments.memory_bytes > 0) {
            store_options.storage_mode = aastore::StorageMode::PrefixMemory;
            store_options.cache_bytes = arguments.memory_bytes;
        } else if (arguments.memory_mb > 0) {
            store_options.storage_mode = aastore::StorageMode::PrefixMemory;
            const uint64_t memory_mb =
                static_cast<uint64_t>(arguments.memory_mb);
            if (memory_mb >
                std::numeric_limits<uint64_t>::max() / (1024ULL * 1024ULL)) {
                throw std::length_error("memory budget is too large");
            }
            store_options.cache_bytes = memory_mb * 1024ULL * 1024ULL;
        }
        store_options.threads = arguments.threads;
        aastore::Store store(arguments.store, store_options);
        const double open_ms = elapsed_ms(open_start);
        const bool have_open_after = storage_read_bytes(open_storage_after);
        const auto prepare_start = Clock::now();
        const std::vector<aastore::SequenceId> ids =
            arguments.sample_percent != 0
                ? sample_ids(store.size(), arguments.sample_percent, arguments.seed)
                : arguments.ids_file.empty()
                ? make_ids(store.size(), arguments.requests, arguments.seed,
                           arguments.sequential)
                : read_ids_file(arguments.ids_file);
        if (arguments.fast_sequential &&
            (!arguments.ids_file.empty() || ids.size() > store.size())) {
            throw std::invalid_argument(
                "fast sequential mode requires a prefix within the store");
        }
        std::vector<size_t> batch_ends;
        uint64_t current_bytes = 0, max_batch_bytes = 0;
        uint64_t trace_hash = kHashBasis;
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto id = ids[i];
            if (id >= store.size()) {
                throw std::out_of_range("sequence id exceeds store size");
            }
            hash_number(trace_hash, id);
            const uint64_t length = store.sequence_length(id);
            if (length == std::numeric_limits<uint64_t>::max()) {
                throw std::length_error("sequence is too long");
            }
            const uint64_t bytes = length + 1;
            if (arguments.batch_bytes && current_bytes &&
                (bytes > arguments.batch_bytes ||
                 current_bytes > arguments.batch_bytes - bytes)) {
                batch_ends.push_back(i);
                max_batch_bytes = std::max(max_batch_bytes, current_bytes);
                current_bytes = 0;
            }
            if (bytes > std::numeric_limits<uint64_t>::max() - current_bytes) {
                throw std::length_error("batch is too large");
            }
            current_bytes += bytes;
        }
        if (!ids.empty()) batch_ends.push_back(ids.size());
        max_batch_bytes = std::max(max_batch_bytes, current_bytes);
        double prepare_ms = elapsed_ms(prepare_start);
        aastore::ReadOptions options;
        options.collect_diagnostics = true;
        options.threads = arguments.threads;
        options.io_backend = arguments.io_backend;
        options.io_queue_depth = arguments.queue_depth;
        options.io_buffer_bytes = arguments.io_buffer_bytes;
        options.merge_gap_bytes = arguments.merge_gap_bytes;
        options.max_merged_range_bytes = arguments.max_range_bytes;
        options.max_exact_merged_range_bytes = arguments.max_exact_range_bytes;
        aastore::BatchStats totals;
        double fetch_ms = 0;
        double fetch_user_ms = 0;
        double fetch_system_ms = 0;
        uint64_t fetch_storage_bytes = 0;
        bool storage_bytes_available = true;
        bool used_pread = false, used_uring = false;
        uint64_t sum = 0, output_hash = kHashBasis;
        double consume_ms = 0;
        std::vector<double> round_fetch_ms;
        std::vector<aastore::BatchStats> round_stats;
        round_fetch_ms.reserve(static_cast<size_t>(arguments.rounds));
        round_stats.reserve(static_cast<size_t>(arguments.rounds));
        for (uint64_t round = 0; round < arguments.rounds; ++round) {
            aastore::BatchStats stats;
            double round_ms = 0;
            uint64_t round_sum = 0, round_hash = kHashBasis;
            size_t begin = 0;
            for (size_t end : batch_ends) {
                const auto copy_start = Clock::now();
                const std::vector<aastore::SequenceId> batch_ids(
                    ids.begin() + begin, ids.begin() + end);
                prepare_ms += elapsed_ms(copy_start);
                uint64_t storage_before = 0;
                uint64_t storage_after = 0;
                const bool have_before = storage_read_bytes(storage_before);
                struct rusage usage_before {}, usage_after {};
                ::getrusage(RUSAGE_SELF, &usage_before);
                const auto fetch_start = Clock::now();
                aastore::Batch current;
                if (arguments.fast_sequential) {
                    current = store.sequential_get(
                        0, static_cast<aastore::SequenceId>(ids.size()), options);
                } else {
                    current = store.get(batch_ids, options);
                }
                round_ms += elapsed_ms(fetch_start);
                ::getrusage(RUSAGE_SELF, &usage_after);
                const bool have_after = storage_read_bytes(storage_after);
                if (have_before && have_after && storage_after >= storage_before) {
                    fetch_storage_bytes += storage_after - storage_before;
                } else {
                    storage_bytes_available = false;
                }
                fetch_user_ms += time_ms(usage_after.ru_utime) -
                                 time_ms(usage_before.ru_utime);
                fetch_system_ms += time_ms(usage_after.ru_stime) -
                                   time_ms(usage_before.ru_stime);
                if (current.stats.ranges) {
                    used_uring |= current.stats.io_uring;
                    used_pread |= !current.stats.io_uring;
                }
                accumulate(stats, current.stats);
                const auto consume_start = Clock::now();
                if (current.size() != batch_ids.size()) {
                    throw std::runtime_error("returned batch size mismatch");
                }
                for (size_t i = 0; i < current.size(); ++i) {
                    const auto view = current.view(i);
                    if (view.size() != store.sequence_length(batch_ids[i])) {
                        throw std::runtime_error("returned sequence length mismatch");
                    }
                    hash_number(round_hash, batch_ids[i]);
                    hash_number(round_hash, view.size());
                    for (unsigned char byte : view) {
                        round_sum += byte;
                        hash_byte(round_hash, byte);
                    }
                }
                consume_ms += elapsed_ms(consume_start);
                begin = end;
            }
            if (round != 0 && (round_hash != output_hash || round_sum != sum)) {
                throw std::runtime_error("output differs between rounds");
            }
            sum = round_sum;
            output_hash = round_hash;
            fetch_ms += round_ms;
            round_fetch_ms.push_back(round_ms);
            round_stats.push_back(stats);
            accumulate(totals, stats);
        }

        struct rusage usage {};
        ::getrusage(RUSAGE_SELF, &usage);
        std::cout << "store_size=" << store.size() << '\n'
                  << "requests=" << ids.size() << '\n'
                  << "sampling=" << (arguments.sample_percent != 0
                          ? "without_replacement" : "legacy") << '\n'
                  << "sample_percent=" << arguments.sample_percent << '\n'
                  << "seed=" << arguments.seed << '\n'
                  << "trace_hash_fnv1a64=" << trace_hash << '\n'
                  << "output_hash_fnv1a64=" << output_hash << '\n'
                  << "cold_cache_verified=" << arguments.drop_cache << '\n'
                  << "batch_limit_bytes=" << arguments.batch_bytes << '\n'
                  << "batches_per_round=" << batch_ends.size() << '\n'
                  << "max_batch_output_bytes=" << max_batch_bytes << '\n'
                  << "id_pattern="
                  << (arguments.ids_file.empty()
                          ? (arguments.fast_sequential
                                 ? "fast_sequential_range"
                                 : (arguments.sequential
                                        ? "sequential"
                                        : "random"))
                          : "ids_file") << '\n'
                  << "residues=" << totals.requested_residues << '\n'
                  << "checksum=" << sum << '\n'
                  << "threads=" << arguments.threads << '\n'
                  << "collect_diagnostics=" << options.collect_diagnostics << '\n'
                  << "diagnostics_collected=" << totals.diagnostics_collected << '\n'
                  << "workers=" << totals.workers << '\n'
                  << "requested_io_backend="
                  << requested_io_backend_name(arguments.io_backend) << '\n'
                  << "requested_queue_depth="
                  << arguments.queue_depth << '\n'
                  << "io_buffer_limit_bytes="
                  << arguments.io_buffer_bytes << '\n'
                  << "rounds=" << arguments.rounds << '\n'
                  << "storage_mode=" << storage_mode_name(
                         store.storage_mode()) << '\n'
                  << "resident_bytes=" << store.resident_bytes() << '\n'
                  << "packed_bytes=" << store.packed_bytes() << '\n'
                  << "prefix_budget_bytes=" << store_options.cache_bytes << '\n'
                  << "preload_ms=" << store.preload_ns() / 1.0e6 << '\n'
                  << "decode_kernel=" << store.decode_kernel_name() << '\n'
                  << "merge_gap_bytes="
                  << arguments.merge_gap_bytes << '\n'
                  << "max_range_limit_bytes="
                  << arguments.max_range_bytes << '\n'
                  << "max_exact_range_limit_bytes="
                  << arguments.max_exact_range_bytes << '\n'
                  << "ranges=" << totals.ranges << '\n'
                  << "free_merges=" << totals.free_merges << '\n'
                  << "active_io_backend="
                  << (used_pread && used_uring ? "mixed"
                          : active_io_backend_name(totals)) << '\n'
                  << "io_queue_depth="
                  << totals.io_queue_depth << '\n'
                  << "io_workers=" << totals.io_workers << '\n'
                  << "decode_workers=" << totals.decode_workers << '\n'
                  << "io_submissions="
                  << totals.io_submissions << '\n'
                  << "io_completions="
                  << totals.io_completions << '\n'
                  << "io_submit_calls=" << totals.io_submit_calls << '\n'
                  << "io_submit_ms=" << totals.io_submit_ns / 1.0e6 << '\n'
                  << "io_wait_calls=" << totals.io_wait_calls << '\n'
                  << "io_empty_waits=" << totals.io_empty_waits << '\n'
                  << "max_range_bytes=" << totals.max_range_bytes << '\n'
                  << "read_bytes=" << totals.read_bytes << '\n'
                  << "unique_packed_bytes="
                  << totals.unique_packed_bytes << '\n'
                  << "overread_bytes=" << totals.overread_bytes << '\n'
                  << "cache_hits=" << totals.cache_hits << '\n'
                  << "cache_misses=" << totals.cache_misses << '\n'
                  << "cache_hit_residues="
                  << totals.cache_hit_residues << '\n'
                  << "cache_miss_residues="
                  << totals.cache_miss_residues << '\n'
                  << "open_ms=" << open_ms << '\n'
                  << "open_storage_read_bytes="
                  << (have_open_before && have_open_after &&
                          open_storage_after >= open_storage_before
                          ? std::to_string(open_storage_after - open_storage_before)
                          : "unavailable") << '\n'
                  << "prepare_ms=" << prepare_ms << '\n'
                  << "fetch_ms=" << fetch_ms << '\n'
                  << "service_ms=" << open_ms + fetch_ms << '\n'
                  << "fetch_user_ms=" << fetch_user_ms << '\n'
                  << "fetch_system_ms=" << fetch_system_ms << '\n'
                  << "fetch_storage_read_bytes="
                  << (storage_bytes_available
                          ? std::to_string(fetch_storage_bytes)
                          : "unavailable") << '\n'
                  << "consume_ms=" << consume_ms << '\n'
                  << "end_to_end_ms=" << elapsed_ms(total_start) << '\n'
                  << "layout_ms=" << totals.layout_ns / 1.0e6 << '\n'
                  << "sort_ms=" << totals.sort_ns / 1.0e6 << '\n'
                  << "range_plan_ms="
                  << totals.range_plan_ns / 1.0e6 << '\n'
                  << "worker_wall_ms="
                  << totals.worker_wall_ns / 1.0e6 << '\n'
                  << "worker_max_busy_ms="
                  << totals.worker_max_busy_ns / 1.0e6 << '\n'
                  << "worker_read_ms=" << totals.read_ns / 1.0e6 << '\n'
                  << "worker_decode_ms="
                  << totals.decode_ns / 1.0e6 << '\n'
                  << "cache_decode_ms="
                  << totals.cache_decode_ns / 1.0e6 << '\n'
                  << "peak_rss_kib=" << usage.ru_maxrss << '\n';
        for (size_t round = 0; round < round_stats.size(); ++round) {
            const aastore::BatchStats& stats = round_stats[round];
            const size_t number = round + 1;
            std::cout << "round_" << number << "_fetch_ms="
                      << round_fetch_ms[round] << '\n'
                      << "round_" << number << "_cache_hits="
                      << stats.cache_hits << '\n'
                      << "round_" << number << "_cache_misses="
                      << stats.cache_misses << '\n'
                      << "round_" << number << "_read_bytes="
                      << stats.read_bytes << '\n'
                      << "round_" << number << "_io_backend="
                      << active_io_backend_name(stats) << '\n'
                      << "round_" << number << "_io_queue_depth="
                      << stats.io_queue_depth << '\n'
                      << "round_" << number << "_io_wait_ms="
                      << stats.read_ns / 1.0e6 << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
