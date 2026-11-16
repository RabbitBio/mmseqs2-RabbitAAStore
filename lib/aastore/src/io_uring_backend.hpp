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

using RangeCompletion = void (*)(
    void* context, size_t decode_worker_index, size_t range_index,
    const uint8_t* data, uint64_t size);
using CooperativeWork = bool (*)(void* context, size_t worker_index);

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
