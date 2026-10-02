// 文件职责：声明按物理字节范围执行的 io_uring 读取接口、回调和统计。
// 完成回调由 ring 所属线程执行，可选协作回调用于在等待 I/O 时处理前缀数据。
#ifndef AASTORE_IO_URING_BACKEND_HPP
#define AASTORE_IO_URING_BACKEND_HPP

#include "byte_range_planner.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace aastore {

struct IoUringStats {
    uint64_t submissions = 0;
    uint64_t completions = 0;
    uint64_t wait_ns = 0;
    uint64_t max_outstanding = 0;
    uint64_t submit_calls = 0;
    uint64_t submit_ns = 0;
    uint64_t wait_calls = 0;
    uint64_t empty_waits = 0;
};

// 处理一个已完整读取的范围；数据视图仅在本次回调返回前有效。
using RangeCompletion = void (*)(
    void* context, size_t decode_worker_index, size_t range_index,
    const uint8_t* data, uint64_t size);
// 在 ring 所属线程首次提交后执行有限工作片，与 CQ 处理交错进行。
// 当前线程没有更多可领取任务时返回 false，后续不再调用该线程的协作回调。
using CooperativeWork = bool (*)(void* context, size_t worker_index);

// 后端初始化不可用时返回 false；初始化后的 I/O 或回调错误抛出异常。
// 范围列表和回调上下文在整个调用期间必须有效。
// 默认只保留请求数和在途峰值；启用诊断时额外收集 submit/wait 调用与耗时。
bool read_ranges_io_uring(int descriptor,
                          const std::vector<ByteRange>& ranges,
                          size_t worker_count,
                          size_t queue_depth,
                          CooperativeWork work,
                          void* work_context,
                          RangeCompletion complete,
                          void* complete_context,
                          IoUringStats& stats,
                          std::string& unavailable_reason,
                          bool collect_diagnostics = false);

}  // namespace aastore

#endif
