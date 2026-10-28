// 文件职责：验证公开配置校验、零值约定，以及无效配置在读写前被拒绝。
#include "aastore/store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void expect_invalid(Function&& function, const std::string& message) {
    try {
        function();
        expect(false, message + " did not reject the options");
    } catch (const std::invalid_argument&) {
    } catch (const std::exception& error) {
        expect(false, message + " raised a different error: " + error.what());
    }
}

// 每次测试拥有独立目录，测试结束时清理已创建的文件。
struct Fixture {
    std::filesystem::path root;
    std::filesystem::path fasta;
    std::filesystem::path path;
    const std::vector<std::string> sequences{"", "ACDEFG", "WYAA"};

    Fixture() {
        char pattern[] = "/tmp/aastore-options-test-XXXXXX";
        const char* directory = ::mkdtemp(pattern);
        if (!directory) throw std::runtime_error("cannot create test directory");
        root = directory;
        fasta = root / "input.fasta";
        path = root / "store";
        std::ofstream output(fasta);
        for (size_t i = 0; i < sequences.size(); ++i) {
            output << ">name" << i << " description" << i << '\n'
                   << sequences[i] << '\n';
        }
        output.close();
        if (!output) throw std::runtime_error("cannot write test FASTA");
        aastore::build_fasta(fasta, path);
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

// 默认配置和文档中的零值都是有效约定，不应被新增校验拒绝。
void check_valid_options(const Fixture& fixture) {
    aastore::BuildOptions{}.validate();
    aastore::StoreOptions{}.validate();
    aastore::ReadOptions{}.validate();
    expect(aastore::StoreOptions{}.index_load_mode == aastore::IndexLoadMode::Mmap,
           "index loading defaults to mmap");
    expect(aastore::Store(fixture.path).index_load_mode() == aastore::IndexLoadMode::Mmap,
           "default Store uses mmap indices");

    aastore::ReadOptions options;
    options.threads = 0;
    options.io_queue_depth = 0;
    options.io_buffer_bytes = 0;
    options.merge_gap_bytes = 0;
    options.max_merged_range_bytes = 0;
    options.max_exact_merged_range_bytes = 0;
    options.input_sorted = true;
    for (const auto backend : {aastore::IoBackend::Auto,
                               aastore::IoBackend::Pread,
                               aastore::IoBackend::IoUring}) {
        options.io_backend = backend;
        options.validate();
    }
    options.io_backend = aastore::IoBackend::Pread;
    const std::vector<aastore::SequenceId> ids{0, 1, 1, 2};
    for (const uint64_t budget : {uint64_t{0}, uint64_t{1},
                                  std::numeric_limits<uint64_t>::max()}) {
        aastore::StoreOptions store_options;
        store_options.preload_bytes = budget;
        store_options.threads = 0;
        store_options.validate();
        for (const auto index_mode : {aastore::IndexLoadMode::Mmap,
                                     aastore::IndexLoadMode::Read}) {
            store_options.index_load_mode = index_mode;
            store_options.validate();
            aastore::Store store(fixture.path, store_options);
            expect(store.index_load_mode() == index_mode,
                   "Store retains the selected index loading mode");
            const auto batch = store.get(ids, options);
            for (size_t i = 0; i < ids.size(); ++i) {
                expect(batch.view(i) == fixture.sequences[ids[i]],
                       "zero budgets preserve reads and repeated sorted IDs");
            }
            const auto sequential = store.sequential_get(0, fixture.sequences.size(), options);
            for (size_t i = 0; i < fixture.sequences.size(); ++i) {
                expect(sequential.view(i) == fixture.sequences[i],
                       "zero thread budget preserves sequential reads");
            }
        }
    }
}

// 无效建库配置先于输入检查被拒绝，且不会创建或覆盖数据库。
void check_invalid_build(const Fixture& fixture) {
    aastore::BuildOptions options;
    options.encode_threads = 0;
    expect_invalid([&] { options.validate(); }, "public BuildOptions validation");
    const auto output = fixture.root / "not-created";
    expect_invalid([&] {
        aastore::build_fasta(fixture.root / "missing.fasta", output, options);
    }, "invalid build before missing input");
    expect(!std::filesystem::exists(output), "invalid build does not create an output directory");
    options.overwrite = true;
    expect_invalid([&] {
        aastore::build_fasta(fixture.fasta, fixture.path, options);
    }, "invalid build before overwriting an existing store");
    aastore::Store store(fixture.path);
    expect(store.size() == fixture.sequences.size() && store.get(1).view(0) == fixture.sequences[1],
           "invalid overwrite preserves the existing store");
}

// 无效索引加载方式即使路径不存在也必须报配置错误。
void check_invalid_store(const Fixture& fixture) {
    for (const auto mode : {static_cast<aastore::IndexLoadMode>(99),
                           static_cast<aastore::IndexLoadMode>(255)}) {
        aastore::StoreOptions options;
        options.index_load_mode = mode;
        expect_invalid([&] { options.validate(); }, "public index loading validation");
        for (const auto& path : {fixture.path, fixture.root / "missing-store"}) {
            expect_invalid([&] { aastore::Store store(path, options); },
                           "Store validates index loading before opening files");
        }
    }
}

// 预算只限制编码序列预载；全量边界必须进入无需文件读取的快速路径。
void check_preload_budget(const Fixture& fixture) {
    const uint64_t packed = aastore::Store(fixture.path).packed_bytes();
    struct Case {
        uint64_t budget;
        aastore::StorageMode mode;
        uint64_t resident;
    };
    const std::vector<Case> cases{
        {0, aastore::StorageMode::NoMemory, 0},
        {1, aastore::StorageMode::PartialMemory, 1},
        {packed - 1, aastore::StorageMode::PartialMemory, packed - 1},
        {packed, aastore::StorageMode::FullMemory, packed},
        {packed + 1, aastore::StorageMode::FullMemory, packed},
        {std::numeric_limits<uint64_t>::max(), aastore::StorageMode::FullMemory, packed}};
    const std::vector<aastore::SequenceId> ids{2, 1, 1, 0};
    aastore::ReadOptions reads;
    reads.io_backend = aastore::IoBackend::Pread;
    reads.collect_diagnostics = true;
    for (const auto index_mode : {aastore::IndexLoadMode::Mmap,
                                 aastore::IndexLoadMode::Read}) {
        for (const auto& test : cases) {
            aastore::StoreOptions options;
            options.preload_bytes = test.budget;
            options.index_load_mode = index_mode;
            aastore::Store store(fixture.path, options);
            expect(store.storage_mode() == test.mode &&
                       store.resident_bytes() == test.resident,
                   "preload budget selects the mode and bounds allocation");
            const auto batch = store.get(ids, reads);
            for (size_t i = 0; i < ids.size(); ++i) {
                expect(batch.view(i) == fixture.sequences[ids[i]],
                       "budget boundaries preserve duplicate, unsorted and empty records");
            }
            if (test.mode == aastore::StorageMode::FullMemory) {
                expect(batch.stats.full_memory && !batch.stats.prefix_memory &&
                           batch.stats.read_bytes == 0 && batch.stats.ranges == 0 &&
                           batch.stats.sort_ns == 0 && batch.stats.range_plan_ns == 0,
                       "a sufficient budget selects the full-memory fast path");
            }
        }
    }

    // 空库和仅含空序列的库无需分配 packed 缓冲，正预算也不能产生部分驻留。
    for (const bool empty_records : {false, true}) {
        const auto fasta = fixture.root / (empty_records ? "empty-records.fa" : "empty.fa");
        const auto path = fixture.root / (empty_records ? "empty-records" : "empty");
        {
            std::ofstream output(fasta);
            if (empty_records) output << ">one\n>two\n";
        }
        aastore::build_fasta(fasta, path);
        for (const auto& test : std::vector<Case>{
                 {0, aastore::StorageMode::NoMemory, 0},
                 {1, aastore::StorageMode::FullMemory, 0},
                 {std::numeric_limits<uint64_t>::max(), aastore::StorageMode::FullMemory, 0}}) {
            aastore::StoreOptions options;
            options.preload_bytes = test.budget;
            aastore::Store store(path, options);
            expect(store.packed_bytes() == 0 && store.resident_bytes() == 0 &&
                       store.storage_mode() == test.mode,
                   "empty encoded data has deterministic residency without allocation");
            const auto batch = store.sequential_get(0, store.size(), reads);
            expect(batch.size() == (empty_records ? 2 : 0) && batch.stats.read_bytes == 0,
                   "empty sequences remain readable in the selected mode");
            if (empty_records) {
                const auto records = store.get({1, 0, 1}, reads);
                expect(records.size() == 3 && records.view(0).empty() &&
                           records.view(1).empty() && records.view(2).empty(),
                       "random reads of empty sequences preserve request count");
            }
        }
    }
}

// 每个读取入口在空请求、驻留数据与调用方输出区上都遵守同一校验规则。
void check_invalid_reads(const Fixture& fixture) {
    for (const uint64_t budget : {uint64_t{0}, uint64_t{1},
                                  std::numeric_limits<uint64_t>::max()}) {
        aastore::StoreOptions store_options;
        store_options.preload_bytes = budget;
        store_options.threads = 1;
        store_options.resident_names = true;
        store_options.resident_descriptions = true;
        aastore::Store store(fixture.path, store_options);
        for (const auto backend : {static_cast<aastore::IoBackend>(99),
                                    static_cast<aastore::IoBackend>(255)}) {
            aastore::ReadOptions options;
            options.io_backend = backend;
            expect_invalid([&] { options.validate(); }, "public ReadOptions validation");
            expect_invalid([&] { store.get(1, options); }, "single get validates options");
            expect_invalid([&] { store.get({}, options); }, "empty initializer-list get validates options");
            for (const auto& ids : {std::vector<aastore::SequenceId>{1},
                                    std::vector<aastore::SequenceId>{}}) {
                expect_invalid([&] { store.get(ids, options); }, "get");
                for (const auto fields : {aastore::RecordFields::None,
                                          aastore::RecordFields::Name,
                                          aastore::RecordFields::Description,
                                          aastore::RecordFields::Sequence,
                                          aastore::RecordFields::All}) {
                    expect_invalid([&] { store.batch_get_records(ids, fields, options); },
                                   "batch_get_records including empty and fieldless requests");
                }
                std::string output(fixture.sequences[1].size(), '#');
                std::vector<char*> outputs(ids.size(), output.data());
                expect_invalid([&] { store.get_into(ids, outputs, options); },
                               "get_into vector overload");
                expect(output == std::string(output.size(), '#'),
                       "invalid batch options do not modify vector outputs");
                expect_invalid([&] {
                    store.get_into(ids, outputs.data(), outputs.size(), options);
                }, "get_into pointer overload");
                expect(output == std::string(output.size(), '#'),
                       "invalid batch options do not modify pointer outputs");
            }
            for (const aastore::SequenceId end : {1, 2}) {
                expect_invalid([&] { store.sequential_get(1, end, options); },
                               "sequential_get including an empty interval");
                std::string output(fixture.sequences[1].size(), '#');
                std::vector<char*> outputs(end - 1, output.data());
                expect_invalid([&] { store.sequential_get_into(1, end, outputs, options); },
                               "sequential_get_into vector overload");
                expect(output == std::string(output.size(), '#'),
                       "invalid sequential options do not modify vector outputs");
                expect_invalid([&] {
                    store.sequential_get_into(1, end, outputs.data(), outputs.size(), options);
                }, "sequential_get_into pointer overload");
                expect(output == std::string(output.size(), '#'),
                       "invalid sequential options do not modify pointer outputs");
            }
        }
    }
}

}  // namespace

int main() {
    try {
        const Fixture fixture;
        check_valid_options(fixture);
        check_invalid_build(fixture);
        check_invalid_store(fixture);
        check_preload_budget(fixture);
        check_invalid_reads(fixture);
    } catch (const std::exception& error) {
        expect(false, std::string("unexpected exception: ") + error.what());
    }
    if (failures == 0) std::cout << "options validation tests passed\n";
    return failures == 0 ? 0 : 1;
}
