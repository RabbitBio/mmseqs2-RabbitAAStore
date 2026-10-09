#include "AAStoreSequenceAccess.h"

#include "AAStoreMemoryBudget.h"
#include "Debug.h"
#include "Sequence.h"
#include "Util.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>

AAStoreSequenceAccess::AAStoreSequenceAccess(
        AAStoreTargetReader& store, DBReader<unsigned int>* native,
        unsigned int thread, size_t computeThreads, bool checkFullMemoryLength)
    : store_(store), native_(native), thread_(thread),
      computeThreads_(computeThreads), checkFullMemoryLength_(checkFullMemoryLength) {
    if (!store_.enabled() && native_ == NULL) {
        throw std::invalid_argument("Native sequence access requires a database reader");
    }
    if (store_.fullMemory()) {
        queryBuffer_.reserve(32768);
        targetBuffer_.reserve(32768);
    }
}

size_t AAStoreSequenceAccess::size() const {
    return store_.enabled() ? store_.size() : native_->getSize();
}

size_t AAStoreSequenceAccess::getId(unsigned int key) const {
    return store_.enabled() ? store_.getId(key) : native_->getId(key);
}

unsigned int AAStoreSequenceAccess::getDbKey(size_t id) const {
    return store_.enabled() ? store_.getDbKey(id) : native_->getDbKey(id);
}

bool AAStoreSequenceAccess::batched() const noexcept {
    return store_.enabled() && !store_.fullMemory();
}

char* AAStoreSequenceAccess::sequence(
        size_t id, size_t& length, std::string& scratch) {
    if (id >= size()) return NULL;
    if (!store_.enabled()) {
        length = native_->getSeqLen(id);
        return native_->getData(id, thread_);
    }
    try {
        if (store_.fullMemory()) {
            if (checkFullMemoryLength_ &&
                store_.sequenceLength(id) > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                throw std::length_error("AAStore sequence exceeds alignment length range");
            }
            store_.get(id, scratch);
            length = scratch.size();
            return scratch.data();
        }
        std::string_view view;
        if (!batch_.sequences.view(id, view)) {
            throw std::logic_error("Sequence is missing from AAStore query batch");
        }
        length = view.size();
        return const_cast<char*>(view.data());
    } catch (const std::exception& error) {
        fail(error);
    }
}

char* AAStoreSequenceAccess::query(size_t id, size_t& length) {
    return sequence(id, length, queryBuffer_);
}

char* AAStoreSequenceAccess::target(size_t id, size_t& length) {
    return sequence(id, length, targetBuffer_);
}

bool AAStoreSequenceAccess::mapTarget(Sequence& sequence, size_t id, unsigned int key) {
    size_t length = 0;
    char* data = target(id, length);
    if (data == NULL) return false;
    sequence.mapSequence(id, key, data, length);
    return true;
}

void AAStoreSequenceAccess::accumulateStats(Stats& total, const Stats& worker) {
    total.batches += worker.batches;
    total.candidateOccurrences += worker.candidateOccurrences;
    total.maxCandidateBytes = std::max(total.maxCandidateBytes, worker.maxCandidateBytes);
    AAStoreTargetReader::accumulateIoStats(total.io, worker.io);
}

void AAStoreSequenceAccess::logStats(const char* stage, const std::vector<Stats>& workers) {
    Stats total;
    for (const Stats& worker : workers) accumulateStats(total, worker);
    Debug(Debug::INFO) << "AAStore " << stage << " batches=" << total.batches
        << " candidate_occurrences=" << total.candidateOccurrences
        << " max_candidate_bytes=" << total.maxCandidateBytes
        << " ranges=" << total.io.ranges << " read_bytes=" << total.io.read_bytes
        << " requests=" << total.io.io_submissions << "\n";
}

[[noreturn]] void AAStoreSequenceAccess::fail(const std::exception& error) {
    Debug(Debug::ERROR) << error.what() << "\n";
    EXIT(EXIT_FAILURE);
    std::abort();
}

AAStoreQueryBatches::AAStoreQueryBatches(
        AAStoreTargetReader& store,
        DBReader<unsigned int>& candidates, bool sameQueries,
        size_t begin, size_t end, uint64_t batchBytes)
    : store_(store), candidates_(candidates),
      sameQueries_(sameQueries), next_(begin), end_(end), batchBytes_(batchBytes) {
    if (batchBytes == 0 || begin > end || end > candidates.getSize()) {
        throw std::invalid_argument("Invalid AAStore query batch range or threshold");
    }
}

bool AAStoreQueryBatches::next(AAStoreSequenceAccess& worker) {
    try {
        if (!worker.batched() || &worker.store_ != &store_) {
            throw std::logic_error("AAStore batch worker uses a different database");
        }
        uint64_t bytes = 0;
        uint64_t occurrences = 0;
        AAStoreTargetReader::ThreadBatch& batch = worker.batch_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (next_ >= end_) return false;
            batch.ids.clear();
            batch.begin = next_;
            batch.end = next_;
            for (size_t query = next_; query < end_; ++query) {
                char* candidates = candidates_.getData(query, worker.thread_);
                if (sameQueries_ && *candidates != '\0') {
                    const unsigned int queryKey = candidates_.getDbKey(query);
                    const size_t queryId = worker.getId(queryKey);
                    if (queryId == UINT_MAX) {
                        throw std::runtime_error("Query sequence " + std::to_string(queryKey) +
                            " is required in the prefiltering, but is not contained in the query sequence database");
                    }
                    batch.ids.push_back(queryId);
                }
                while (*candidates != '\0') {
                    const unsigned int targetKey = Util::fast_atoi<unsigned int>(candidates);
                    const size_t targetId = worker.getId(targetKey);
                    if (targetId == UINT_MAX) {
                        throw std::runtime_error("Sequence " + std::to_string(targetKey) +
                            " is required in the prefiltering, but is not contained in the target sequence database");
                    }
                    batch.ids.push_back(targetId);
                    bytes = aastore_memory::add_bytes(bytes,
                        aastore_memory::add_bytes(store_.sequenceLength(targetId), 1));
                    ++occurrences;
                    candidates = Util::skipLine(candidates);
                }
                batch.end = query + 1;
                // Retain complete query boundaries and the previous definition
                // of candidate bytes, including repeated target occurrences.
                if (bytes >= batchBytes_) break;
            }
            next_ = batch.end;
        }
        store_.fetch_into(batch, worker.computeThreads_);
        ++worker.stats_.batches;
        worker.stats_.candidateOccurrences += occurrences;
        worker.stats_.maxCandidateBytes = std::max(worker.stats_.maxCandidateBytes, bytes);
        AAStoreTargetReader::accumulateIoStats(worker.stats_.io, batch.sequences.fetched.stats);
        return true;
    } catch (const std::exception& error) {
        AAStoreSequenceAccess::fail(error);
    }
}
