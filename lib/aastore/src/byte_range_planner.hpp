#ifndef AASTORE_BYTE_RANGE_PLANNER_HPP
#define AASTORE_BYTE_RANGE_PLANNER_HPP

#include "aastore/store.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aastore {

struct SequenceRequest {
    uint64_t sequence_id;
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
    uint64_t merged_gaps = 0;
    uint64_t free_merges = 0;
    uint64_t max_range_bytes = 0;
};

void sort_requests_by_sequence_id(SequenceRequests& requests,
                                  size_t thread_count,
                                  uint64_t sequence_count);

ReadPlan plan_byte_ranges(const SequenceRequests& sorted_requests,
                          const ReadOptions& options);

}  // namespace aastore

#endif
