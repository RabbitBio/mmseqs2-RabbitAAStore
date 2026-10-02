// 文件职责：定义序列请求、物理字节区间和范围规划结果。
// 提供请求排序与范围合并接口，把随机序列 ID 转换为后端可执行的有序读取计划。
#ifndef AASTORE_BYTE_RANGE_PLANNER_HPP
#define AASTORE_BYTE_RANGE_PLANNER_HPP

#include "aastore/store.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aastore {

struct SequenceRequest {
    uint64_t sequence_id;
    // 准备阶段保存原请求索引；输出布局完成后改为调用方输出地址
    // 或 Batch arena 中的字节偏移。
    size_t output_position;
    uint64_t start;
    uint64_t length;
};

using SequenceRequests =
    std::vector<SequenceRequest, NoInitAllocator<SequenceRequest>>;

struct PackedByteExtent {
    uint64_t begin = 0;
    uint64_t end = 0;
};

// 返回连续 5 位编码中请求需要的物理字节区间。
PackedByteExtent packed_byte_extent(const SequenceRequest& request);

struct ByteRange {
    uint64_t begin = 0;
    uint64_t end = 0;
    size_t request_begin = 0;
    size_t request_end = 0;
};

struct ReadPlan {
    std::vector<ByteRange> ranges;
    uint64_t requested_residues = 0;
    uint64_t unique_packed_bytes = 0;
    uint64_t read_bytes = 0;
    uint64_t sparse_ranges = 0;
    uint64_t scan_ranges = 0;
    uint64_t merged_gaps = 0;
    uint64_t free_merges = 0;
    uint64_t max_range_bytes = 0;
};

// 按序列 ID 对请求排序，并按规模选择比较排序或并行基数排序。
void sort_requests_by_sequence_id(SequenceRequests& requests,
                                  size_t thread_count,
                                  uint64_t sequence_count);

// 为按序列 ID 有序的请求计算范围合并方案；输入必须已排序。
ReadPlan plan_byte_ranges(const SequenceRequests& sorted_requests,
                          const ReadOptions& options);

}  // namespace aastore

#endif
