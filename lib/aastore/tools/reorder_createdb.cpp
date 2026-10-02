// 文件职责：将顺序 AAStore 库按 createdb 的 32 路分片顺序重建序列 ID。
// 先按原 ID 取模分发到临时库，再依次解码重写，避免直接拼接不对齐的 5 位尾部。
#include "aastore/store.hpp"

#include "builder_writer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kCreatedbShuffleSplits = 32;
constexpr uint64_t kDefaultBatchBytes = UINT64_C(64) * 1024 * 1024;
constexpr uint64_t kProgressInterval = UINT64_C(10) * 1000 * 1000;

struct Arguments {
    std::filesystem::path source;
    std::filesystem::path output;
    size_t threads = 1;
    uint64_t batch_bytes = kDefaultBatchBytes;
    bool overwrite = false;
    bool remove_source = false;
};

// 打印重排工具的路径参数、线程预算和输出处理选项。
void print_usage(std::ostream& output) {
    output << "usage: aastore_reorder_createdb SOURCE OUTPUT [OPTIONS]\n"
              "  --threads N       total merge/build threads (default: 1)\n"
              "  --batch-bytes N   decoded sequential batch bytes (default: 67108864)\n"
              "  --overwrite       replace an existing output store\n"
              "  --remove-source   remove SOURCE after committing OUTPUT\n";
}

// 解析非零整数参数，拒绝不能完整转换的输入并附上对应选项名称。
uint64_t parse_positive_u64(const std::string& value,
                            const std::string& option) {
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0) {
        throw std::invalid_argument("invalid value for " + option +
                                    ": " + value);
    }
    return static_cast<uint64_t>(parsed);
}

// 解析源库、目标库及批次大小等命令行选项，并检查线程值是否可表示。
Arguments parse_arguments(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage(std::cout);
        std::exit(0);
    }
    if (argc < 3) {
        throw std::invalid_argument("expected SOURCE and OUTPUT paths");
    }

    Arguments arguments;
    arguments.source = argv[1];
    arguments.output = argv[2];
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--overwrite") {
            arguments.overwrite = true;
        } else if (option == "--remove-source") {
            arguments.remove_source = true;
        } else if (option == "--threads" || option == "--batch-bytes") {
            if (++index >= argc) {
                throw std::invalid_argument("missing value for " + option);
            }
            const uint64_t parsed = parse_positive_u64(argv[index], option);
            if (option == "--threads") {
                if (parsed > std::numeric_limits<size_t>::max()) {
                    throw std::invalid_argument("value exceeds size_t for " +
                                                option);
                }
                arguments.threads = static_cast<size_t>(parsed);
            } else {
                arguments.batch_bytes = parsed;
            }
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    return arguments;
}

// 对文件或目录执行 fsync，处理中断并报告同步、关闭错误。
void sync_path(const std::filesystem::path& path, int flags) {
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open for fsync " + path.string() +
                                 ": " + std::strerror(errno));
    }
    int result;
    do {
        result = ::fsync(descriptor);
    } while (result < 0 && errno == EINTR);
    const int saved_error = result < 0 ? errno : 0;
    const int close_result = ::close(descriptor);
    if (saved_error != 0) {
        throw std::runtime_error("fsync failed for " + path.string() +
                                 ": " + std::strerror(saved_error));
    }
    if (close_result < 0) {
        throw std::runtime_error("close failed for " + path.string() +
                                 ": " + std::strerror(errno));
    }
}

// 清理已存在的临时文件或旧文本列，不存在时直接返回。
void remove_if_present(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "cannot remove stale store column", path, error);
    }
}

// 按记录数量间隔或阶段结束时报告进度，并推进下一次报告阈值。
void report_progress(const char* phase, uint64_t completed,
                     uint64_t total, uint64_t* next_report) {
    if (completed < *next_report && completed != total) return;
    const double percent = total == 0
        ? 100.0
        : 100.0 * static_cast<double>(completed) /
              static_cast<double>(total);
    std::cout << "phase=" << phase << " sequences=" << completed << '/'
              << total << " percent=" << percent << '\n'
              << std::flush;
    *next_report = completed + kProgressInterval;
}

// 累计序列长度直到达到输出字节阈值，保留完整序列并返回半开批次终点。
uint64_t next_batch_end(const aastore::Store& store,
                        uint64_t begin, uint64_t batch_bytes) {
    uint64_t end = begin;
    uint64_t residues = 0;
    while (end < store.size() &&
           (end == begin || residues < batch_bytes)) {
        const uint64_t length = store.sequence_length(end);
        if (residues > std::numeric_limits<uint64_t>::max() - length) {
            throw std::overflow_error("batch residue count overflow");
        }
        residues += length;
        ++end;
    }
    return end;
}

struct TemporaryTree {
    // 创建存放重排分片的临时目录树。
    explicit TemporaryTree(std::filesystem::path value)
        : path(std::move(value)) {
        std::filesystem::create_directories(path);
    }

    // 未要求保留时尽力删除临时目录树，不传播清理异常。
    ~TemporaryTree() {
        if (!keep) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }

    std::filesystem::path path;
    bool keep = false;
};

// 根据分片编号生成临时 AAStore 库目录路径。
std::filesystem::path shard_path(const TemporaryTree& temporary,
                                 size_t shard) {
    return temporary.path / ("shard-" + std::to_string(shard));
}

// 复用输出 arena 并建立逐序列指针，顺读后仍通过 Batch 视图交给编码器。
void read_sequential_batch(const aastore::Store& source,
                           uint64_t begin, uint64_t end,
                           const aastore::ReadOptions& reads,
                           aastore::Batch& batch,
                           std::vector<char*>& outputs) {
    const size_t count = static_cast<size_t>(end - begin);
    batch.offsets.resize(count);
    batch.lengths.resize(count);
    outputs.resize(count);
    uint64_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t length = source.sequence_length(begin + i);
        if (length > std::numeric_limits<size_t>::max() - bytes) {
            throw std::length_error("sequential output exceeds address space");
        }
        batch.offsets[i] = bytes;
        batch.lengths[i] = length;
        bytes += length;
    }
    batch.storage.resize_uninitialized(static_cast<size_t>(bytes));
    for (size_t i = 0; i < count; ++i) {
        outputs[i] = batch.lengths[i] == 0 ? nullptr
            : batch.storage.data() + static_cast<size_t>(batch.offsets[i]);
    }
    batch.stats = source.sequential_get_into(begin, end, outputs, reads);
}

// 顺读源库，将每条序列按原 ID 对 32 取模分发到对应临时库。
void scatter_source(const Arguments& arguments,
                    const TemporaryTree& temporary,
                    uint64_t* sequence_count,
                    uint64_t* residue_count) {
    aastore::Store source(arguments.source);
    *sequence_count = source.size();
    *residue_count = source.total_residues();

    const aastore::BuildThreadPlan shard_threads =
        aastore::plan_build_threads(1, false);
    std::array<std::unique_ptr<aastore::SequenceBuildWriter>,
               kCreatedbShuffleSplits> writers;
    for (size_t shard = 0; shard < writers.size(); ++shard) {
        const std::filesystem::path path = shard_path(temporary, shard);
        std::filesystem::create_directories(path);
        writers[shard] = std::make_unique<aastore::SequenceBuildWriter>(
            path / "packed.bin", path / "meta.bin", shard_threads);
    }

    aastore::ReadOptions reads;
    reads.threads = arguments.threads;
    aastore::Batch batch;
    std::vector<char*> outputs;
    uint64_t begin = 0;
    uint64_t next_report = 0;
    while (begin < source.size()) {
        const uint64_t end = next_batch_end(
            source, begin, arguments.batch_bytes);
        read_sequential_batch(source, begin, end, reads, batch, outputs);
        for (uint64_t id = begin; id < end; ++id) {
            aastore::SequenceBuildWriter& writer =
                *writers[static_cast<size_t>(id % writers.size())];
            writer.append_residues(
                batch.view(static_cast<size_t>(id - begin)));
            writer.finish_sequence();
        }
        begin = end;
        report_progress("scatter", begin, source.size(), &next_report);
    }
    if (source.size() == 0) {
        report_progress("scatter", 0, 0, &next_report);
    }
    for (auto& writer : writers) writer->finish();
}

// 依次顺读各分片并重新编码成连续 packed 流，最终校验记录数与残基总数。
void combine_shards(const Arguments& arguments,
                    const TemporaryTree& temporary,
                    uint64_t expected_sequences,
                    uint64_t expected_residues,
                    const std::filesystem::path& packed_tmp,
                    const std::filesystem::path& meta_tmp) {
    const aastore::BuildThreadPlan final_threads =
        aastore::plan_build_threads(arguments.threads, false);
    aastore::SequenceBuildWriter output(
        packed_tmp, meta_tmp, final_threads);
    aastore::ReadOptions reads;
    reads.threads = arguments.threads;
    aastore::Batch batch;
    std::vector<char*> outputs;
    uint64_t completed = 0;
    uint64_t next_report = 0;

    for (size_t shard = 0; shard < kCreatedbShuffleSplits; ++shard) {
        aastore::Store source(shard_path(temporary, shard));
        uint64_t begin = 0;
        while (begin < source.size()) {
            const uint64_t end = next_batch_end(
                source, begin, arguments.batch_bytes);
            read_sequential_batch(source, begin, end, reads, batch, outputs);
            for (uint64_t id = begin; id < end; ++id) {
                output.append_residues(
                    batch.view(static_cast<size_t>(id - begin)));
                output.finish_sequence();
            }
            completed += end - begin;
            begin = end;
            report_progress("combine", completed, expected_sequences,
                            &next_report);
        }
    }
    if (expected_sequences == 0) {
        report_progress("combine", 0, 0, &next_report);
    }
    output.finish();
    if (output.stats().sequences != expected_sequences ||
        output.stats().residues != expected_residues) {
        throw std::runtime_error("reordered store size mismatch");
    }
}

// 校验源目标关系，完成分发、合并、同步与发布，并按选项在成功后删除源库。
void reorder(const Arguments& arguments) {
    const std::filesystem::path source_absolute =
        std::filesystem::absolute(arguments.source).lexically_normal();
    const std::filesystem::path output_absolute =
        std::filesystem::absolute(arguments.output).lexically_normal();
    const auto common = std::mismatch(
        source_absolute.begin(), source_absolute.end(),
        output_absolute.begin(), output_absolute.end());
    if (source_absolute == output_absolute ||
        common.first == source_absolute.end()) {
        throw std::invalid_argument("SOURCE and OUTPUT must differ");
    }

    std::filesystem::create_directories(arguments.output);
    const std::filesystem::path packed_path =
        arguments.output / "packed.bin";
    const std::filesystem::path meta_path = arguments.output / "meta.bin";
    if (!arguments.overwrite &&
        (std::filesystem::exists(packed_path) ||
         std::filesystem::exists(meta_path))) {
        throw std::runtime_error("output store already exists: " +
                                 arguments.output.string());
    }

    const std::string suffix = ".tmp." + std::to_string(::getpid());
    const std::filesystem::path packed_tmp = packed_path.string() + suffix;
    const std::filesystem::path meta_tmp = meta_path.string() + suffix;
    remove_if_present(packed_tmp);
    remove_if_present(meta_tmp);
    TemporaryTree temporary(arguments.output.string() +
                            ".shuffle" + suffix);

    uint64_t sequence_count = 0;
    uint64_t residue_count = 0;
    try {
        scatter_source(arguments, temporary, &sequence_count,
                       &residue_count);
        combine_shards(arguments, temporary, sequence_count, residue_count,
                       packed_tmp, meta_tmp);
        sync_path(packed_tmp, O_RDONLY | O_CLOEXEC);
        sync_path(meta_tmp, O_RDONLY | O_CLOEXEC);
        std::filesystem::rename(packed_tmp, packed_path);
        std::filesystem::rename(meta_tmp, meta_path);
        remove_if_present(arguments.output / "names.bin");
        remove_if_present(arguments.output / "names.meta");
        remove_if_present(arguments.output / "descriptions.bin");
        remove_if_present(arguments.output / "descriptions.meta");
        sync_path(arguments.output, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        const std::filesystem::path parent =
            arguments.output.parent_path().empty()
                ? std::filesystem::current_path()
                : arguments.output.parent_path();
        sync_path(parent, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(packed_tmp, error);
        error.clear();
        std::filesystem::remove(meta_tmp, error);
        throw;
    }

    if (arguments.remove_source) {
        std::filesystem::remove_all(arguments.source);
    }
    std::cout << "shuffle_splits=" << kCreatedbShuffleSplits << '\n'
              << "sequences=" << sequence_count << '\n'
              << "residues=" << residue_count << '\n'
              << "source_removed=" << std::boolalpha
              << arguments.remove_source << '\n';
}

}  // namespace

// 执行重排命令并报告总耗时；遇到参数或处理错误时输出诊断并返回失败。
int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        const auto started = std::chrono::steady_clock::now();
        reorder(arguments);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "reorder_seconds=" << elapsed << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        print_usage(std::cerr);
        return 2;
    }
}
