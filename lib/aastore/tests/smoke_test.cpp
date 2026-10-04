// 文件职责：使用临时小库检查建库、运行时解码、线程和读取后端是否可用。
// 只调用公开接口；这是构建后的环境检查，不是性能基准。
#include <aastore/store.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 不使用 assert，保证 Release 构建中也实际检查结果。
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// 每个 case 独占临时目录，并保留未编码原文用于逐条核对。
struct Fixture {
    std::filesystem::path root;          // 自动清理的临时根目录。
    std::vector<std::string> sequences;   // 按 ID 排列的预期序列。
    std::vector<aastore::SequenceId> ids; // 包含乱序、重复 ID 和空序列的请求。

    // 创建唯一临时目录，允许多个 CTest 进程并行执行。
    Fixture() {
        std::string pattern = (std::filesystem::temp_directory_path() /
            "aastore-smoke-XXXXXX").string();
        check(::mkdtemp(pattern.data()) != nullptr, "cannot create temporary directory");
        root = pattern;
    }

    // 无论检查成功还是抛出异常，都删除本 case 的输入和输出。
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    // 生成不到 24 KiB 的 FASTA，并用两个线程的预算建库。
    void build() {
        const std::string alphabet = "ACDEFGHIKLMNPQRSTVWY";
        std::ofstream fasta(root / "input.fa");
        fasta.exceptions(std::ios::badbit | std::ios::failbit);
        uint64_t residues = 0;
        for (size_t i = 0; i < 257; ++i) {
            std::string sequence;
            if (i != 7) {
                for (size_t j = 0; j < 31 + i % 47; ++j) {
                    sequence += alphabet[(j + i) % alphabet.size()];
                }
            }
            fasta << ">seq" << i << " record " << i << '\n' << sequence << '\n';
            residues += sequence.size();
            sequences.push_back(std::move(sequence));
        }
        fasta.close();
        aastore::BuildOptions options;
        options.encode_threads = 2;
        const auto built = aastore::build_fasta(root / "input.fa", root / "store", options);
        check(built.sequences == sequences.size() && built.residues == residues,
              "FASTA build statistics mismatch");
        // 请求量足以走多线程 FullMemory 解码，实际输出仍不到 320 KiB。
        for (size_t i = 0; i < 4096; ++i) ids.push_back((i * 73 + 19) % sequences.size());
    }
};

// 设置小范围和小队列，确保小数据也能覆盖多个 I/O 范围与槽位复用。
aastore::ReadOptions read_options(aastore::IoBackend backend, size_t threads) {
    aastore::ReadOptions options;
    options.io_backend = backend;
    options.threads = threads;
    options.io_queue_depth = 2;
    options.io_buffer_bytes = 4096;
    options.merge_gap_bytes = 0;
    options.max_merged_range_bytes = 64;
    options.max_exact_merged_range_bytes = 64;
    return options;
}

// 按原始请求顺序检查每条序列，不以校验和替代完整比较。
void check_batch(const Fixture& fixture, const aastore::Batch& batch) {
    check(batch.size() == fixture.ids.size(), "batch size mismatch");
    for (size_t i = 0; i < fixture.ids.size(); ++i) {
        check(batch.view(i) == fixture.sequences[fixture.ids[i]],
              "sequence mismatch at request " + std::to_string(i));
    }
}

// 检查 pread、用户缓冲写入、连续读取及名称和描述的往返一致性。
void check_pread(const Fixture& fixture) {
    aastore::Store store(fixture.root / "store");
    std::cout << "decode kernel: " << store.decode_kernel_name() << '\n';
    for (size_t threads : {size_t{1}, size_t{2}}) {
        const auto options = read_options(aastore::IoBackend::Pread, threads);
        const auto batch = store.get(fixture.ids, options);
        check_batch(fixture, batch);
        check(!batch.stats.io_uring && batch.stats.read_bytes > 0, "pread was not used");
        check(batch.stats.workers == threads, "pread worker count mismatch");
    }
    const auto options = read_options(aastore::IoBackend::Pread, 2);
    check(store.get(0, options)[0] == fixture.sequences[0], "single get mismatch");
    check(store.get({}, options).empty(), "empty get mismatch");
    std::string single;
    store.get_into(0, single);
    check(single == fixture.sequences[0], "single get_into mismatch");

    const std::vector<aastore::SequenceId> ids{256, 7, 0, 256};
    std::vector<std::string> strings(ids.size());
    std::vector<char*> outputs(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        strings[i].resize(store.sequence_length(ids[i]));
        outputs[i] = strings[i].empty() ? nullptr : strings[i].data();
    }
    store.get_into(ids, outputs, options);
    const auto records = store.batch_get_records(ids, aastore::RecordFields::All, options);
    for (size_t i = 0; i < ids.size(); ++i) {
        check(strings[i] == fixture.sequences[ids[i]], "caller buffer mismatch");
        check(records.sequence_view(i) == strings[i], "record sequence mismatch");
        check(records.name(i) == "seq" + std::to_string(ids[i]), "record name mismatch");
        check(records.description(i) == "record " + std::to_string(ids[i]),
              "record description mismatch");
    }
    const auto sequential = store.sequential_get(0, store.size(), options);
    check(sequential.size() == fixture.sequences.size(), "sequential size mismatch");
    for (size_t i = 0; i < sequential.size(); ++i) {
        check(sequential.view(i) == fixture.sequences[i], "sequential read mismatch");
    }
}

// 检查混合前缀命中/磁盘未命中、全内存解码，以及常驻文本列。
void check_memory(const Fixture& fixture) {
    for (const auto mode : {aastore::StorageMode::PrefixMemory,
                            aastore::StorageMode::FullMemory}) {
        aastore::StoreOptions opening;
        opening.storage_mode = mode;
        opening.cache_bytes = 64;
        opening.threads = 2;
        opening.resident_names = true;
        opening.resident_descriptions = true;
        aastore::Store store(fixture.root / "store", opening);
        const auto options = read_options(aastore::IoBackend::Pread, 2);
        const auto batch = store.get(fixture.ids, options);
        check_batch(fixture, batch);
        if (mode == aastore::StorageMode::PrefixMemory) {
            check(batch.stats.cache_hits > 0 && batch.stats.cache_misses > 0 &&
                  batch.stats.read_bytes > 0, "mixed prefix hit/miss path was not used");
        } else {
            check(batch.stats.full_memory && batch.stats.read_bytes == 0 &&
                  batch.stats.workers == 2, "parallel full-memory path was not used");
        }
        const auto records = store.batch_get_records({0, 256},
            aastore::RecordFields::Name | aastore::RecordFields::Description, options);
        check(records.names.stats.resident && records.descriptions.stats.resident &&
              records.names.stats.read_bytes == 0 && records.descriptions.stats.read_bytes == 0,
              "resident text path was not used");
        check(records.name(0) == "seq0" && records.description(1) == "record 256",
              "resident text mismatch");
    }
}

// 检查自动后端的结果；不可用环境中也必须通过 pread 成功读取。
void check_auto(const Fixture& fixture) {
    aastore::Store store(fixture.root / "store");
    const auto batch = store.get(fixture.ids, read_options(aastore::IoBackend::Auto, 2));
    check_batch(fixture, batch);
    check(batch.stats.read_bytes > 0, "Auto did not exercise file reads");
    std::cout << "Auto selected: " << (batch.stats.io_uring ? "io_uring" : "pread") << '\n';
}

// 仅初始化/操作码不可用允许跳过；实际 I/O 错误和结果错误必须失败。
int check_uring(const Fixture& fixture) {
    aastore::Store store(fixture.root / "store");
    try {
        const auto first = store.get(0, read_options(aastore::IoBackend::IoUring, 1));
        check(first[0] == fixture.sequences[0] && first.stats.io_uring,
              "io_uring initial read mismatch");
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).rfind("io_uring unavailable: ", 0) != 0) throw;
        std::cout << "SKIP: " << error.what() << '\n';
        return 77;
    }
    for (size_t threads : {size_t{1}, size_t{2}}) {
        const auto batch = store.get(fixture.ids, read_options(aastore::IoBackend::IoUring, threads));
        check_batch(fixture, batch);
        check(batch.stats.io_uring && batch.stats.io_workers == threads &&
              batch.stats.io_submissions > 0 &&
              batch.stats.io_submissions == batch.stats.io_completions,
              "io_uring workers or completion counts mismatch");
    }
    return 0;
}

}  // namespace

// 每次进程执行一个独立 case，错误返回非零供 CTest 和构建工具捕获。
int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: aastore_smoke_test pread|memory|auto|io_uring");
        Fixture fixture;
        fixture.build();
        const std::string name = argv[1];
        if (name == "pread") check_pread(fixture);
        else if (name == "memory") check_memory(fixture);
        else if (name == "auto") check_auto(fixture);
        else if (name == "io_uring") {
            const int status = check_uring(fixture);
            if (status != 0) return status;
        } else throw std::runtime_error("unknown smoke case: " + name);
        std::cout << "PASS: " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
