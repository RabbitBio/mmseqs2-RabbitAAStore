// 文件职责：读取名称或描述列，校验文本索引并按请求合并范围、复制结果。
// 支持自有内存中的常驻数据以及 pread、io_uring 读取，文本结果无需经过序列编解码器。
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

// 一条名称或描述读取请求，同时记录源文件字节范围与结果中的输出位置。
struct TextRequest {
    // 文本记录 ID，与对应序列 ID 一致，用作排序键。
    SequenceId id = 0;
    // 保留的原输入下标；当前复制路径直接使用 output_offset 定位输出。
    size_t original_index = 0;
    // 文本数据文件中该记录的起始字节偏移。
    uint64_t begin = 0;
    // 文本结束字节偏移，不含此位置；等于 begin 时为空文本。
    uint64_t end = 0;
    // 该记录在 TextBatch::storage 中的字节偏移，排序后保持不变。
    uint64_t output_offset = 0;
};

// 单列文本的有序读取计划，复用 ByteRange，但源数据是不压缩的文本字节。
struct TextPlan {
    // 合并后的文件区间及其覆盖的有序文本请求下标。
    std::vector<ByteRange> ranges;
    // 诊断开启时统计请求文件区间并集的字节数，不重复统计相同记录。
    uint64_t unique_bytes = 0;
    // 所有合并范围的字节总数，包含跨过的未请求文本。
    uint64_t read_bytes = 0;
    // 诊断开启时统计合并跨越文件间隙的次数。
    uint64_t merged_gaps = 0;
    // 最大单范围字节数，用于约束 io_uring 缓冲预算下的深度。
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

// 按记录 ID 比较文本请求，供稳定排序使用。
bool text_request_less(const TextRequest& left,
                       const TextRequest& right) {
    return left.id < right.id;
}

// 已读文本范围的回填上下文，由 pread 和 io_uring 完成路径共用。
struct TextCopyContext {
    // 用 range_index 查询范围边界以及关联的文本请求。
    const TextPlan* plan = nullptr;
    // 借用的有序请求列表，每条请求保留自己的输出位置。
    const std::vector<TextRequest>* requests = nullptr;
    // 调用方持有的结果批次，各任务只复制到自己的输出区间。
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

// 文本 pread 工作线程的共享读取状态，按范围动态分配任务。
struct TextReadContext {
    // 借用的名称或描述数据文件描述符。
    int descriptor = -1;
    // 要读取的有序文本范围计划，执行期间只读。
    const TextPlan* plan = nullptr;
    // 读取后用于把各条文本复制到结果批次的共享上下文。
    TextCopyContext* copy = nullptr;
    // 原子领取的下一范围下标，由调用方持有。
    std::atomic<size_t>* next_range = nullptr;
    // 各线程独占写入自己的异常槽位，join 后由调用线程检查。
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

// 文本列资源所有者：加载 AATEXT1 索引，按配置持有文件描述符或完整文本缓冲。
struct TextColumn::Impl {
    // 校验文本元数据和数据文件，常驻模式将完整文本读入自有缓冲。
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

    // 关闭文本文件；索引和常驻缓冲随成员析构释放。
    ~Impl() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    // 返回已读入自有缓冲的文本字节数，不含元数据，也不代表实时 RSS。
    uint64_t resident_bytes() const noexcept {
        return resident_data_.size();
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
        if (offsets_[index] > total_bytes_ ||
            block_base > total_bytes_ - offsets_[index]) {
            throw std::runtime_error("text-column boundary exceeds data size");
        }
        return block_base + offsets_[index];
    }

    // 查询文本长度，包含空字符串并拒绝倒置边界。
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

    // 按请求顺序建立输出布局，从常驻缓冲或合并读取范围填充文本批次。
    TextBatch batch_get(const std::vector<SequenceId>& ids,
                        const ReadOptions& options) const {
        TextBatch batch;
        batch.offsets.resize(ids.size());
        batch.lengths.resize(ids.size());
        batch.stats.requested_strings = ids.size();
        // 单条常驻文本或显式 pread 直接填充结果，不创建请求和范围规划。
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

    // 文本记录数，打开时要求与序列总数一致，不计末尾边界。
    uint64_t record_count_ = 0;
    // 文本数据文件总字节数，不含索引文件或额外终止符。
    uint64_t total_bytes_ = 0;
    // 每块边界数的 log2，记录编号右移此值即可定位块基址。
    uint64_t block_shift_ = 0;
    // 本对象持有的数据文件描述符；常驻正文载入后关闭并置为 -1。
    int descriptor_ = -1;
    // 是否在打开时将完整文本读入自有缓冲；此后批读不再读取正文文件。
    bool resident_ = false;
    // 本对象拥有的文本索引映射或缓冲，供 offsets_ 和 bases_ 借用。
    IndexData index_data_;
    // 常驻模式持有的完整文本；普通模式或空列时为空，生命周期与读取器一致。
    std::vector<char, NoInitAllocator<char>> resident_data_;
    // 按记录边界编号索引的 32 位块内字节偏移，共 record_count_ + 1 项。
    const uint32_t* offsets_ = nullptr;
    // 64 位块字节基址数组的原始地址，按字节指针配合 memcpy 读取以兼容未对齐布局。
    const char* bases_ = nullptr;
};

// 打开文本列并校验记录数与序列数据库一致。
TextColumn::TextColumn(const std::filesystem::path& data_path,
                       const std::filesystem::path& metadata_path,
                       uint64_t expected_records,
                       bool resident,
                       IndexLoadMode index_load_mode)
    : impl_(std::make_unique<Impl>(
          data_path, metadata_path, expected_records, resident, index_load_mode)) {}

// 释放文本列文件和索引资源。
TextColumn::~TextColumn() = default;
// 批量读取指定记录的文本，返回独立拥有数据的批次。
TextBatch TextColumn::batch_get(const std::vector<SequenceId>& ids,
                                const ReadOptions& options) const {
    return impl_->batch_get(ids, options);
}

// 从现有元数据查询一条记录的文本字节数。
uint64_t TextColumn::length(SequenceId id) const {
    return impl_->length(id);
}

// 返回配置为常驻模式的文本数据大小，不含元数据。
uint64_t TextColumn::resident_bytes() const noexcept {
    return impl_->resident_bytes();
}

}  // namespace aastore
