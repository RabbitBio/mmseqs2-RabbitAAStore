// 文件职责：集中定义存储模式、读取后端、线程与范围规划选项及读取统计。
// 字段掩码辅助函数用于选择记录的名称、描述和序列列。
#ifndef AASTORE_READ_OPTIONS_HPP
#define AASTORE_READ_OPTIONS_HPP

#include <cstddef>
#include <cstdint>

namespace aastore {

inline constexpr uint64_t kByteRangeMergeGapBytes = 4096;
inline constexpr uint64_t kMaxMergedRangeBytes = 16 * 1024;
inline constexpr uint64_t kMaxExactMergedRangeBytes = 1 * 1024 * 1024;
inline constexpr uint64_t kScanMaxRangeBytes = 256 * 1024;
inline constexpr uint64_t kIoBufferBytes = 16 * 1024 * 1024;

enum class RecordFields : uint8_t {
    None = 0,
    Name = 1,
    Description = 2,
    Sequence = 4,
    All = 7
};

// 合并两个字段掩码，使结果包含任一掩码指定的字段。
constexpr RecordFields operator|(RecordFields left, RecordFields right) {
    return static_cast<RecordFields>(
        static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

// 取两个字段掩码的交集，仅保留共同指定的字段。
constexpr RecordFields operator&(RecordFields left, RecordFields right) {
    return static_cast<RecordFields>(
        static_cast<uint8_t>(left) & static_cast<uint8_t>(right));
}

// 判断 fields 是否包含 field 中的任意一个字段位。
constexpr bool has_field(RecordFields fields, RecordFields field) {
    return (fields & field) != RecordFields::None;
}

enum class StorageMode : uint8_t {
    ByteRange = 0,
    PrefixMemory = 1,
    FullMemory = 3
};

struct StoreOptions {
    StorageMode storage_mode = StorageMode::ByteRange;
    // PrefixMemory 常驻 packed 前缀的字节上限。
    uint64_t cache_bytes = 0;
    // 预加载的最大工作线程数，包含调用线程。
    size_t threads = 8;
    bool resident_names = false;
    bool resident_descriptions = false;
};

enum class IoBackend : uint8_t {
    Auto,
    Pread,
    IoUring
};

enum class ReadStrategy : uint8_t {
    Sparse,
    Scan
};

struct ReadOptions {
    // 每次批读用于排序、范围规划、I/O 和解码的最大工作线程数；
    // 包含调用线程，零按一个线程处理。
    size_t threads = 8;
    // 读取后端；Auto 优先尝试 io_uring，不可用时回退到 pread。
    // Pread 强制使用 pread，IoUring 不可用时抛出异常。
    IoBackend io_backend = IoBackend::Auto;
    // 每个 io_uring 工作线程所管理 ring 的最大队列深度；
    // 实际深度还会受范围数量和 io_buffer_bytes 限制，零按一处理。
    size_t io_queue_depth = 64;
    // 所有 io_uring ring 合计的在途读取缓冲预算（字节）；
    // 用最大单范围大小保守估算并限制队列深度，零表示不限制。
    uint64_t io_buffer_bytes = kIoBufferBytes;
    // 输入 ID 是否已按 SequenceId 非递减排列；为 true 时跳过内部排序。
    bool input_sorted = false;
    // 范围读取策略；Sparse 保留稀疏范围，Scan 再将其合并为较大扫描范围。
    ReadStrategy read_strategy = ReadStrategy::Sparse;
    // Sparse 规划中允许跨越并合并的最大物理间隙（字节）。
    uint64_t merge_gap_bytes = kByteRangeMergeGapBytes;
    // Sparse 规划中跨越非零间隙后，单个合并范围的最大字节数；
    // 零表示不限制。
    uint64_t max_merged_range_bytes = kMaxMergedRangeBytes;
    // Sparse 规划中仅重叠或首尾相接时，单个合并范围的最大字节数；
    // 零表示不限制。
    uint64_t max_exact_merged_range_bytes = kMaxExactMergedRangeBytes;
    // Scan 策略二次合并后，单个扫描范围的最大字节数；零表示不限制。
    uint64_t scan_max_range_bytes = kScanMaxRangeBytes;
    // 收集阶段耗时和 submit/wait API 诊断计数；默认关闭，基本统计始终保留。
    bool collect_diagnostics = false;
};

// 一次序列批读的规划、前缀命中、I/O 和解码统计。
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
    uint64_t sparse_ranges = 0;
    uint64_t scan_ranges = 0;
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
    // 以下详细诊断仅在 ReadOptions::collect_diagnostics 为 true 时收集，否则为零。
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
    // 本次读取是否启用了详细诊断，用于区分未采集与测量值为零。
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
