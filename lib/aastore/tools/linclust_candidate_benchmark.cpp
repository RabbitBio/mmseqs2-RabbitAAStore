// 文件职责：按候选 ID 轨迹比较 AAStore 与 MMseqs 数据库的读取成本。
// 管理对照文件映射、并行读取和结果校验，输出候选读取的性能统计。
#include "aastore/store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct IndexEntry {
    uint64_t offset = 0;
    uint64_t length = 0;
};

struct Arguments {
    std::filesystem::path store;
    std::filesystem::path mmseqs_db;
    std::filesystem::path ids_file;
    size_t threads = 8;
    size_t queue_depth = 64;
    size_t repeats = 3;
    bool drop_cache = false;
    uint64_t merge_gap = aastore::kByteRangeMergeGapBytes;
    uint64_t max_range = aastore::kMaxMergedRangeBytes;
};

// 解析非零的 uint64_t 参数，并要求消费完整字符串。
uint64_t parse_u64(const std::string& value, const char* option) {
    size_t consumed = 0;
    const uint64_t result = std::stoull(value, &consumed);
    if (consumed != value.size() || result == 0) {
        throw std::invalid_argument(std::string("invalid ") + option);
    }
    return result;
}

// 解析允许为零的 uint64_t 参数，并要求消费完整字符串。
uint64_t parse_u64_allow_zero(const std::string& value, const char* option) {
    size_t consumed = 0;
    const uint64_t result = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string("invalid ") + option);
    }
    return result;
}

// 取得选项后面的参数值，并检查命令行是否提前结束。
std::string next_argument(int argc, char** argv, int* index,
                          const std::string& option) {
    ++*index;
    if (*index >= argc) {
        throw std::invalid_argument("missing value for " + option);
    }
    return argv[*index];
}

// 解析候选序列读取基准的数据库路径、并发和范围选项。
Arguments parse_arguments(int argc, char** argv) {
    if (argc < 4) {
        throw std::invalid_argument(
            "usage: linclust_candidate_bench STORE MMSEQS_DB IDS_FILE "
            "[--threads N] [--queue-depth N] "
            "[--repeats N] [--merge-gap N] [--max-range N] "
            "[--drop-cache]");
    }
    Arguments arguments{argv[1], argv[2], argv[3]};
    for (int i = 4; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "--threads") {
            arguments.threads = parse_u64(
                next_argument(argc, argv, &i, option), "threads");
        } else if (option == "--queue-depth") {
            arguments.queue_depth = parse_u64(
                next_argument(argc, argv, &i, option), "queue-depth");
        } else if (option == "--repeats") {
            arguments.repeats = parse_u64(
                next_argument(argc, argv, &i, option), "repeats");
        } else if (option == "--merge-gap") {
            arguments.merge_gap = parse_u64_allow_zero(
                next_argument(argc, argv, &i, option), "merge-gap");
        } else if (option == "--max-range") {
            arguments.max_range = parse_u64(
                next_argument(argc, argv, &i, option), "max-range");
        } else if (option == "--drop-cache") {
            arguments.drop_cache = true;
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    return arguments;
}

// 读取候选 ID 列表，拒绝空文件或格式错误。
std::vector<aastore::SequenceId> read_ids(
        const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open ids file");
    std::vector<aastore::SequenceId> ids;
    aastore::SequenceId id = 0;
    while (input >> id) ids.push_back(id);
    if (!input.eof() || ids.empty()) {
        throw std::runtime_error("ids file is empty or invalid");
    }
    return ids;
}

// 读取原生数据库索引，并要求 ID 从零连续递增。
std::vector<IndexEntry> read_index(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open MMseqs index");
    std::vector<IndexEntry> entries;
    uint64_t id = 0;
    IndexEntry entry;
    while (input >> id >> entry.offset >> entry.length) {
        if (id != entries.size()) {
            throw std::runtime_error("MMseqs index is not dense");
        }
        entries.push_back(entry);
    }
    if (!input.eof()) throw std::runtime_error("invalid MMseqs index");
    return entries;
}

// 向内核建议丢弃指定文件的页缓存，不保证缓存已完全清空。
void drop_cache(const std::vector<std::filesystem::path>& paths) {
#ifdef POSIX_FADV_DONTNEED
    for (const auto& path : paths) {
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) continue;
        (void)::posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED);
        ::close(descriptor);
    }
#else
    (void)paths;
#endif
}

// 累加全部字节作为结果校验值，用于比较不同读取路径。
uint64_t checksum(const uint8_t* data, size_t size) {
    uint64_t result = 0;
    for (size_t i = 0; i < size; ++i) result += data[i];
    return result;
}

// 计算无符号整数样本的均值，空样本返回零。
long double average_u64(const std::vector<uint64_t>& values) {
    long double total = 0;
    for (uint64_t value : values) total += value;
    return values.empty() ? 0.0L : total / values.size();
}

// 排序副本后返回中间样本，偶数长度取上中位数，空样本返回零。
uint64_t median_u64(std::vector<uint64_t> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// 计算非空浮点样本的算术均值。
double average_double(const std::vector<double>& values) {
    double result = 0;
    for (double value : values) result += value;
    return result / values.size();
}

// 统计候选 ID 去重后的物理跳转和间隙，输出读取局部性指标。
void print_locality(const std::vector<aastore::SequenceId>& ids,
                    const std::vector<IndexEntry>& index) {
    std::unordered_set<aastore::SequenceId> seen;
    std::vector<uint64_t> offsets;
    uint64_t logical_bytes = 0;
    for (aastore::SequenceId id : ids) {
        if (!seen.insert(id).second) continue;
        const IndexEntry& entry = index.at(static_cast<size_t>(id));
        const uint64_t sequence_bytes = entry.length >= 2
            ? entry.length - 2
            : entry.length;
        logical_bytes += sequence_bytes;
        offsets.push_back(entry.offset);
    }
    std::vector<uint64_t> original_jumps;
    original_jumps.reserve(offsets.size() > 0 ? offsets.size() - 1 : 0);
    for (size_t i = 1; i < offsets.size(); ++i) {
        original_jumps.push_back(offsets[i] >= offsets[i - 1]
            ? offsets[i] - offsets[i - 1]
            : offsets[i - 1] - offsets[i]);
    }
    std::sort(offsets.begin(), offsets.end());
    std::vector<uint64_t> sorted_gaps;
    sorted_gaps.reserve(offsets.size() > 0 ? offsets.size() - 1 : 0);
    for (size_t i = 1; i < offsets.size(); ++i) {
        sorted_gaps.push_back(offsets[i] - offsets[i - 1]);
    }
    std::cout << "candidate_unique_ids=" << seen.size() << '\n'
              << "candidate_logical_bytes=" << logical_bytes << '\n'
              << "candidate_original_mean_abs_offset_jump="
              << static_cast<double>(average_u64(original_jumps)) << '\n'
              << "candidate_original_median_abs_offset_jump="
              << median_u64(original_jumps) << '\n'
              << "candidate_sorted_mean_offset_gap="
              << static_cast<double>(average_u64(sorted_gaps)) << '\n'
              << "candidate_sorted_median_offset_gap="
              << median_u64(sorted_gaps) << '\n';
}

// 串行读取原生数据库记录，剥离尾部分隔符后累加校验值。
uint64_t read_native(const std::filesystem::path& database,
                     const std::vector<IndexEntry>& index,
                     const std::vector<aastore::SequenceId>& ids) {
    const int descriptor = ::open(database.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) throw std::runtime_error("cannot open MMseqs DB");
    uint64_t result = 0;
    std::vector<uint8_t> buffer;
    for (aastore::SequenceId id : ids) {
        const IndexEntry& entry = index.at(static_cast<size_t>(id));
        buffer.resize(static_cast<size_t>(entry.length));
        size_t completed = 0;
        while (completed < buffer.size()) {
            const ssize_t count = ::pread(
                descriptor, buffer.data() + completed,
                buffer.size() - completed,
                static_cast<off_t>(entry.offset + completed));
            if (count <= 0) {
                ::close(descriptor);
                throw std::runtime_error("native MMseqs pread failed");
            }
            completed += static_cast<size_t>(count);
        }
        // 原生序列记录以 "\n\0" 结束；校验只比较序列内容，
        // 不计入这两个分隔字节。
        size_t sequence_size = buffer.size();
        if (sequence_size >= 2 && buffer[sequence_size - 2] == '\n' &&
            buffer[sequence_size - 1] == '\0') {
            sequence_size -= 2;
        }
        result += checksum(buffer.data(), sequence_size);
    }
    ::close(descriptor);
    return result;
}

struct NativeReadContext {
    const std::filesystem::path* database = nullptr;
    const std::vector<IndexEntry>* index = nullptr;
    const std::vector<aastore::SequenceId>* ids = nullptr;
    size_t worker_count = 1;
    std::vector<uint64_t>* partial = nullptr;
    std::vector<std::exception_ptr>* errors = nullptr;
};

// 读取分配给当前线程的原生记录，保存校验值并捕获异常。
void read_native_worker(NativeReadContext* ctx, size_t worker) noexcept {
    int descriptor = -1;
    try {
        descriptor = ::open(
            ctx->database->c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            throw std::runtime_error("cannot open MMseqs DB");
        }
        std::vector<uint8_t> buffer;
        const size_t begin =
            ctx->ids->size() * worker / ctx->worker_count;
        const size_t end =
            ctx->ids->size() * (worker + 1) / ctx->worker_count;
        for (size_t i = begin; i < end; ++i) {
            const IndexEntry& entry = ctx->index->at(
                static_cast<size_t>((*ctx->ids)[i]));
            buffer.resize(static_cast<size_t>(entry.length));
            size_t completed = 0;
            while (completed < buffer.size()) {
                const ssize_t count = ::pread(
                    descriptor, buffer.data() + completed,
                    buffer.size() - completed,
                    static_cast<off_t>(entry.offset + completed));
                if (count <= 0) {
                    throw std::runtime_error(
                        "native parallel pread failed");
                }
                completed += static_cast<size_t>(count);
            }
            size_t sequence_size = buffer.size();
            if (sequence_size >= 2 &&
                buffer[sequence_size - 2] == '\n' &&
                buffer[sequence_size - 1] == '\0') {
                sequence_size -= 2;
            }
            (*ctx->partial)[worker] +=
                checksum(buffer.data(), sequence_size);
        }
        ::close(descriptor);
    } catch (...) {
        if (descriptor >= 0) ::close(descriptor);
        (*ctx->errors)[worker] = std::current_exception();
    }
}

// 分片并发读取原生数据库，等待工作线程并合并校验值。
uint64_t read_native_parallel(
        const std::filesystem::path& database,
        const std::vector<IndexEntry>& index,
        const std::vector<aastore::SequenceId>& ids,
        size_t worker_count) {
    worker_count = std::max<size_t>(1, std::min(worker_count, ids.size()));
    std::vector<uint64_t> partial(worker_count, 0);
    std::vector<std::exception_ptr> errors(worker_count);
    NativeReadContext ctx{
        &database, &index, &ids, worker_count, &partial, &errors};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back(read_native_worker, &ctx, worker);
    }
    for (std::thread& worker : workers) worker.join();
    for (const std::exception_ptr& error : errors) {
        if (error) std::rethrow_exception(error);
    }
    uint64_t result = 0;
    for (uint64_t value : partial) result += value;
    return result;
}

struct OffsetLess {
    const std::vector<IndexEntry>* index = nullptr;

    // 按原生数据库索引中的物理字节偏移比较两个 ID。
    bool operator()(aastore::SequenceId left,
                    aastore::SequenceId right) const {
        return (*index)[static_cast<size_t>(left)].offset <
               (*index)[static_cast<size_t>(right)].offset;
    }
};

// 返回从指定时刻到当前时刻的毫秒数。
double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               Clock::now() - start).count();
}

}  // namespace

// 对同一候选集合运行原生和 AAStore 读取基准，核对结果并输出统计。
int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        const auto ids = read_ids(arguments.ids_file);
        const auto index = read_index(arguments.mmseqs_db.string() + ".index");
        for (aastore::SequenceId id : ids) {
            if (id >= index.size()) throw std::out_of_range("candidate ID");
        }
        print_locality(ids, index);
        std::vector<aastore::SequenceId> offset_sorted_ids = ids;
        std::stable_sort(
            offset_sorted_ids.begin(), offset_sorted_ids.end(),
            OffsetLess{&index});

        const std::vector<std::filesystem::path> native_cache_paths = {
            arguments.mmseqs_db, arguments.mmseqs_db.string() + ".index"};
        const std::vector<std::filesystem::path> aastore_cache_paths = {
            arguments.store / "packed.bin", arguments.store / "meta.bin"};
        aastore::Store store(arguments.store);

        std::vector<double> native_times;
        std::vector<double> native_parallel_times;
        std::vector<double> native_sorted_times;
        std::vector<double> aastore_times;
        uint64_t native_sum = 0;
        uint64_t aastore_sum = 0;
        for (size_t repeat = 0; repeat < arguments.repeats; ++repeat) {
            if (arguments.drop_cache) drop_cache(native_cache_paths);
            const auto native_start = Clock::now();
            native_sum = read_native(arguments.mmseqs_db, index, ids);
            native_times.push_back(elapsed_ms(native_start));

            if (arguments.drop_cache) drop_cache(native_cache_paths);
            const auto native_parallel_start = Clock::now();
            const uint64_t native_parallel_sum = read_native_parallel(
                arguments.mmseqs_db, index, ids, arguments.threads);
            native_parallel_times.push_back(elapsed_ms(native_parallel_start));
            if (native_parallel_sum != native_sum) {
                throw std::runtime_error("native parallel checksum mismatch");
            }

            if (arguments.drop_cache) drop_cache(native_cache_paths);
            const auto native_sorted_start = Clock::now();
            (void)read_native(arguments.mmseqs_db, index, offset_sorted_ids);
            native_sorted_times.push_back(elapsed_ms(native_sorted_start));

            if (arguments.drop_cache) drop_cache(aastore_cache_paths);
            aastore::ReadOptions options;
            options.collect_diagnostics = true;
            options.threads = arguments.threads;
            options.io_queue_depth = arguments.queue_depth;
            options.io_backend = aastore::IoBackend::IoUring;
            options.merge_gap_bytes = arguments.merge_gap;
            options.max_merged_range_bytes = arguments.max_range;
            const auto aastore_start = Clock::now();
            const aastore::Batch batch = store.batch_get(ids, options);
            aastore_sum = checksum(
                reinterpret_cast<const uint8_t*>(batch.storage.data()),
                batch.storage.size());
            aastore_times.push_back(elapsed_ms(aastore_start));
            std::cout << "repeat=" << repeat + 1
                      << " native_ms=" << native_times.back()
                      << " native_parallel_ms="
                      << native_parallel_times.back()
                      << " native_offset_sorted_ms="
                      << native_sorted_times.back()
                      << " aastore_ms=" << aastore_times.back()
                      << " ranges=" << batch.stats.ranges
                      << " read_bytes=" << batch.stats.read_bytes
                      << " unique_bytes=" << batch.stats.unique_packed_bytes
                      << " overread_bytes=" << batch.stats.overread_bytes
                      << " free_merges=" << batch.stats.free_merges << '\n';
        }

        const std::unordered_set<aastore::SequenceId> unique_ids(
            ids.begin(), ids.end());
        std::cout << "ids=" << ids.size()
                  << " unique_ids=" << unique_ids.size() << '\n';
        std::cout << "native_checksum=" << native_sum << '\n'
                  << "aastore_checksum=" << aastore_sum << '\n'
                  << "native_avg_ms=" << average_double(native_times) << '\n'
                  << "native_parallel_avg_ms="
                  << average_double(native_parallel_times) << '\n'
                  << "native_offset_sorted_avg_ms="
                  << average_double(native_sorted_times) << '\n'
                  << "aastore_avg_ms="
                  << average_double(aastore_times) << '\n';
        return native_sum == aastore_sum ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "linclust candidate benchmark: " << error.what() << '\n';
        return 2;
    }
}
