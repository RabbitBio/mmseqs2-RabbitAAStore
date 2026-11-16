#ifndef AASTORE_STORE_IO_HPP
#define AASTORE_STORE_IO_HPP

#include "byte_range_planner.hpp"
#include "thread_helpers.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <thread>
#include <vector>

namespace aastore {

using Clock = std::chrono::steady_clock;

class Metadata;

struct StoreReadContext {
    const Metadata* metadata = nullptr;
    StorageMode storage_mode = StorageMode::NoMemory;
    int packed_descriptor = -1;
    const uint8_t* full_packed = nullptr;
    uint64_t full_packed_bytes = 0;
    const uint8_t* prefix_packed = nullptr;
    uint64_t prefix_packed_bytes = 0;
};

uint64_t elapsed_ns(Clock::time_point start);

void pread_exact(int descriptor, void* output, uint64_t bytes,
                 uint64_t offset);

void pread_exact_parallel(int descriptor, void* output, uint64_t bytes,
                          uint64_t offset, size_t thread_count,
                          bool discard_source_cache = false);

uint64_t file_size(int descriptor);

void decode_request_from_extent(const SequenceRequest& request,
                                const uint8_t* packed,
                                uint64_t packed_begin,
                                uint64_t packed_end,
                                char* output,
                                bool terminate_output = true);

template <typename Context>
void run_indexed_workers(size_t worker_count,
                         void (*worker_function)(Context*, size_t),
                         Context* context) {
    if (worker_count == 0) return;
    std::vector<std::thread> threads;
    ThreadJoinGuard join_guard(threads);
    threads.reserve(worker_count - 1);
    for (size_t worker = 1; worker < worker_count; ++worker) {
        threads.emplace_back(worker_function, context, worker);
    }
    worker_function(context, 0);
    join_guard.join_all();
}

template <typename Worker>
void run_indexed_workers(size_t worker_count, Worker&& worker_function) {
    if (worker_count == 0) return;
    std::vector<std::thread> threads;
    ThreadJoinGuard join_guard(threads);
    threads.reserve(worker_count - 1);
    for (size_t worker = 1; worker < worker_count; ++worker) {
        threads.emplace_back([&worker_function, worker]() {
            worker_function(worker);
        });
    }
    worker_function(0);
    join_guard.join_all();
}

inline void rethrow_worker_errors(
        const std::vector<std::exception_ptr>& errors) {
    for (const std::exception_ptr& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

void fill_batch_from_store(const StoreReadContext& store,
                           const std::vector<SequenceId>& ids,
                           const ReadOptions& options,
                           Batch& batch,
                           char* const* outputs = nullptr);

void sequential_get_from_store(const StoreReadContext& store,
                               SequenceId first_sequence,
                               SequenceId end_sequence,
                               const ReadOptions& options,
                               Batch& batch,
                               char* const* outputs = nullptr);

}  // namespace aastore

#endif
