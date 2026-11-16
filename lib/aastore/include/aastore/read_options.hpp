#ifndef AASTORE_READ_OPTIONS_HPP
#define AASTORE_READ_OPTIONS_HPP

#include <cstddef>
#include <cstdint>

namespace aastore {

inline constexpr uint64_t kByteRangeMergeGapBytes = 4096;
inline constexpr uint64_t kMaxMergedRangeBytes = 16 * 1024;
inline constexpr uint64_t kMaxExactMergedRangeBytes = 1 * 1024 * 1024;
inline constexpr uint64_t kIoBufferBytes = 16 * 1024 * 1024;

enum class RecordFields : uint8_t {
    None = 0,
    Name = 1,
    Description = 2,
    Sequence = 4,
    All = 7
};

constexpr RecordFields operator|(RecordFields left, RecordFields right) {
    return static_cast<RecordFields>(
        static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

constexpr RecordFields operator&(RecordFields left, RecordFields right) {
    return static_cast<RecordFields>(
        static_cast<uint8_t>(left) & static_cast<uint8_t>(right));
}

constexpr bool has_field(RecordFields fields, RecordFields field) {
    return (fields & field) != RecordFields::None;
}

enum class StorageMode : uint8_t {
    NoMemory = 0,
    PartialMemory = 1,
    FullMemory = 3
};

enum class IndexLoadMode : uint8_t {
    Mmap,
    Read
};

struct StoreOptions {
    uint64_t preload_bytes = 0;
    size_t threads = 8;
    bool resident_names = false;
    bool resident_descriptions = false;
    IndexLoadMode index_load_mode = IndexLoadMode::Mmap;

    void validate() const;
};

enum class IoBackend : uint8_t {
    Auto,
    Pread,
    IoUring
};

struct ReadOptions {
    size_t threads = 8;
    IoBackend io_backend = IoBackend::Auto;
    size_t io_queue_depth = 64;
    uint64_t io_buffer_bytes = kIoBufferBytes;
    bool input_sorted = false;
    uint64_t merge_gap_bytes = kByteRangeMergeGapBytes;
    uint64_t max_merged_range_bytes = kMaxMergedRangeBytes;
    uint64_t max_exact_merged_range_bytes = kMaxExactMergedRangeBytes;
    bool collect_diagnostics = false;

    void validate() const;
};

struct BatchStats {
    bool full_memory = false;
    bool prefix_memory = false;
    bool io_uring = false;
    uint64_t requested_sequences = 0;
    uint64_t requested_residues = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t cache_hit_residues = 0;
    uint64_t cache_miss_residues = 0;
    uint64_t unique_packed_bytes = 0;
    uint64_t ranges = 0;
    uint64_t merged_gaps = 0;
    uint64_t free_merges = 0;
    uint64_t read_bytes = 0;
    uint64_t overread_bytes = 0;
    uint64_t max_range_bytes = 0;
    uint64_t workers = 0;
    uint64_t io_workers = 0;
    uint64_t decode_workers = 0;
    uint64_t io_queue_depth = 0;
    uint64_t io_submissions = 0;
    uint64_t io_completions = 0;
    uint64_t io_submit_calls = 0;
    uint64_t io_submit_ns = 0;
    uint64_t io_wait_calls = 0;
    uint64_t io_empty_waits = 0;
    uint64_t layout_ns = 0;
    uint64_t sort_ns = 0;
    uint64_t range_plan_ns = 0;
    uint64_t worker_wall_ns = 0;
    uint64_t worker_max_busy_ns = 0;
    uint64_t read_ns = 0;
    uint64_t decode_ns = 0;
    uint64_t cache_decode_ns = 0;
    bool diagnostics_collected = false;
};

struct TextBatchStats {
    bool resident = false;
    bool io_uring = false;
    uint64_t requested_strings = 0;
    uint64_t requested_bytes = 0;
    uint64_t unique_bytes = 0;
    uint64_t ranges = 0;
    uint64_t merged_gaps = 0;
    uint64_t read_bytes = 0;
    uint64_t overread_bytes = 0;
    uint64_t max_range_bytes = 0;
};

}  // namespace aastore

#endif
