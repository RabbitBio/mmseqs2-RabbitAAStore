// 文件职责：比较交错步长请求的直接批读与连续窗口顺读成本。
// 窗口路径读取覆盖请求的连续 ID 区间，汇总耗时、范围、读取字节和校验值。
#include "aastore/store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

// 解析要求非零的 uint64_t 命令行参数。
uint64_t parse_u64(const char* value) {
    size_t consumed = 0;
    const std::string text(value);
    const uint64_t result = std::stoull(text, &consumed);
    if (consumed != text.size() || result == 0) {
        throw std::invalid_argument("invalid positive integer");
    }
    return result;
}

// 为线程生成交错步长的请求 ID，超出数据库大小时循环取模。
std::vector<aastore::SequenceId> make_ids(uint64_t size, size_t thread,
                                          size_t threads, size_t count) {
    std::vector<aastore::SequenceId> ids;
    ids.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        ids.push_back((static_cast<uint64_t>(index) * threads + thread) % size);
    }
    return ids;
}

struct Totals {
    uint64_t ranges = 0;
    uint64_t read_bytes = 0;
    uint64_t unique_bytes = 0;
};

// 把单次读取的范围数量和字节统计累加到总计。
void add(Totals& total, const aastore::BatchStats& stats) {
    total.ranges += stats.ranges;
    total.read_bytes += stats.read_bytes;
    total.unique_bytes += stats.unique_packed_bytes;
}

struct RunContext {
    aastore::Store* store = nullptr;
    const std::vector<std::vector<aastore::SequenceId>>* requests = nullptr;
    const aastore::ReadOptions* options = nullptr;
    size_t window = 0;
    bool use_windows = false;
    std::vector<uint64_t>* checksums = nullptr;
    std::vector<Totals>* totals = nullptr;
};

// 按普通批量或窗口方式读取线程请求，并记录校验值和读取统计。
void run_worker(RunContext* ctx, size_t thread) {
    uint64_t checksum = 0;
    if (!ctx->use_windows) {
        const aastore::Batch batch = ctx->store->get(
            (*ctx->requests)[thread], *ctx->options);
        add((*ctx->totals)[thread], batch.stats);
        for (size_t index = 0; index < batch.size(); ++index) {
            for (const unsigned char value : batch.view(index)) {
                checksum += value;
            }
        }
    } else {
        aastore::ReadOptions options = *ctx->options;
        options.threads = 1;
        size_t position = 0;
        while (position < (*ctx->requests)[thread].size()) {
            const uint64_t begin = (*ctx->requests)[thread][position];
            const uint64_t end = std::min<uint64_t>(
                ctx->store->size(), begin + ctx->window);
            size_t next = position + 1;
            while (next < (*ctx->requests)[thread].size() &&
                   (*ctx->requests)[thread][next] < end) {
                ++next;
            }
            const aastore::Batch block = ctx->store->sequential_get(
                begin, end, options);
            add((*ctx->totals)[thread], block.stats);
            for (size_t index = position; index < next; ++index) {
                const size_t local = static_cast<size_t>(
                    (*ctx->requests)[thread][index] - begin);
                for (const unsigned char value : block.view(local)) {
                    checksum += value;
                }
            }
            position = next;
        }
    }
    (*ctx->checksums)[thread] = checksum;
}

// 启动各请求线程执行指定方案，汇总耗时、校验值与读取统计。
void run_benchmark(aastore::Store* store,
                   const std::vector<std::vector<aastore::SequenceId>>* requests,
                   const aastore::ReadOptions* options,
                   size_t threads,
                   size_t window,
                   bool use_windows,
                   Totals* totals) {
    std::vector<uint64_t> checksums(threads, 0);
    std::vector<Totals> worker_totals(threads);
    RunContext ctx{
        store, requests, options, window, use_windows,
        &checksums, &worker_totals};
    std::vector<std::thread> workers;
    workers.reserve(threads);
    const auto start = Clock::now();
    for (size_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back(run_worker, &ctx, thread);
    }
    for (std::thread& worker : workers) worker.join();
    uint64_t checksum = 0;
    for (const uint64_t value : checksums) checksum += value;
    for (const Totals& worker_total : worker_totals) {
        totals->ranges += worker_total.ranges;
        totals->read_bytes += worker_total.read_bytes;
        totals->unique_bytes += worker_total.unique_bytes;
    }
    const char* label = use_windows ? "window" : "batch";
    std::cout << label << "_ms="
              << std::chrono::duration<double, std::milli>(
                     Clock::now() - start).count()
              << '\n'
              << label << "_ranges=" << totals->ranges << '\n'
              << label << "_read_bytes=" << totals->read_bytes << '\n'
              << label << "_unique_bytes=" << totals->unique_bytes << '\n'
              << label << "_checksum=" << checksum << '\n';
}

}  // namespace

// 解析窗口基准参数，对比普通批量读取和窗口读取。
int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::invalid_argument(
                "usage: aastore_window_bench STORE THREADS BATCH_SIZE "
                "WINDOW_SEQUENCES");
        }
        const std::string store_path(argv[1]);
        const size_t threads = static_cast<size_t>(parse_u64(argv[2]));
        const size_t batch_size = static_cast<size_t>(parse_u64(argv[3]));
        const size_t window = static_cast<size_t>(parse_u64(argv[4]));
        aastore::Store store(store_path);

        std::vector<std::vector<aastore::SequenceId>> requests(threads);
        for (size_t thread = 0; thread < threads; ++thread) {
            requests[thread] = make_ids(
                store.size(), thread, threads, batch_size);
        }
        aastore::ReadOptions batch_options;
        batch_options.collect_diagnostics = true;
        batch_options.threads = 1;
        batch_options.io_queue_depth = 64;

        Totals batch_totals;
        run_benchmark(
            &store, &requests, &batch_options, threads, window, false,
            &batch_totals);
        Totals window_totals;
        run_benchmark(
            &store, &requests, &batch_options, threads, window, true,
            &window_totals);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
