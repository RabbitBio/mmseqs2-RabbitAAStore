#ifndef AASTORE_MEMORY_BUDGET_H
#define AASTORE_MEMORY_BUDGET_H

#include <algorithm>
#include <cstdint>
#include <limits>

namespace aastore_memory {

struct Budget {
    uint64_t memoryLimit = 0;
    uint64_t memoryAvailable = 0;
    uint64_t memoryReserve = 0;
    uint64_t indexReserve = 0;
    uint64_t bufferReserve = 0;
    uint64_t safetyReserve = 0;
    uint64_t stageReserve = 0;
    uint64_t metadataReserve = 0;
    uint64_t textReserve = 0;
    uint64_t remainingPool = 0;
    uint64_t cacheBytes = 0;
    uint64_t pageCacheReserve = 0;
    bool cgroupLimited = false;
    bool residentText = false;
};

inline uint64_t add_bytes(uint64_t left, uint64_t right) {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

// Separate the budget calculation from host probes so large-database and
// constrained-memory cases can be checked without allocating the cache.
inline Budget plan(uint64_t memoryLimit, uint64_t memoryAvailable,
                   uint64_t indexBytes, uint64_t bufferBytes,
                   uint64_t stageBytes, uint64_t safetyBytes = 0) {
    constexpr uint64_t kGiB = UINT64_C(1024) * 1024 * 1024;
    constexpr uint64_t kCacheAlignment = UINT64_C(2) * 1024 * 1024;
    Budget budget;
    budget.memoryLimit = memoryLimit;
    budget.memoryAvailable = std::min(memoryAvailable, memoryLimit);
    budget.indexReserve = indexBytes;
    budget.bufferReserve = bufferBytes;
    budget.stageReserve = stageBytes;
    budget.safetyReserve = safetyBytes != 0 ? std::max<uint64_t>(4 * kGiB, safetyBytes)
        : std::max<uint64_t>(4 * kGiB, memoryLimit / 5);
    budget.memoryReserve = add_bytes(
        add_bytes(add_bytes(budget.safetyReserve, indexBytes), bufferBytes),
        stageBytes);
    budget.remainingPool = budget.memoryAvailable > budget.memoryReserve
        ? budget.memoryAvailable - budget.memoryReserve : 0;
    // Give AAStore 90% of the usable pool and retain the rest for transient
    // allocations and kernel file pages. Aligning the prefix keeps mapped and
    // packed-file boundaries stable across repeated budget probes.
    const uint64_t cacheCandidate =
        (budget.remainingPool / 10) * 9 +
        ((budget.remainingPool % 10) * 9) / 10;
    budget.cacheBytes = cacheCandidate - cacheCandidate % kCacheAlignment;
    budget.pageCacheReserve = budget.remainingPool - budget.cacheBytes;
    return budget;
}

// Record exports replace native header mappings with two optional resident
// text columns. Their metadata remains necessary even when the text is read
// in batches. Reserve both before assigning the remaining pool to packed data.
inline Budget plan_records(uint64_t memoryLimit, uint64_t memoryAvailable,
                           uint64_t indexBytes, uint64_t bufferBytes,
                           uint64_t stageBytes, uint64_t textBytes,
                           uint64_t metadataBytes, uint64_t safetyBytes = 0) {
    const uint64_t fixedStage = add_bytes(stageBytes, metadataBytes);
    const Budget initial = plan(memoryLimit, memoryAvailable, indexBytes,
                                bufferBytes, fixedStage, safetyBytes);
    const bool residentText = textBytes <= initial.remainingPool;
    Budget budget = plan(memoryLimit, memoryAvailable, indexBytes, bufferBytes,
                         add_bytes(fixedStage, residentText ? textBytes : 0), safetyBytes);
    budget.stageReserve = stageBytes;
    budget.metadataReserve = metadataBytes;
    budget.textReserve = residentText ? textBytes : 0;
    budget.residentText = residentText;
    return budget;
}

}  // namespace aastore_memory

#endif
