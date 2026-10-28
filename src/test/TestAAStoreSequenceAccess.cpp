#include "AAStoreSequenceAccess.h"
#include "DBWriter.h"
#include "Parameters.h"
#include "Util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

const char* binary_name = "test_aastore_sequence_access";
DEFAULT_PARAMETER_SINGLETON_INIT

namespace {
constexpr size_t kWorkers = 4, kBatchBytes = 100;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Query { unsigned int key; std::vector<unsigned int> targets; };
// Costs count each candidate occurrence's sequence length plus one terminator.
// Queries and ID bookkeeping do not set the batch boundary.
const std::vector<Query> kQueries{
    {0, {1}}, {7, {2}}, {6, {3}}, {9, {8}},  // 20 + 30 + 40 + 10 = 100
    {4, {1, 1, 1}}, {3, {1, 1}},           // five occurrences = 100
    {2, {4, 4, 4}},                        // one complete query = 150
    {5, {}}, {1, {2}}, {8, {3}}            // empty query, then 30 + 40
};
const std::vector<std::pair<size_t, size_t>> kRanges{{0, 4}, {4, 6}, {6, 7}, {7, 10}};
const std::array<size_t, 4> kOccurrences{{4, 5, 3, 2}}, kBytes{{100, 100, 150, 70}};
std::string candidates(const std::vector<unsigned int>& keys) {
    std::string text;
    for (unsigned int key : keys) text += std::to_string(key) + "\t100\t0\n";
    return text;
}
bool same_io(const aastore::BatchStats& a, const aastore::BatchStats& b) {
    return a.ranges == b.ranges && a.read_bytes == b.read_bytes &&
           a.io_submissions == b.io_submissions;
}
void validate_batch(AAStoreSequenceAccess& access, DBReader<unsigned int>& pref, const std::vector<std::string>& source,
                    unsigned int thread) {
    const auto before = access.stats();
    std::vector<std::pair<std::string_view, unsigned int>> borrowed;
    std::vector<unsigned int> visited;
    for (size_t index = access.batchBegin(); index < access.batchEnd(); ++index) {
        const auto& expected = kQueries.at(index);
        require(pref.getDbKey(index) == expected.key, "collector reordered queries");
        char* scoring = pref.getData(index, thread);
        require(std::string(scoring) == candidates(expected.targets),
                "collector modified candidate text");
        if (*scoring == '\0') continue;
        size_t length = 0;
        char* data = access.query(access.getId(expected.key), length);
        require(data != NULL && std::string_view(data, length) == source[expected.key],
                "batched query ID mapping failed");
        borrowed.emplace_back(std::string_view(data, length), expected.key);
        for (unsigned int key : expected.targets) {
            require(Util::fast_atoi<unsigned int>(scoring) == key, "candidate order changed");
            data = access.target(access.getId(key), length);
            require(data != NULL && std::string_view(data, length) == source[key],
                    "batched candidate ID mapping failed");
            borrowed.emplace_back(std::string_view(data, length), key);
            visited.push_back(key);
            scoring = Util::skipLine(scoring);
        }
        require(*scoring == '\0', "scoring did not consume the complete query");
    }
    // Realign/alternative alignment can revisit every prefetched candidate.
    for (auto key = visited.rbegin(); key != visited.rend(); ++key) {
        size_t length = 0;
        char* data = access.target(access.getId(*key), length);
        require(data != NULL && std::string_view(data, length) == source[*key],
                "realign revisit missed a prefetched candidate");
    }
    for (const auto& value : borrowed)
        require(value.first == source[value.second], "read invalidated a batch-wide sequence view");
    require(same_io(before.io, access.stats().io) && before.batches == access.stats().batches &&
            before.candidateOccurrences == access.stats().candidateOccurrences,
            "scoring or realign performed I/O after prefetch");
}
void check_batched(const std::string& store, DBReader<unsigned int>& pref, const std::vector<std::string>& source,
                   aastore::StorageMode mode) {
    aastore::StoreOptions options;
    options.preload_bytes = mode == aastore::StorageMode::PartialMemory ? 17 : 0;
    aastore::ReadOptions reads;
    reads.io_backend = aastore::IoBackend::Pread;
    reads.collect_diagnostics = true;
    AAStoreTargetReader reader;
    reader.open(store, options, reads);
    require(!reader.fullMemory(), "disk-backed fixture became full-memory");
    AAStoreSequenceAccess access(reader, NULL, 0, kWorkers);
    require(access.batched(), "disk-backed source bypassed batch scheduling");
    AAStoreQueryBatches batches(reader, pref, true, 0, pref.getSize(), kBatchBytes);
    size_t count = 0, occurrences = 0, maxBytes = 0;
    while (batches.next(access)) {
        require(count < kRanges.size(), "collector created extra batches");
        require(std::make_pair(access.batchBegin(), access.batchEnd()) == kRanges[count],
                "complete-query candidate-byte boundary changed");
        occurrences += kOccurrences[count];
        maxBytes = std::max(maxBytes, kBytes[count]);
        require(access.stats().batches == count + 1 &&
                access.stats().candidateOccurrences == occurrences &&
                access.stats().maxCandidateBytes == maxBytes,
                "batch counters included queries or deduplicated occurrences");
        validate_batch(access, pref, source, 0);
        ++count;
    }
    require(count == kRanges.size() && !batches.next(access),
            "collector lost the final batch or advanced after exhaustion");

    AAStoreQueryBatches shared(reader, pref, true, 0, pref.getSize(), kBatchBytes);
    std::array<std::vector<size_t>, kWorkers> assigned;
    std::array<std::vector<std::pair<size_t, size_t>>, kWorkers> ranges;
    std::array<AAStoreSequenceAccess::Stats, kWorkers> stats;
    std::array<std::exception_ptr, kWorkers> failures;
    std::vector<std::thread> workers;
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    for (unsigned int thread = 0; thread < kWorkers; ++thread)
        workers.emplace_back([&, thread]() {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            try {
                AAStoreSequenceAccess worker(reader, NULL, thread, kWorkers);
                while (shared.next(worker)) {
                    ranges[thread].emplace_back(worker.batchBegin(), worker.batchEnd());
                    for (size_t query = worker.batchBegin(); query < worker.batchEnd(); ++query)
                        assigned[thread].push_back(query);
                    validate_batch(worker, pref, source, thread);
                }
                stats[thread] = worker.stats();
            } catch (...) { failures[thread] = std::current_exception(); }
        });
    while (ready.load() != kWorkers) std::this_thread::yield();
    start.store(true);
    for (auto& worker : workers) worker.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    std::vector<size_t> queries;
    std::vector<std::pair<size_t, size_t>> boundaries;
    size_t totalBatches = 0, totalOccurrences = 0;
    for (size_t thread = 0; thread < kWorkers; ++thread) {
        queries.insert(queries.end(), assigned[thread].begin(), assigned[thread].end());
        boundaries.insert(boundaries.end(), ranges[thread].begin(), ranges[thread].end());
        totalBatches += stats[thread].batches;
        totalOccurrences += stats[thread].candidateOccurrences;
    }
    std::sort(queries.begin(), queries.end());
    std::sort(boundaries.begin(), boundaries.end());
    require(queries.size() == kQueries.size(), "thread dispatch lost or repeated queries");
    for (size_t query = 0; query < queries.size(); ++query)
        require(queries[query] == query, "a query was not processed exactly once");
    require(boundaries == kRanges && totalBatches == 4 && totalOccurrences == 14,
            "thread dispatch changed candidate-byte batching");

    // Preserve the enclosing iteration's query range.
    AAStoreQueryBatches partial(reader, pref, true, 1, 4, kBatchBytes);
    AAStoreSequenceAccess part(reader, NULL, 0, kWorkers);
    require(partial.next(part) && part.batchBegin() == 1 && part.batchEnd() == 4,
            "collector ignored its outer query range");
    require(part.stats().candidateOccurrences == 3 && part.stats().maxCandidateBytes == 80,
            "collector counted candidates outside its range");
    validate_batch(part, pref, source, 0);
    require(!partial.next(part), "collector crossed its range end");
}
void check_direct(AAStoreTargetReader& reader, DBReader<unsigned int>& native,
                  const std::vector<std::string>& source) {
    AAStoreSequenceAccess access(reader, reader.enabled() ? NULL : &native, 0, kWorkers);
    require(!access.batched(), "full-memory or native reads entered batch scheduling");
    require(access.size() == source.size(), "direct source has the wrong number of IDs");
    for (unsigned int key = 0; key < source.size(); ++key)
        require(access.getDbKey(access.getId(key)) == key, "direct key lookup did not roundtrip");
    size_t queryLength = 0;
    char* query = access.query(access.getId(0), queryLength);
    require(query != NULL && std::string_view(query, queryLength) == source[0],
            "direct query mapping failed");
    for (unsigned int key : {1, 1, 3, 8, 2, 4, 1}) {
        size_t length = 0;
        char* target = access.target(access.getId(key), length);
        require(target != NULL && std::string_view(target, length) == source[key],
                "direct target mapping failed");
        require(std::string_view(query, queryLength) == source[0], "target overwrote direct query");
    }
    require(access.stats().batches == 0 && access.stats().candidateOccurrences == 0,
            "direct reads produced batch statistics");
}
}

int main() {
    std::string path = (std::filesystem::temp_directory_path() /
                        "mmseqs-aastore-sequence-access.XXXXXX").string();
    const char* created = ::mkdtemp(path.data());
    if (!created) return 1;
    const std::filesystem::path directory(created);
    try {
        const std::vector<std::string> source{
            std::string(151, 'A'), std::string(19, 'D'), std::string(29, 'G'),
            std::string(39, 'H'), std::string(49, 'I'), std::string(61, 'K'),
            std::string(73, 'L'), "MNPQRST", std::string(9, 'N'), "ACD"};
        {
            std::ofstream fasta(directory / "input.fa");
            for (size_t key = 0; key < source.size(); ++key)
                fasta << '>' << key << '\n' << source[key] << '\n';
        }
        const std::string store = (directory / "store").string();
        const std::string database = (directory / "database").string();
        const std::string prefDatabase = (directory / "pref").string();
        aastore::build_fasta(directory / "input.fa", store);
        AAStoreTargetReader::associateDatabase(database, store);
        DBWriter writer(database.c_str(), (database + ".index").c_str(), 1, false,
                        Parameters::DBTYPE_AMINO_ACIDS);
        writer.open();
        for (unsigned int key : {3, 0, 8, 1, 4, 7, 2, 9, 6, 5}) {
            const std::string sequence = source[key] + '\n';
            writer.writeData(sequence.data(), sequence.size(), key, 0);
        }
        writer.close();
        DBWriter prefWriter(prefDatabase.c_str(), (prefDatabase + ".index").c_str(),
                            1, false, Parameters::DBTYPE_PREFILTER_RES);
        prefWriter.open();
        for (const auto& query : kQueries) {
            const std::string text = candidates(query.targets);
            prefWriter.writeData(text.data(), text.size(), query.key, 0);
        }
        prefWriter.close();
        DBReader<unsigned int> native(database.c_str(), (database + ".index").c_str(), kWorkers,
                                      DBReader<unsigned int>::USE_INDEX);
        native.open(DBReader<unsigned int>::LINEAR_ACCCESS);
        require(native.getDbKey(0) != 0, "fixture did not exercise nonidentity local IDs");
        DBReader<unsigned int> pref(prefDatabase.c_str(), (prefDatabase + ".index").c_str(), kWorkers,
                                    DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
        pref.open(DBReader<unsigned int>::LINEAR_ACCCESS);
        for (aastore::StorageMode mode : {aastore::StorageMode::NoMemory,
                                        aastore::StorageMode::PartialMemory})
            check_batched(store, pref, source, mode);
        aastore::StoreOptions options;
        options.preload_bytes = UINT64_MAX;
        AAStoreTargetReader full;
        full.open(store, options, aastore::ReadOptions{});
        check_direct(full, native, source);
        {
            const std::filesystem::path index = database + ".index";
            const std::filesystem::path hidden = database + ".index.hidden";
            std::filesystem::rename(index, hidden);
            AAStoreTargetReader independent;
            require(independent.openForScoring(database, kWorkers, 0, true),
                    "RabbitAAStore scoring required a native sequence index");
            AAStoreSequenceAccess access(independent, NULL, 0, kWorkers);
            require(access.size() == source.size(), "independent scoring view has wrong size");
            for (unsigned int key = 0; key < source.size(); ++key)
                require(access.getId(key) == key && access.getDbKey(key) == key,
                        "full scoring view changed stable RabbitAAStore IDs");
            std::filesystem::rename(hidden, index);
        }
        DBReader<unsigned int> nativeData(database.c_str(), (database + ".index").c_str(), 1,
                                          DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
        nativeData.open(DBReader<unsigned int>::LINEAR_ACCCESS);
        AAStoreTargetReader disabled;
        check_direct(disabled, nativeData, source);
        nativeData.close();
        pref.close();
        native.close();
        std::filesystem::remove_all(directory);
        std::cout << "RabbitAAStore whole-query batches, duplicate-byte accounting, stable batch views, "
                     "prefetched revisits, thread dispatch, ID mapping and direct reads: passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "; fixture retained at " << directory << '\n';
        return 1;
    }
    return 0;
}
