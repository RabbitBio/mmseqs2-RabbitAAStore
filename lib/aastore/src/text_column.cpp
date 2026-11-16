#include "text_column.hpp"
#include "index_data.hpp"

#include "byte_range_planner.hpp"
#include "io_uring_backend.hpp"
#include "thread_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace aastore {
namespace {

constexpr uint64_t kTextHeaderBytes = 8 + 6 * sizeof(uint64_t);

template <typename T>
bool read_value(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}

void pread_exact(int descriptor, void* output, uint64_t bytes,
                 uint64_t offset) {
    char* destination = static_cast<char*>(output);
    uint64_t completed = 0;
    while (completed < bytes) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(
            bytes - completed,
            static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())));
        const ssize_t result = ::pread(
            descriptor, destination + completed, chunk,
            static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(
                "pread text column failed: " +
                std::string(std::strerror(errno)));
        }
        if (result == 0) throw std::runtime_error("short text-column read");
        completed += static_cast<uint64_t>(result);
    }
}

struct TextRequest {
    SequenceId id = 0;
    size_t original_index = 0;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t output_offset = 0;
};

struct TextPlan {
    std::vector<ByteRange> ranges;
    uint64_t unique_bytes = 0;
    uint64_t read_bytes = 0;
    uint64_t merged_gaps = 0;
    uint64_t max_range_bytes = 0;
};

TextPlan plan_ranges(const std::vector<TextRequest>& requests,
                     const ReadOptions& options) {
    TextPlan plan;
    uint64_t covered_end = 0;
    bool covered = false;
    for (size_t i = 0; i < requests.size(); ++i) {
        const TextRequest& request = requests[i];
        if (request.begin == request.end) continue;
        if (options.collect_diagnostics &&
            (!covered || request.end > covered_end)) {
            plan.unique_bytes += request.end -
                (covered ? std::max(request.begin, covered_end) : request.begin);
            covered_end = request.end;
            covered = true;
        }

        const bool overlaps = !plan.ranges.empty() &&
            request.begin <= plan.ranges.back().end;
        const uint64_t gap = plan.ranges.empty() || overlaps
            ? 0
            : request.begin - plan.ranges.back().end;
        const uint64_t merged_end = plan.ranges.empty()
            ? request.end
            : std::max(plan.ranges.back().end, request.end);
        const bool contained = !plan.ranges.empty() &&
            merged_end == plan.ranges.back().end;
        const bool within_cap = plan.ranges.empty() || contained ||
            options.max_merged_range_bytes == 0 ||
            merged_end - plan.ranges.back().begin <=
                options.max_merged_range_bytes;
        if (!plan.ranges.empty() && within_cap &&
            (overlaps || gap <= options.merge_gap_bytes)) {
            ByteRange& range = plan.ranges.back();
            if (options.collect_diagnostics && !overlaps) ++plan.merged_gaps;
            range.end = merged_end;
            range.request_end = i + 1;
        } else {
            plan.ranges.push_back(
                ByteRange{request.begin, request.end, i, i + 1});
        }
    }
    for (const ByteRange& range : plan.ranges) {
        const uint64_t bytes = range.end - range.begin;
        plan.read_bytes += bytes;
        plan.max_range_bytes = std::max(plan.max_range_bytes, bytes);
    }
    return plan;
}

bool text_request_less(const TextRequest& left,
                       const TextRequest& right) {
    return left.id < right.id;
}

struct TextCopyContext {
    const TextPlan* plan = nullptr;
    const std::vector<TextRequest>* requests = nullptr;
    TextBatch* batch = nullptr;
};

void copy_text_range(void* opaque, size_t, size_t range_index,
                     const uint8_t* data, uint64_t bytes) {
    TextCopyContext* ctx = static_cast<TextCopyContext*>(opaque);
    const ByteRange& range = ctx->plan->ranges[range_index];
    if (bytes != range.end - range.begin) {
        throw std::runtime_error("text range size mismatch");
    }
    for (size_t i = range.request_begin; i < range.request_end; ++i) {
        const TextRequest& request = (*ctx->requests)[i];
        if (request.begin == request.end) continue;
        std::memcpy(
            ctx->batch->storage.data() + request.output_offset,
            data + static_cast<size_t>(request.begin - range.begin),
            static_cast<size_t>(request.end - request.begin));
    }
}

struct TextReadContext {
    int descriptor = -1;
    const TextPlan* plan = nullptr;
    TextCopyContext* copy = nullptr;
    std::atomic<size_t>* next_range = nullptr;
    std::vector<std::exception_ptr>* errors = nullptr;
};

void read_text_ranges(TextReadContext* ctx, size_t worker) noexcept {
    try {
        std::vector<uint8_t, NoInitAllocator<uint8_t>> buffer;
        while (true) {
            const size_t range_index = ctx->next_range->fetch_add(
                1, std::memory_order_relaxed);
            if (range_index >= ctx->plan->ranges.size()) break;
            const ByteRange& range = ctx->plan->ranges[range_index];
            const size_t bytes = static_cast<size_t>(
                range.end - range.begin);
            buffer.resize(bytes);
            pread_exact(ctx->descriptor, buffer.data(), bytes, range.begin);
            copy_text_range(ctx->copy, worker, range_index,
                            buffer.data(), bytes);
        }
    } catch (...) {
        (*ctx->errors)[worker] = std::current_exception();
    }
}

}  // namespace

struct TextColumn::Impl {
    Impl(const std::filesystem::path& data_path,
         const std::filesystem::path& metadata_path,
         uint64_t expected_records,
         bool resident,
         IndexLoadMode index_load_mode)
        : resident_(resident) {
        std::ifstream input(metadata_path, std::ios::binary);
        char magic[8] = {};
        uint64_t version = 0;
        uint64_t block_count = 0;
        uint64_t offset_count = 0;
        input.read(magic, sizeof(magic));
        if (!input || std::strncmp(magic, "AATEXT1", 7) != 0 ||
            !read_value(input, version) || version != 1 ||
            !read_value(input, record_count_) ||
            !read_value(input, total_bytes_) ||
            !read_value(input, block_shift_) ||
            !read_value(input, block_count) ||
            !read_value(input, offset_count) ||
            record_count_ != expected_records || block_shift_ == 0 ||
            block_shift_ > 31 || offset_count != record_count_ + 1) {
            throw std::runtime_error(
                "invalid text-column metadata: " + metadata_path.string());
        }
        const uint64_t expected_blocks = record_count_ == 0
            ? 0
            : (record_count_ + (uint64_t{1} << block_shift_) - 1) >>
                  block_shift_;
        const uint64_t offsets_bytes = offset_count * sizeof(uint32_t);
        const uint64_t bases_bytes = (block_count + 1) * sizeof(uint64_t);
        const uint64_t index_bytes = kTextHeaderBytes + offsets_bytes + bases_bytes;
        if (block_count != expected_blocks ||
            std::filesystem::file_size(metadata_path) != index_bytes ||
            index_bytes > static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
            throw std::runtime_error(
                "inconsistent text-column metadata: " + metadata_path.string());
        }

        index_data_.open(metadata_path, static_cast<size_t>(index_bytes), index_load_mode);
        const char* base = index_data_.data();
        offsets_ = reinterpret_cast<const uint32_t*>(
            base + kTextHeaderBytes);
        bases_ = base + kTextHeaderBytes + offsets_bytes;
        if (offset(0) != 0 || offset(record_count_) != total_bytes_) {
            throw std::runtime_error("invalid text-column offset endpoints");
        }

        descriptor_ = ::open(data_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor_ < 0) {
            throw std::runtime_error("cannot open " + data_path.string());
        }
        const off_t bytes = ::lseek(descriptor_, 0, SEEK_END);
        if (bytes < 0 || static_cast<uint64_t>(bytes) != total_bytes_) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("text-column data size mismatch");
        }
        if (resident_) {
            try {
                if (total_bytes_ > static_cast<uint64_t>(
                        std::numeric_limits<size_t>::max())) {
                    throw std::length_error(
                        "resident text column exceeds address space");
                }
                resident_data_.resize(static_cast<size_t>(total_bytes_));
                pread_exact(descriptor_, resident_data_.data(), total_bytes_, 0);
            } catch (...) {
                ::close(descriptor_);
                descriptor_ = -1;
                throw;
            }
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

    ~Impl() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    uint64_t resident_bytes() const noexcept {
        return resident_data_.size();
    }

    uint64_t offset(uint64_t index) const {
        if (index > record_count_) {
            throw std::out_of_range("text-column id out of range");
        }
        uint64_t block_base = 0;
        std::memcpy(&block_base,
                    bases_ + (index >> block_shift_) * sizeof(uint64_t),
                    sizeof(block_base));
        if (offsets_[index] > total_bytes_ ||
            block_base > total_bytes_ - offsets_[index]) {
            throw std::runtime_error("text-column boundary exceeds data size");
        }
        return block_base + offsets_[index];
    }

    uint64_t length(SequenceId id) const {
        if (id >= record_count_) {
            throw std::out_of_range("text-column id out of range");
        }
        const uint64_t begin = offset(id), end = offset(id + 1);
        if (end < begin) {
            throw std::runtime_error("text-column boundaries are not monotonic");
        }
        return end - begin;
    }

    TextBatch batch_get(const std::vector<SequenceId>& ids,
                        const ReadOptions& options) const {
        TextBatch batch;
        batch.offsets.resize(ids.size());
        batch.lengths.resize(ids.size());
        batch.stats.requested_strings = ids.size();
        if (ids.size() == 1 &&
            (resident_ || options.io_backend == IoBackend::Pread)) {
            if (ids[0] >= record_count_) {
                throw std::out_of_range("text-column id out of range");
            }
            const uint64_t begin = offset(ids[0]), end = offset(ids[0] + 1);
            if (end < begin || end > total_bytes_) {
                throw std::runtime_error("invalid text-column boundaries");
            }
            const uint64_t bytes = end - begin;
            if (bytes > static_cast<uint64_t>(
                    std::numeric_limits<size_t>::max())) {
                throw std::length_error("text batch exceeds address space");
            }
            batch.offsets[0] = 0;
            batch.lengths[0] = bytes;
            batch.storage.resize_uninitialized(static_cast<size_t>(bytes));
            batch.stats.requested_bytes = bytes;
            batch.stats.resident = resident_;
            if (options.collect_diagnostics) batch.stats.unique_bytes = bytes;
            if (bytes == 0) return batch;
            if (resident_) {
                std::memcpy(batch.storage.data(), resident_data_.data() + begin,
                            static_cast<size_t>(bytes));
            } else {
                pread_exact(descriptor_, batch.storage.data(), bytes, begin);
                batch.stats.ranges = 1;
                batch.stats.read_bytes = bytes;
                batch.stats.max_range_bytes = bytes;
            }
            return batch;
        }
        std::vector<TextRequest> requests(ids.size());
        uint64_t output_bytes = 0;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] >= record_count_) {
                throw std::out_of_range("text-column id out of range");
            }
            const uint64_t begin = offset(ids[i]);
            const uint64_t end = offset(ids[i] + 1);
            const uint64_t length = end - begin;
            batch.offsets[i] = output_bytes;
            batch.lengths[i] = length;
            requests[i] = TextRequest{ids[i], i, begin, end, output_bytes};
            output_bytes += length;
        }
        batch.stats.requested_bytes = output_bytes;
        if (output_bytes > static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
            throw std::length_error("text batch exceeds address space");
        }
        batch.storage.resize_uninitialized(static_cast<size_t>(output_bytes));
        batch.stats.resident = resident_;
        if (resident_) {
            const char* data = resident_data_.data();
            for (const TextRequest& request : requests) {
                if (request.begin == request.end) continue;
                std::memcpy(
                    batch.storage.data() + request.output_offset,
                    data + request.begin,
                    static_cast<size_t>(request.end - request.begin));
            }
            if (options.collect_diagnostics) batch.stats.unique_bytes = output_bytes;
            return batch;
        }
        if (!options.input_sorted) {
            std::stable_sort(
                requests.begin(), requests.end(), text_request_less);
        }
        const TextPlan plan = plan_ranges(requests, options);
        batch.stats.ranges = plan.ranges.size();
        batch.stats.read_bytes = plan.read_bytes;
        batch.stats.max_range_bytes = plan.max_range_bytes;
        if (options.collect_diagnostics) {
            batch.stats.unique_bytes = plan.unique_bytes;
            batch.stats.merged_gaps = plan.merged_gaps;
            batch.stats.overread_bytes = plan.read_bytes - plan.unique_bytes;
        }
        if (plan.ranges.empty()) return batch;

        TextCopyContext copy{&plan, &requests, &batch};

        if (options.io_backend != IoBackend::Pread) {
            IoUringStats stats;
            std::string unavailable;
            size_t queue_depth = std::max<size_t>(1, options.io_queue_depth);
            const size_t ring_workers = std::min(
                std::max<size_t>(1, options.threads),
                plan.ranges.size());
            if (options.io_buffer_bytes != 0 && plan.max_range_bytes != 0) {
                queue_depth = std::min<uint64_t>(
                    queue_depth,
                    std::max<uint64_t>(
                        1, options.io_buffer_bytes /
                               plan.max_range_bytes / ring_workers));
            }
            const bool used = read_ranges_io_uring(
                descriptor_, plan.ranges, ring_workers,
                queue_depth, nullptr, nullptr,
                copy_text_range, &copy, stats, unavailable);
            if (used) {
                batch.stats.io_uring = true;
                return batch;
            }
            if (options.io_backend == IoBackend::IoUring) {
                throw std::runtime_error(
                    "io_uring unavailable for text column: " + unavailable);
            }
        }

        const size_t workers = std::min(
            std::max<size_t>(1, options.threads), plan.ranges.size());
        std::atomic<size_t> next_range{0};
        std::vector<std::exception_ptr> errors(workers);
        TextReadContext read{
            descriptor_, &plan, &copy, &next_range, &errors};
        std::vector<std::thread> threads;
        ThreadJoinGuard join_guard(threads);
        for (size_t worker = 1; worker < workers; ++worker) {
            threads.emplace_back(read_text_ranges, &read, worker);
        }
        read_text_ranges(&read, 0);
        join_guard.join_all();
        for (const std::exception_ptr& error : errors) {
            if (error) std::rethrow_exception(error);
        }
        return batch;
    }

    uint64_t record_count_ = 0;
    uint64_t total_bytes_ = 0;
    uint64_t block_shift_ = 0;
    int descriptor_ = -1;
    bool resident_ = false;
    IndexData index_data_;
    std::vector<char, NoInitAllocator<char>> resident_data_;
    const uint32_t* offsets_ = nullptr;
    const char* bases_ = nullptr;
};

TextColumn::TextColumn(const std::filesystem::path& data_path,
                       const std::filesystem::path& metadata_path,
                       uint64_t expected_records,
                       bool resident,
                       IndexLoadMode index_load_mode)
    : impl_(std::make_unique<Impl>(
          data_path, metadata_path, expected_records, resident, index_load_mode)) {}

TextColumn::~TextColumn() = default;
TextBatch TextColumn::batch_get(const std::vector<SequenceId>& ids,
                                const ReadOptions& options) const {
    return impl_->batch_get(ids, options);
}

uint64_t TextColumn::length(SequenceId id) const {
    return impl_->length(id);
}

uint64_t TextColumn::resident_bytes() const noexcept {
    return impl_->resident_bytes();
}

}  // namespace aastore
