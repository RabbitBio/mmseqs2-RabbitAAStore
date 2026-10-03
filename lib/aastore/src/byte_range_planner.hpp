// 文件职责：定义序列请求、物理字节区间和范围规划结果。
// 提供请求排序与范围合并接口，把随机序列 ID 转换为后端可执行的有序读取计划。
#ifndef AASTORE_BYTE_RANGE_PLANNER_HPP
#define AASTORE_BYTE_RANGE_PLANNER_HPP

#include "aastore/store.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aastore {

// 一条逻辑读取请求：记录序列边界与输出位置，排序后仍可回填原请求的结果。
struct SequenceRequest {
    // 数据库中的序列 ID，用作请求排序键。
    uint64_t sequence_id;
    // 准备阶段保存原请求索引；输出布局完成后改为调用方输出地址
    // 或 Batch arena 中的字节偏移。
    size_t output_position;
    // 在全库连续字符流中的起点，单位为字符而非 packed 字节。
    uint64_t start;
    // 要解码的字符数；零长度序列无需磁盘读取。
    uint64_t length;
};

using SequenceRequests =
    std::vector<SequenceRequest, NoInitAllocator<SequenceRequest>>;

// 一条序列解码所需的 packed 文件半开字节区间，含起点对齐所需字节。
struct PackedByteExtent {
    // packed.bin 中的起始字节偏移，包含此位置。
    uint64_t begin = 0;
    // 结束字节偏移，不包含此位置；读取长度为 end - begin。
    uint64_t end = 0;
};

// 返回连续 5 位编码中请求需要的物理字节区间。
PackedByteExtent packed_byte_extent(const SequenceRequest& request);

// 可一次读取的合并物理区间，并关联该区间内需要解码的逻辑请求。
struct ByteRange {
    // 源数据文件中的读取起点，单位字节；序列读取时指向 packed.bin。
    uint64_t begin = 0;
    // 读取终点的下一字节偏移，长度为 end - begin。
    uint64_t end = 0;
    // 有序读取请求数组中的首个请求下标，不是序列 ID。
    size_t request_begin = 0;
    // 最后一个关联请求的下一下标，形成 [request_begin, request_end)。
    size_t request_end = 0;
};

// 本批磁盘请求的有序范围计划及字节统计，不包含实际读取缓冲和序列内容。
struct ReadPlan {
    // 按文件偏移排列的合并区间，后端按此列表分配和提交读取。
    std::vector<ByteRange> ranges;
    // 传给规划器的请求字符数之和，是否含重复项取决于上游是否已去重。
    uint64_t requested_residues = 0;
    // 诊断开启时统计所需 packed 区间并集的字节数，去掉重叠和额外间隙。
    uint64_t unique_packed_bytes = 0;
    // 所有计划范围长度之和，含为减少请求数而额外读取的间隙字节。
    uint64_t read_bytes = 0;
    // 诊断开启时统计合并跨过非零物理间隙的次数。
    uint64_t merged_gaps = 0;
    // 诊断开启时统计重叠或首尾相接的合并次数，不引入额外读取字节。
    uint64_t free_merges = 0;
    // 最大单个读取范围的字节数，用于估算槽位缓冲预算。
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
