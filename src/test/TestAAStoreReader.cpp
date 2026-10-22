#include "AAStoreMemoryBudget.h"
#include "AAStoreTargetReader.h"
#include "Parameters.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

const char* binary_name = "test_aastore_reader";
DEFAULT_PARAMETER_SINGLETON_INIT

namespace {

void check_memory_budget() {
    constexpr uint64_t gib = UINT64_C(1024) * 1024 * 1024;
    constexpr uint64_t mib = UINT64_C(1024) * 1024;
    constexpr uint64_t cacheAlignment = 2 * mib;
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();

    const auto expected_cache = [cacheAlignment](uint64_t remaining) {
        const uint64_t candidate =
            (remaining / 10) * 9 + ((remaining % 10) * 9) / 10;
        return candidate - candidate % cacheAlignment;
    };

    const auto budget = aastore_memory::plan(
        256 * gib, 180 * gib, 25 * gib, 256 * mib, 75 * gib);
    if (budget.safetyReserve != 256 * gib / 5 ||
        budget.memoryReserve !=
            budget.safetyReserve + 25 * gib + 256 * mib + 75 * gib ||
        budget.cacheBytes != expected_cache(budget.remainingPool) ||
        budget.cacheBytes % cacheAlignment != 0 ||
        budget.pageCacheReserve !=
            budget.remainingPool - budget.cacheBytes ||
        budget.memoryReserve + budget.cacheBytes +
            budget.pageCacheReserve != budget.memoryAvailable) {
        throw std::runtime_error("incorrect AAStore memory budget");
    }

    const auto constrained = aastore_memory::plan(
        16 * gib, 12 * gib, gib, 256 * mib, 2 * gib);
    if (constrained.safetyReserve != 4 * gib ||
        constrained.cacheBytes == 0 ||
        constrained.memoryReserve + constrained.cacheBytes +
            constrained.pageCacheReserve > constrained.memoryAvailable) {
        throw std::runtime_error(
            "AAStore budget exceeded constrained memory");
    }

    const auto nrAll = aastore_memory::plan(
        UINT64_C(270066999296), UINT64_C(224517115904),
        UINT64_C(27131261701), UINT64_C(1073741824),
        UINT64_C(104296337899));
    if (nrAll.cacheBytes != UINT64_C(34200354816)) {
        throw std::runtime_error(
            "AAStore nr_all memory budget changed unexpectedly");
    }

    if (aastore_memory::plan(
            16 * gib, 3 * gib, gib, 0, 0).cacheBytes != 0 ||
        aastore_memory::plan(
            16 * gib, 12 * gib, maximum, maximum, maximum).cacheBytes != 0) {
        throw std::runtime_error(
            "exhausted reserves enabled an AAStore prefix");
    }
}

void check_record_memory_budget() {
    constexpr uint64_t gib = UINT64_C(1024) * 1024 * 1024;
    constexpr uint64_t mib = UINT64_C(1024) * 1024;
    const auto enough = aastore_memory::plan_records(
        64 * gib, 50 * gib, gib, gib, 2 * gib, 10 * gib, 64 * mib);
    if (!enough.residentText || enough.textReserve != 10 * gib ||
        enough.metadataReserve != 64 * mib ||
        enough.memoryReserve < enough.safetyReserve + 14 * gib + 64 * mib ||
        enough.memoryReserve + enough.cacheBytes + enough.pageCacheReserve !=
            enough.memoryAvailable) {
        throw std::runtime_error("resident records budget lost a reserve");
    }
    const auto constrained = aastore_memory::plan_records(
        16 * gib, 12 * gib, gib, gib, 2 * gib, 8 * gib, 64 * mib);
    if (constrained.residentText || constrained.textReserve != 0 ||
        constrained.metadataReserve != 64 * mib ||
        constrained.memoryReserve + constrained.cacheBytes +
            constrained.pageCacheReserve != constrained.memoryAvailable) {
        throw std::runtime_error("records budget failed to fall back");
    }
    const auto empty = aastore_memory::plan_records(
        64 * gib, 50 * gib, gib, gib, 2 * gib, 0, 64 * mib);
    if (empty.textReserve != 0 || empty.metadataReserve != 64 * mib ||
        empty.memoryReserve + empty.cacheBytes + empty.pageCacheReserve !=
            empty.memoryAvailable) {
        throw std::runtime_error("empty text columns changed the budget");
    }
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const auto exhausted = aastore_memory::plan_records(
        64 * gib, 50 * gib, maximum, maximum, maximum, maximum, maximum);
    if (exhausted.residentText || exhausted.cacheBytes != 0 ||
        exhausted.remainingPool != 0 || exhausted.memoryReserve != maximum) {
        throw std::runtime_error("overflow enabled resident records");
    }
}

void check_batch(
        AAStoreTargetReader& reader,
        const std::vector<std::string>& source,
        const std::vector<uint32_t>& keys,
        size_t threads) {
    AAStoreTargetReader::ThreadBatch batch;
    batch.ids = {0, 2, 0, keys.size() - 1, 1};
    reader.fetch_into(batch, threads);
    for (const size_t id : batch.ids) {
        std::string_view sequence;
        if (!batch.sequences.view(id, sequence) ||
            sequence != source[keys[id]] ||
            sequence.data()[sequence.size()] != '\0') {
            throw std::runtime_error(
                "AAStore random batch returned a wrong sequence");
        }
    }
    if (batch.physicalIds.size() != 4 ||
        batch.sequences.fetched.stats.workers > 1 ||
        batch.sequences.fetched.stats.io_workers > 1 ||
        batch.sequences.fetched.stats.decode_workers > 1 ||
        batch.sequences.fetched.stats.io_queue_depth >
            AAStoreTargetReader::threadBatchQueueDepth(threads)) {
        throw std::runtime_error(
            "AAStore random batch violated its thread budget");
    }

    const char* arena = batch.sequences.fetched.storage.data();
    const size_t arenaBytes = batch.sequences.fetched.storage.size();
    batch.ids = {2, 0, 2};
    reader.fetch_into(batch, threads);
    if (batch.sequences.fetched.storage.data() != arena ||
        batch.sequences.fetched.storage.size() > arenaBytes) {
        throw std::runtime_error("AAStore smaller batch did not reuse its output arena");
    }
    for (const size_t id : batch.ids) {
        std::string_view sequence;
        if (!batch.sequences.view(id, sequence) ||
            sequence != source[keys[id]] ||
            sequence.data()[sequence.size()] != '\0') {
            throw std::runtime_error("AAStore reused batch returned a wrong sequence");
        }
    }

    batch.ids.clear();
    reader.fetch_into(batch, threads);
    std::string_view stale;
    if (batch.sequences.view(0, stale) ||
        batch.sequences.fetched.storage.data() != arena ||
        !batch.sequences.fetched.empty() ||
        batch.sequences.fetched.stats.requested_sequences != 0) {
        throw std::runtime_error("AAStore empty batch retained old views");
    }
}

void check_scoring_views(const std::filesystem::path& directory,
                         const std::string& store,
                         const std::string& database,
                         const std::vector<std::string>& source) {
    // This fixture has no native sequence payload or .index. Fresh full-store
    // associations must use stable AAStore keys without rebuilding an index.
    AAStoreTargetReader full;
    if (!full.openForScoring(database, 4, 0, true) || full.size() != source.size())
        throw std::runtime_error("full scoring view depended on a native index");
    uint64_t total = 0, maximum = 0;
    for (unsigned int key = 0; key < source.size(); ++key) {
        total += source[key].size();
        maximum = std::max(maximum, static_cast<uint64_t>(source[key].size()));
        if (full.getId(key) != key || full.getDbKey(key) != key)
            throw std::runtime_error("full scoring view changed stable sequence keys");
    }
    if (full.totalLength() != total || full.maxSequenceLength() != maximum ||
        full.getId(static_cast<unsigned int>(source.size())) != UINT_MAX)
        throw std::runtime_error("full scoring view has incorrect metadata or bounds");

    const std::string subset = (directory / "subset").string();
    const std::vector<uint32_t> keys{0, 2, 4, 5};
    AAStoreTargetReader::associateSubsetDatabase(subset, store, {5, 0, 4, 2});
    AAStoreTargetReader view;
    if (!view.openForScoring(subset, 4, 0, true) || view.size() != keys.size())
        throw std::runtime_error("subset scoring view depended on a native index");
    total = 0;
    maximum = 0;
    for (size_t id = 0; id < keys.size(); ++id) {
        const unsigned int key = keys[id];
        total += source[key].size();
        maximum = std::max(maximum, static_cast<uint64_t>(source[key].size()));
        if (view.getId(key) != id || view.getDbKey(id) != key ||
            view.sequenceLength(id) != source[key].size())
            throw std::runtime_error("subset scoring view lost its original key mapping");
    }
    if (view.totalLength() != total || view.maxSequenceLength() != maximum ||
        view.getId(1) != UINT_MAX || view.getId(3) != UINT_MAX)
        throw std::runtime_error("subset scoring view admitted a nonmember key");
    check_batch(view, source, keys, 4);

    // Native createsubdb retains repeated index rows. AAStore's compact view
    // keeps those local rows and uses the first row for a key lookup as well.
    const std::string repeatedSubset = (directory / "repeated_subset").string();
    AAStoreTargetReader::associateSubsetDatabase(repeatedSubset, store, {5, 0, 5});
    AAStoreTargetReader repeated;
    if (!repeated.openForScoring(repeatedSubset, 4, 0, true) ||
        repeated.size() != 3 || repeated.getDbKey(0) != 0 ||
        repeated.getDbKey(1) != 5 || repeated.getDbKey(2) != 5 ||
        repeated.getId(5) != 1 || repeated.getId(1) != UINT_MAX ||
        repeated.sequenceLength(2) != source[5].size() ||
        repeated.maxSequenceLength() != std::max(source[0].size(), source[5].size()) ||
        repeated.totalLength() != source[0].size() + 2 * source[5].size()) {
        throw std::runtime_error("subset scoring view changed native repeated-key semantics");
    }
    // A view may have more local rows than physical sequences when repeated
    // selections are present, without expanding the shared AAStore itself.
    AAStoreTargetReader::associateSubsetDatabase(repeatedSubset, store,
        std::vector<uint32_t>(source.size() + 1, 5));
    AAStoreTargetReader extraRows;
    if (!extraRows.openForScoring(repeatedSubset, 4, 0, true) ||
        extraRows.size() != source.size() + 1 || extraRows.getId(5) != 0 ||
        extraRows.getDbKey(source.size()) != 5 ||
        extraRows.maxSequenceLength() != source[5].size() ||
        extraRows.totalLength() != (source.size() + 1) * source[5].size()) {
        throw std::runtime_error("subset scoring view rejected repeated rows beyond physical count");
    }

    // A marked subset cannot silently become a full-store view if its member
    // table is missing, even though all excluded keys exist in packed.bin.
    std::filesystem::remove(subset + ".aastore.ids");
    bool rejected = false;
    try {
        AAStoreTargetReader missing;
        missing.openForScoring(subset, 4, 0, true);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("subset without its member table became a full view");
}

}  // namespace

int main() {
    std::string path = (std::filesystem::temp_directory_path() /
                        "mmseqs-aastore-reader.XXXXXX").string();
    const char* created = ::mkdtemp(path.data());
    if (!created) return 1;
    const std::filesystem::path directory(created);

    try {
        check_memory_budget();
        check_record_memory_budget();
        aastore::BatchStats ioStats;
        aastore::BatchStats firstBatch;
        firstBatch.ranges = 3;
        firstBatch.read_bytes = 4096;
        firstBatch.io_submissions = 4;
        AAStoreTargetReader::accumulateIoStats(ioStats, firstBatch);
        AAStoreTargetReader::accumulateIoStats(ioStats, firstBatch);
        AAStoreTargetReader::accumulateIoStats(ioStats, {});
        if (ioStats.ranges != 6 || ioStats.read_bytes != 8192 ||
            ioStats.io_submissions != 8) {
            throw std::runtime_error("incorrect AAStore I/O counters");
        }
        for (const auto& setting :
             std::vector<std::pair<size_t, size_t>>{
                 {1, 512}, {4, 128}, {32, 16}, {128, 4},
                 {300, 1}, {512, 1}}) {
            if (AAStoreTargetReader::threadBatchQueueDepth(
                    setting.first) != setting.second) {
                throw std::runtime_error(
                    "wrong AAStore per-thread queue depth");
            }
        }
        for (const size_t threads : {size_t{0}, size_t{513}}) {
            bool rejected = false;
            try {
                AAStoreTargetReader::threadBatchQueueDepth(threads);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            if (!rejected) {
                throw std::runtime_error(
                    "invalid AAStore compute-thread count accepted");
            }
        }

        const std::vector<std::string> source{
            "ACDEFG", "M*X-", "WQ", "K", "PPPP", "LLLL"};
        {
            std::ofstream fasta(directory / "input.fa");
            for (size_t id = 0; id < source.size(); ++id) {
                fasta << '>' << id << '\n' << source[id] << '\n';
            }
        }
        const std::string store = (directory / "store").string();
        const std::string database = (directory / "database").string();
        aastore::build_fasta(directory / "input.fa", store);
        AAStoreTargetReader::associateDatabase(database, store);
        if (AAStoreTargetReader::associatedStorePath(database) !=
            std::filesystem::absolute(store).lexically_normal().string()) {
            throw std::runtime_error(
                "AAStore database association was not preserved");
        }
        check_scoring_views(directory, store, database, source);

        const std::vector<uint32_t> keys{4, 2, 5, 0, 3, 1};
        for (const auto backend :
             {aastore::IoBackend::Pread, aastore::IoBackend::IoUring}) {
            for (const auto mode :
                 {aastore::StorageMode::ByteRange,
                  aastore::StorageMode::PrefixMemory}) {
                aastore::StoreOptions options;
                options.storage_mode = mode;
                options.cache_bytes = 7;
                aastore::ReadOptions reads;
                reads.io_backend = backend;
                reads.merge_gap_bytes = 0;
                reads.max_merged_range_bytes = 1;
                reads.max_exact_merged_range_bytes = 1;
                reads.threads = 8;

                AAStoreTargetReader reader;
                reader.open(store, options, reads);
                reader.mapFromMMseqsKeys(keys);
                reader.validateSize(keys.size());
                check_batch(reader, source, keys, 4);

                std::vector<std::exception_ptr> errors(4);
                std::vector<std::thread> workers;
                for (size_t worker = 0; worker < errors.size(); ++worker) {
                    workers.emplace_back([&, worker]() {
                        try {
                            for (size_t repeat = 0; repeat < 10; ++repeat) {
                                check_batch(
                                    reader, source, keys, errors.size());
                            }
                        } catch (...) {
                            errors[worker] = std::current_exception();
                        }
                    });
                }
                for (auto& worker : workers) worker.join();
                for (const auto& error : errors) {
                    if (error) std::rethrow_exception(error);
                }
            }
        }

        aastore::StoreOptions fullOptions;
        fullOptions.storage_mode = aastore::StorageMode::FullMemory;
        aastore::ReadOptions reads;
        AAStoreTargetReader direct;
        direct.open(store, fullOptions, reads);
        direct.mapFromMMseqsKeys(keys);
        uint64_t total = 0;
        for (const uint32_t key : keys) total += source[key].size();
        if (!direct.fullMemory() || direct.totalLength() != total) {
            throw std::runtime_error(
                "AAStore FullMemory view has wrong metadata");
        }

        std::string decoded;
        for (size_t id = 0; id < keys.size(); ++id) {
            direct.get(id, decoded);
            if (decoded != source[keys[id]] ||
                direct.sequenceLength(id) != decoded.size()) {
                throw std::runtime_error(
                    "AAStore FullMemory direct read failed");
            }
        }

        {
            AAStoreTargetReader native;
            if (native.openForDatabase(database, 4)) {
                throw std::runtime_error("AAStore was enabled without explicit opt-in");
            }

            AAStoreTargetReader associated;
            if (!associated.openForDatabase(database, 4, 0, true)) {
                throw std::runtime_error(
                    "AAStore associated random reader did not open");
            }
            associated.mapFromMMseqsKeys(keys);
            check_batch(associated, source, keys, 4);

            AAStoreTargetReader batched;
            if (!batched.openForDatabase(
                    database, 4, std::numeric_limits<uint64_t>::max(), true) ||
                batched.fullMemory()) {
                throw std::runtime_error(
                    "exhausted memory budget enabled resident reads");
            }
            batched.mapFromMMseqsKeys(keys);
            check_batch(batched, source, keys, 4);

            AAStoreTargetReader missing;
            if (missing.openForDatabase((directory / "missing").string(), 4, 0, true)) {
                throw std::runtime_error(
                    "unassociated database unexpectedly enabled AAStore");
            }

            {
                std::ofstream locator(database + ".aastore.path");
                locator << (directory / "missing_store").string() << '\n';
            }
            AAStoreTargetReader defaultNative;
            if (defaultNative.openForDatabase(database, 4)) {
                throw std::runtime_error(
                    "invalid association affected default native reads");
            }
            bool rejected = false;
            try {
                AAStoreTargetReader invalidAssociation;
                invalidAssociation.openForDatabase(database, 4, 0, true);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            if (!rejected) {
                throw std::runtime_error("invalid database association was accepted");
            }
        }

        std::filesystem::remove_all(directory);
        std::cout
            << "AAStore sequence/records memory budgets, association, random batches, "
               "ID views, pread/io_uring, and FullMemory direct reads: passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what()
                  << "; fixture retained at " << directory << '\n';
        return 1;
    }
    return 0;
}
