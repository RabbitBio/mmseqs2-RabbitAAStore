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

// Random-access adapter shared by rescorediagonal and Alignment. MMseqs local
// IDs stay separate from AAStore physical IDs so createsubdb views can reuse
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

    AAStoreTargetReader() = default;
    ~AAStoreTargetReader();

    void open(const std::string& path,
              const aastore::StoreOptions& storeOptions,
              const aastore::ReadOptions& readOptions);
    // 仅显式启用时读取数据库关联的 AAStore；默认保持原生读取。
    bool openForDatabase(const std::string& database,
                         size_t computeThreads = 1,
                         uint64_t stageMemoryReserve = 0,
                         bool enabled = false);

    static std::string associatedStorePath(const std::string& database);
    static void associateDatabase(const std::string& database,
                                  const std::string& store);

    bool enabled() const noexcept;
    bool fullMemory() const noexcept;
    static constexpr size_t kThreadBatchBytes = 64 * 1024 * 1024;
    static constexpr size_t kThreadBatchIoBudget = 512;
    static size_t threadBatchQueueDepth(size_t computeThreads);
    void fetch_into(ThreadBatch& batch, size_t computeThreads) const;

    static void accumulateIoStats(aastore::BatchStats& total,
                                  const aastore::BatchStats& batch);
    static void logIoStats(const std::vector<aastore::BatchStats>& workers);

    uint64_t size() const;
    uint64_t totalLength() const;
    uint64_t sequenceLength(size_t mmseqsId) const;
    void mapFromMMseqsKeys(const std::vector<uint32_t>& keys);
    void validateSize(size_t mmseqsSize) const;

    // FullMemory fast path.
    void get(size_t mmseqsId, std::string& output) const;

private:
    void prepare_into(ThreadBatch& batch) const;
    void installStore(std::unique_ptr<aastore::Store> store);
    size_t store_id_for_mmseqs_id(size_t mmseqsId) const;

    std::unique_ptr<aastore::Store> store_;
    std::vector<aastore::SequenceId> mmseqsToAAStoreId_;
    bool hasIdMap_ = false;
    aastore::ReadOptions readOptions_;
    size_t trackedResidentBytes_ = 0;
};

#endif
