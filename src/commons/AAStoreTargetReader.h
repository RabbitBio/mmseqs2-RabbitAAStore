#ifndef AASTORE_TARGET_READER_H
#define AASTORE_TARGET_READER_H

#include "MemoryTracker.h"

#include <aastore/store.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Random-access adapter shared by alignment and sequence export. MMseqs local
// IDs stay separate from RabbitAAStore physical IDs so createsubdb views can reuse
// one external store without rewriting sequence data.
class AAStoreTargetReader : public MemoryTracker {
public:
    class IdPositionMap {
    public:
        void clear() noexcept {
            keys_.clear();
            values_.clear();
            mask_ = 0;
        }

        void prepare(size_t expected) {
            if (expected == 0) {
                clear();
                return;
            }
            size_t capacity = 16;
            while (capacity < expected * 10 / 7 + 1) capacity <<= 1;
            keys_.assign(capacity, kEmpty);
            values_.resize(capacity);
            mask_ = capacity - 1;
        }

        bool insert_if_absent(size_t key, size_t value) noexcept;
        bool find(size_t key, size_t& value) const noexcept;

    private:
        static constexpr size_t kEmpty = static_cast<size_t>(-1);

        static size_t hash(size_t value) noexcept {
            uint64_t x = static_cast<uint64_t>(value);
            x += UINT64_C(0x9e3779b97f4a7c15);
            x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
            return static_cast<size_t>(x ^ (x >> 31));
        }

        std::vector<size_t> keys_;
        std::vector<size_t> values_;
        size_t mask_ = 0;
    };

    struct Batch {
        aastore::Batch fetched;
        IdPositionMap positions;

        bool view(size_t mmseqsId, std::string_view& sequence) const;
    };

    // One workspace per compute thread. Views remain valid until the next
    // fetch_into call on the same workspace.
    struct ThreadBatch {
        Batch sequences;
        std::vector<size_t> ids;
        std::vector<aastore::SequenceId> physicalIds;
        std::vector<char*> outputs;
        size_t begin = 0;
        size_t end = 0;
    };

    struct RecordThreadBatch {
        aastore::RecordBatch records;
        std::vector<aastore::SequenceId> physicalIds;
        IdPositionMap positions;
    };

    AAStoreTargetReader();
    ~AAStoreTargetReader();

    void open(const std::string& path,
              const aastore::StoreOptions& storeOptions,
              const aastore::ReadOptions& readOptions);
    // 仅显式启用时读取数据库关联的 RabbitAAStore；默认保持原生读取。
    bool openForDatabase(const std::string& database,
                         size_t computeThreads = 1,
                         uint64_t stageMemoryReserve = 0,
                         bool enabled = false,
                         bool preferRecordFields = false,
                         uint64_t recordSafetyReserve = 0);
    // Scoring uses RabbitAAStore metadata and a compact subset membership file;
    // it does not load a native sequence DBReader/index.
    bool openForScoring(const std::string& database,
                        size_t computeThreads = 1,
                        uint64_t stageMemoryReserve = 0,
                        bool enabled = false);
    // TSV needs text columns only, never a packed-sequence cache.
    bool openForHeaders(const std::string& database, size_t computeThreads,
                        uint64_t stageMemoryReserve, bool fullHeader);
    void getHeaderFields(size_t key, std::string& name,
                         std::string& description) const;
    bool headerDescriptionsEnabled() const noexcept;

    // Certified record columns retain either raw native-header separators or
    // the default RabbitAAStore name/description fields.
    static bool hasNativeRecordHeaders(const std::string& database);
    bool recordsEnabled() const noexcept;
    bool recordsDenseIds() const noexcept;
    std::string_view headerSeparator(std::string_view description) const noexcept;
    uint64_t recordLength(size_t mmseqsId) const;
    void fetch_records(const std::vector<size_t>& ids,
                       RecordThreadBatch& batch, size_t computeThreads,
                       size_t readThreads = 1) const;
    void fetch_headers(const std::vector<size_t>& ids,
                       RecordThreadBatch& batch, size_t readThreads = 1) const;

    static std::string associatedStorePath(const std::string& database);
    static aastore::IndexLoadMode configuredIndexLoadMode();
    static void associateDatabase(const std::string& database,
                                  const std::string& store);
    static void associateSubsetDatabase(const std::string& database,
                                        const std::string& store,
                                        std::vector<uint32_t> keys);

    bool enabled() const noexcept;
    bool fullMemory() const noexcept;
    static constexpr size_t kThreadBatchBytes = 64 * 1024 * 1024;
    // Record offsets, ID vectors and temporary read requests also consume
    // space; accounting per member keeps even empty records bounded.
    static constexpr size_t kRecordBatchOverheadBytes = 256;
    static constexpr size_t kThreadBatchIoBudget = 512;
    static size_t threadBatchQueueDepth(size_t computeThreads);
    void fetch_into(ThreadBatch& batch, size_t computeThreads) const;

    static void accumulateIoStats(aastore::BatchStats& total,
                                  const aastore::BatchStats& batch);
    static void logIoStats(const std::vector<aastore::BatchStats>& workers);

    uint64_t size() const;
    uint64_t totalLength() const;
    uint64_t sequenceLength(size_t mmseqsId) const;
    uint64_t maxSequenceLength() const;
    size_t getId(unsigned int key) const;
    unsigned int getDbKey(size_t mmseqsId) const;
    void mapFromMMseqsKeys(const std::vector<uint32_t>& keys);
    void validateSize(size_t mmseqsSize) const;

    // FullMemory fast path.
    void get(size_t mmseqsId, std::string& output) const;

private:
    void prepare_into(ThreadBatch& batch) const;
    void installStore(std::unique_ptr<aastore::Store> store);
    size_t store_id_for_mmseqs_id(size_t mmseqsId) const;
    bool openAssociated(const std::string& database, size_t computeThreads,
                        uint64_t stageMemoryReserve, bool preferRecordFields,
                        uint64_t recordSafetyReserve, bool scoring,
                        bool headersOnly = false, bool fullHeader = false);
    void updateViewStatistics() const;
    void resetIdMap();

    std::unique_ptr<aastore::Store> store_;
    std::vector<uint32_t> mmseqsToAAStoreId_;
    // Only the legacy explicit map API can request a non-key-sorted view.
    // New subset files are key-sorted and need no inverse table.
    std::vector<uint32_t> keyOrder_;
    bool hasIdMap_ = false;
    mutable bool viewStatisticsKnown_ = false;
    mutable uint64_t viewTotalLength_ = 0;
    mutable uint64_t viewMaxLength_ = 0;
    aastore::ReadOptions readOptions_;
    size_t trackedResidentBytes_ = 0;
    bool recordsEnabled_ = false;
    bool recordsDenseIds_ = false;
    bool recordHeaderNeedsSpace_ = false;
    bool headerDescriptionsEnabled_ = false;
};

#endif
