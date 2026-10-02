// 文件职责：读取名称或描述列，校验文本索引并按请求合并范围、复制结果。
// 支持常驻映射以及 pread、io_uring 读取，文本结果无需经过序列编解码器。
#include "text_column.hpp"

#include "byte_range_planner.hpp"
#include "io_uring_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace aastore {
namespace {

constexpr uint64_t kTextHeaderBytes = 8 + 6 * sizeof(uint64_t);

// 从输入流读取一个定长文本元数据字段，返回是否成功。
template <typename T>
bool read_value(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}

// 按文件偏移完整读取文本字节，重试信号中断并检查短读。
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

// 为按 ID 排序的文本请求合并读取范围，并统计有效字节与额外读取。
TextPlan plan_ranges(const std::vector<TextRequest>& requests,
                     const ReadOptions& options) {
    TextPlan plan;
    uint64_t covered_end = 0;
    bool covered = false;
    for (size_t i = 0; i < requests.size(); ++i) {
        const TextRequest& request = requests[i];
        if (request.begin == request.end) continue;
        if (!covered || request.end > covered_end) {
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
            if (!overlaps) ++plan.merged_gaps;
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

// 按记录 ID 比较文本请求，供稳定排序使用。
bool text_request_less(const TextRequest& left,
                       const TextRequest& right) {
    return left.id < right.id;
}

struct TextCopyContext {
    const TextPlan* plan = nullptr;
    const std::vector<TextRequest>* requests = nullptr;
    TextBatch* batch = nullptr;
};

// 校验完成范围长度，并把其中各条文本复制到最终输出位置。
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

// 动态领取文本范围，同步读取并复制结果，异常保存到线程槽位。
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
    // 校验文本元数据和数据文件，并按配置建立预触页的只读数据映射。
    Impl(const std::filesystem::path& data_path,
         const std::filesystem::path& metadata_path,
         uint64_t expected_records,
         bool resident)
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
        mapping_bytes_ = kTextHeaderBytes + offsets_bytes + bases_bytes;
        if (block_count != expected_blocks ||
            std::filesystem::file_size(metadata_path) != mapping_bytes_ ||
            mapping_bytes_ > static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
            throw std::runtime_error(
                "inconsistent text-column metadata: " + metadata_path.string());
        }

        const int metadata_descriptor = ::open(
            metadata_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (metadata_descriptor < 0) {
            throw std::runtime_error("cannot open " + metadata_path.string());
        }
        mapping_ = ::mmap(nullptr, static_cast<size_t>(mapping_bytes_),
                          PROT_READ, MAP_PRIVATE, metadata_descriptor, 0);
        const int saved_errno = errno;
        ::close(metadata_descriptor);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            throw std::runtime_error(
                "cannot mmap text metadata: " +
                std::string(std::strerror(saved_errno)));
        }
        const char* base = static_cast<const char*>(mapping_);
        offsets_ = reinterpret_cast<const uint32_t*>(
            base + kTextHeaderBytes);
        bases_ = base + kTextHeaderBytes + offsets_bytes;
        if (offset(0) != 0 || offset(record_count_) != total_bytes_) {
            ::munmap(mapping_, static_cast<size_t>(mapping_bytes_));
            mapping_ = nullptr;
            throw std::runtime_error("invalid text-column offset endpoints");
        }

        descriptor_ = ::open(data_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor_ < 0) {
            ::munmap(mapping_, static_cast<size_t>(mapping_bytes_));
            mapping_ = nullptr;
            throw std::runtime_error("cannot open " + data_path.string());
        }
        const off_t bytes = ::lseek(descriptor_, 0, SEEK_END);
        if (bytes < 0 || static_cast<uint64_t>(bytes) != total_bytes_) {
            ::close(descriptor_);
            descriptor_ = -1;
            ::munmap(mapping_, static_cast<size_t>(mapping_bytes_));
            mapping_ = nullptr;
            throw std::runtime_error("text-column data size mismatch");
        }
        if (resident_) {
            if (total_bytes_ > static_cast<uint64_t>(
                    std::numeric_limits<size_t>::max())) {
                throw std::length_error(
                    "resident text column exceeds address space");
            }
            if (total_bytes_ != 0) {
                data_mapping_ = ::mmap(
                    nullptr, static_cast<size_t>(total_bytes_), PROT_READ,
                    MAP_PRIVATE, descriptor_, 0);
                const int saved_errno = errno;
                if (data_mapping_ == MAP_FAILED) {
                    data_mapping_ = nullptr;
                    ::close(descriptor_);
                    descriptor_ = -1;
                    ::munmap(mapping_, static_cast<size_t>(mapping_bytes_));
                    mapping_ = nullptr;
                    throw std::runtime_error(
                        "cannot mmap resident text column: " +
                        std::string(std::strerror(saved_errno)));
                }
                // 逐页触碰数据映射，使首批读取前先完成缺页加载；
                // 这不锁定页面，内核仍可在内存压力下回收它们。
                const long page_size = ::sysconf(_SC_PAGESIZE);
                const size_t stride = page_size > 0
                    ? static_cast<size_t>(page_size)
                    : size_t{4096};
                volatile unsigned char touch = 0;
                const char* data = static_cast<const char*>(data_mapping_);
                for (size_t offset = 0;
                     offset < static_cast<size_t>(total_bytes_);
                     offset += stride) {
                    touch ^= static_cast<unsigned char>(data[offset]);
                }
                touch ^= static_cast<unsigned char>(
                    data[static_cast<size_t>(total_bytes_) - 1]);
                (void)touch;
            }
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

    // 关闭文本文件并解除数据和元数据映射。
    ~Impl() {
        if (descriptor_ >= 0) ::close(descriptor_);
        if (data_mapping_ != nullptr) {
            ::munmap(data_mapping_, static_cast<size_t>(total_bytes_));
        }
        if (mapping_ != nullptr) {
            ::munmap(mapping_, static_cast<size_t>(mapping_bytes_));
        }
    }

    // 返回内存映射模式的数据字节数，不含元数据，也不代表实时 RSS。
    uint64_t resident_bytes() const noexcept {
        return resident_ ? total_bytes_ : 0;
    }

    // 由块基址和相对偏移还原文本边界，允许查询末尾哨兵。
    uint64_t offset(uint64_t index) const {
        if (index > record_count_) {
            throw std::out_of_range("text-column id out of range");
        }
        uint64_t block_base = 0;
        std::memcpy(&block_base,
                    bases_ + (index >> block_shift_) * sizeof(uint64_t),
                    sizeof(block_base));
        return block_base + offsets_[index];
    }

    // 按请求顺序建立输出布局，从映射或合并读取范围填充文本批次。
    TextBatch batch_get(const std::vector<SequenceId>& ids,
                        const ReadOptions& options) const {
        TextBatch batch;
        batch.offsets.resize(ids.size());
        batch.lengths.resize(ids.size());
        batch.stats.requested_strings = ids.size();
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
            const char* data = static_cast<const char*>(data_mapping_);
            for (const TextRequest& request : requests) {
                if (request.begin == request.end) continue;
                std::memcpy(
                    batch.storage.data() + request.output_offset,
                    data + request.begin,
                    static_cast<size_t>(request.end - request.begin));
            }
            batch.stats.unique_bytes = output_bytes;
            return batch;
        }
        if (!options.input_sorted) {
            std::stable_sort(
                requests.begin(), requests.end(), text_request_less);
        }
        const TextPlan plan = plan_ranges(requests, options);
        batch.stats.unique_bytes = plan.unique_bytes;
        batch.stats.ranges = plan.ranges.size();
        batch.stats.merged_gaps = plan.merged_gaps;
        batch.stats.read_bytes = plan.read_bytes;
        batch.stats.overread_bytes = plan.read_bytes - plan.unique_bytes;
        batch.stats.max_range_bytes = plan.max_range_bytes;
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
        for (size_t worker = 1; worker < workers; ++worker) {
            threads.emplace_back(read_text_ranges, &read, worker);
        }
        read_text_ranges(&read, 0);
        for (std::thread& thread : threads) thread.join();
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
    void* mapping_ = nullptr;
    uint64_t mapping_bytes_ = 0;
    void* data_mapping_ = nullptr;
    const uint32_t* offsets_ = nullptr;
    const char* bases_ = nullptr;
};

// 打开文本列并校验记录数与序列数据库一致。
TextColumn::TextColumn(const std::filesystem::path& data_path,
                       const std::filesystem::path& metadata_path,
                       uint64_t expected_records,
                       bool resident)
    : impl_(std::make_unique<Impl>(
          data_path, metadata_path, expected_records, resident)) {}

// 释放文本列文件和映射资源。
TextColumn::~TextColumn() = default;
// 批量读取指定记录的文本，返回独立拥有数据的批次。
TextBatch TextColumn::batch_get(const std::vector<SequenceId>& ids,
                                const ReadOptions& options) const {
    return impl_->batch_get(ids, options);
}

// 返回配置为常驻模式的文本数据大小，不含元数据。
uint64_t TextColumn::resident_bytes() const noexcept {
    return impl_->resident_bytes();
}

}  // namespace aastore
