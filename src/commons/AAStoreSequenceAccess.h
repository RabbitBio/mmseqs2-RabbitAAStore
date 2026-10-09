#ifndef AASTORE_SEQUENCE_ACCESS_H
#define AASTORE_SEQUENCE_ACCESS_H

#include "AAStoreTargetReader.h"
#include "DBReader.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

class Sequence;
class AAStoreQueryBatches;

// One workspace per compute thread. Nonresident sequence views stay valid
// throughout a claimed batch of complete queries.
class AAStoreSequenceAccess {
public:
    struct Stats {
        size_t batches = 0;
        uint64_t candidateOccurrences = 0;
        uint64_t maxCandidateBytes = 0;
        aastore::BatchStats io;
    };

    AAStoreSequenceAccess(AAStoreTargetReader& store,
                         DBReader<unsigned int>* native,
                         unsigned int thread, size_t computeThreads,
                         bool checkFullMemoryLength = false);

    size_t size() const;
    size_t getId(unsigned int key) const;
    unsigned int getDbKey(size_t id) const;

    bool batched() const noexcept;
    size_t batchBegin() const noexcept { return batch_.begin; }
    size_t batchEnd() const noexcept { return batch_.end; }
    char* query(size_t id, size_t& length);
    char* target(size_t id, size_t& length);
    bool mapTarget(Sequence& sequence, size_t id, unsigned int key);

    const Stats& stats() const noexcept { return stats_; }
    static void accumulateStats(Stats& total, const Stats& worker);
    static void logStats(const char* stage, const std::vector<Stats>& workers);

private:
    friend class AAStoreQueryBatches;
    char* sequence(size_t id, size_t& length, std::string& scratch);
    [[noreturn]] static void fail(const std::exception& error);

    AAStoreTargetReader& store_;
    DBReader<unsigned int>* native_;
    unsigned int thread_;
    size_t computeThreads_;
    bool checkFullMemoryLength_;
    AAStoreTargetReader::ThreadBatch batch_;
    std::string queryBuffer_;
    std::string targetBuffer_;
    Stats stats_;
};

// Shared only for assigning complete queries. The collector retains the
// existing candidate-byte threshold; sequence I/O runs outside the lock.
class AAStoreQueryBatches {
public:
    AAStoreQueryBatches(AAStoreTargetReader& store,
                       DBReader<unsigned int>& candidates,
                       bool sameQueries,
                       size_t begin, size_t end,
                       uint64_t batchBytes = AAStoreTargetReader::kThreadBatchBytes);
    bool next(AAStoreSequenceAccess& worker);

private:
    AAStoreTargetReader& store_;
    DBReader<unsigned int>& candidates_;
    bool sameQueries_;
    size_t next_;
    size_t end_;
    uint64_t batchBytes_;
    std::mutex mutex_;
};

#endif
