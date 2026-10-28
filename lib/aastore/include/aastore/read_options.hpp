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

// 开库时根据 preload_bytes 自动确定的编码序列驻留状态。
// NoMemory 只表示不主动预载序列，索引和读取缓冲仍可占用内存。
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
    // 编码序列预载的字节预算，不包含索引、文本列或解码输出。
    // 0 不预载；不足全量时预载固定前缀；足够时自动使用全内存读取。
    uint64_t preload_bytes = 0;
    // 预加载的最大工作线程数，包含调用线程。
    size_t threads = 8;
    // 打开时将完整名称正文读入 Store 持有的内存，后续名称批读不访问正文文件。
    bool resident_names = false;
    // 打开时将完整描述正文读入 Store 持有的内存；与名称及序列驻留选项独立。
    bool resident_descriptions = false;
    // 序列、名称和描述的边界索引：只读映射，或完整读入自有内存。
    IndexLoadMode index_load_mode = IndexLoadMode::Mmap;

    // 校验索引加载方式；所有预载预算有效，零线程保留单线程语义。
    void validate() const;
};

enum class IoBackend : uint8_t {
    Auto,
    Pread,
    IoUring
};

struct ReadOptions {
    // 每次批读用于排序、范围规划、I/O 和解码的最大工作线程数；
    // 包含调用线程，零按一个线程处理。
    size_t threads = 8;
    // 读取后端；io_uring 按能力选择 READ/READV，旧内核使用 READV。
    // Auto 在初始化或操作码不可用时回退到 pread；实际读错仍抛出异常。
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
    // Sparse 规划中允许跨越并合并的最大物理间隙（字节）。
    uint64_t merge_gap_bytes = kByteRangeMergeGapBytes;
    // Sparse 规划中跨越非零间隙后，单个合并范围的最大字节数；
    // 零表示不限制。
    uint64_t max_merged_range_bytes = kMaxMergedRangeBytes;
    // Sparse 规划中仅重叠或首尾相接时，单个合并范围的最大字节数；
    // 零表示不限制。
    uint64_t max_exact_merged_range_bytes = kMaxExactMergedRangeBytes;
    // 收集读取放大、合并次数、命中/未命中残基数、阶段耗时及 submit/wait 诊断。
    // 默认关闭；请求量、命中条数、读取字节数、范围/线程/队列及 SQE/CQE 计数始终保留。
    bool collect_diagnostics = false;

    // 拒绝未知后端；零线程、零队列深度及零预算保留原有约定。
    void validate() const;
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
    // 以下三项仅在 collect_diagnostics=true 时采集，否则为零。
    uint64_t cache_hit_residues = 0;
    uint64_t cache_miss_residues = 0;
    uint64_t unique_packed_bytes = 0;
    uint64_t ranges = 0;
    // 合并次数仅在 collect_diagnostics=true 时采集，否则为零。
    uint64_t merged_gaps = 0;
    uint64_t free_merges = 0;
    uint64_t read_bytes = 0;
    // 依赖 unique_packed_bytes，仅在 collect_diagnostics=true 时采集。
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
    // 仅在 ReadOptions::collect_diagnostics=true 时采集，否则为零。
    uint64_t unique_bytes = 0;
    uint64_t ranges = 0;
    // 仅在 ReadOptions::collect_diagnostics=true 时采集，否则为零。
    uint64_t merged_gaps = 0;
    uint64_t read_bytes = 0;
    // 依赖 unique_bytes，仅在 ReadOptions::collect_diagnostics=true 时采集。
    uint64_t overread_bytes = 0;
    uint64_t max_range_bytes = 0;
};

}  // namespace aastore

#endif
