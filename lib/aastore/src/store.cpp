// 文件职责：实现公开 Store 与批次访问接口，管理文件、元数据、前缀和完整常驻数据。
// 单条读取在本文件完成，批量和连续区间读取分别委托给独立执行模块。
#include "aastore/store.hpp"

#include "byte_range_planner.hpp"
#include "codec.hpp"
#include "metadata.hpp"
#include "store_io.hpp"
#include "text_column.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

namespace aastore {

// 转移 arena 的缓冲区所有权并清空源对象的尺寸信息。
ByteArena::ByteArena(ByteArena&& other) noexcept
    : data_(std::move(other.data_)),
      size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {}

// 用移动赋值替换当前缓冲区并重置源对象。
ByteArena& ByteArena::operator=(ByteArena&& other) noexcept {
    if (this == &other) return *this;
    data_ = std::move(other.data_);
    size_ = std::exchange(other.size_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
    return *this;
}

// 调整逻辑大小，容量不足时增长并按粒度取整；扩容不保留内容，也不初始化。
void ByteArena::resize_uninitialized(size_t size) {
    if (size > capacity_) {
        size_t allocation = size;
        if (capacity_ != 0 && capacity_ <=
                std::numeric_limits<size_t>::max() - capacity_ / 2) {
            allocation = std::max(allocation, capacity_ + capacity_ / 2);
        }
        const size_t granularity = allocation >= 16 * 1024 * 1024
            ? 16 * 1024 * 1024
            : 64 * 1024;
        if (allocation <= std::numeric_limits<size_t>::max() -
                              (granularity - 1)) {
            allocation = ((allocation + granularity - 1) / granularity) *
                         granularity;
        }
        data_.reset(new char[allocation]);
        capacity_ = allocation;
    }
    size_ = size;
}

// 校验 arena 边界后返回指定序列的只读视图。
std::string_view Batch::view(size_t index) const {
    if (index >= lengths.size() || index >= offsets.size()) {
        throw std::out_of_range("batch sequence index out of range");
    }
    const uint64_t offset = offsets[index];
    const uint64_t length = lengths[index];
    if (offset > storage.size() || length > storage.size() - offset) {
        throw std::runtime_error("corrupt batch arena");
    }
    return std::string_view(storage.data() + offset,
                            static_cast<size_t>(length));
}

// 校验文本 arena 边界后返回指定字段的只读视图。
std::string_view TextBatch::view(size_t index) const {
    if (index >= lengths.size() || index >= offsets.size()) {
        throw std::out_of_range("text batch index out of range");
    }
    const uint64_t offset = offsets[index];
    const uint64_t length = lengths[index];
    if (offset > storage.size() || length > storage.size() - offset) {
        throw std::runtime_error("corrupt text batch arena");
    }
    if (length == 0) return {};
    return std::string_view(storage.data() + offset,
                            static_cast<size_t>(length));
}

// 返回记录批次中指定位置的名称字段。
std::string_view RecordBatch::name_view(size_t index) const {
    if (!has_field(fields_, RecordFields::Name)) {
        throw std::logic_error("record batch does not contain names");
    }
    return names.view(index);
}

// 返回记录批次中指定位置的描述字段。
std::string_view RecordBatch::description_view(size_t index) const {
    if (!has_field(fields_, RecordFields::Description)) {
        throw std::logic_error("record batch does not contain descriptions");
    }
    return descriptions.view(index);
}

// 返回记录批次中指定位置的序列字段。
std::string_view RecordBatch::sequence_view(size_t index) const {
    if (!has_field(fields_, RecordFields::Sequence)) {
        throw std::logic_error("record batch does not contain sequences");
    }
    return sequences.view(index);
}

namespace {

// 成对校验并打开可选文本列的数据文件与元数据文件。
std::unique_ptr<TextColumn> open_text_column(
        const std::filesystem::path& store_path,
        const char* data_name,
        const char* metadata_name,
        const char* label,
        uint64_t sequence_count,
        bool resident) {
    const std::filesystem::path data_path = store_path / data_name;
    const std::filesystem::path metadata_path = store_path / metadata_name;
    const bool data_exists = std::filesystem::exists(data_path);
    const bool metadata_exists = std::filesystem::exists(metadata_path);
    if (data_exists != metadata_exists) {
        throw std::runtime_error(
            "incomplete " + std::string(label) + " sidecar in " +
            store_path.string());
    }
    if (!data_exists) return nullptr;
    return std::make_unique<TextColumn>(
        data_path, metadata_path, sequence_count, resident);
}

}  // namespace

// Impl 管理文件、元数据和缓存，并把批次布局、规划与执行委托给内部辅助函数。
struct Store::Impl {
    friend class Store;

    // 打开存储文件，并按配置初始化文本列和 packed 数据缓存。
    Impl(const std::filesystem::path& path, const StoreOptions& options)
        : metadata_(path), storage_mode_(options.storage_mode) {
        const std::filesystem::path packed_path = path / "packed.bin";
        packed_descriptor_ = ::open(
            packed_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (packed_descriptor_ < 0) {
            throw std::runtime_error("cannot open " + packed_path.string() +
                                     ": " + std::strerror(errno));
        }
        try {
            if (file_size(packed_descriptor_) < metadata_.packed_bytes()) {
                throw std::runtime_error(
                    "packed.bin is shorter than metadata");
            }
            names_ = open_text_column(
                path, "names.bin", "names.meta", "name", size(),
                options.resident_names);
            descriptions_ = open_text_column(
                path, "descriptions.bin", "descriptions.meta",
                "description", size(), options.resident_descriptions);
            if (storage_mode_ == StorageMode::FullMemory) {
                preload_full_packed(options.threads);
                ::close(packed_descriptor_);
                packed_descriptor_ = -1;
            } else if (storage_mode_ == StorageMode::PrefixMemory) {
                preload_packed_prefix(
                    options.cache_bytes, options.threads);
            }
        } catch (...) {
            if (packed_descriptor_ >= 0) ::close(packed_descriptor_);
            packed_descriptor_ = -1;
            throw;
        }
    }

    // 关闭仍由 Store 持有的 packed 文件描述符。
    ~Impl() {
        if (packed_descriptor_ >= 0) ::close(packed_descriptor_);
    }

    // 返回元数据中的序列总数。
    uint64_t size() const noexcept { return metadata_.sequence_count(); }

    // 返回所有序列包含的 residue 总数。
    uint64_t total_residues() const noexcept {
        return metadata_.total_residues();
    }

    // 返回全部残基按连续 5 位编码计算的有效字节数。
    uint64_t packed_bytes() const noexcept {
        return metadata_.packed_bytes();
    }

    // 返回当前 Store 使用的存储模式。
    StorageMode storage_mode() const noexcept { return storage_mode_; }

    // 返回序列 packed 数据当前占用的常驻内存字节数。
    uint64_t resident_bytes() const {
        if (storage_mode_ == StorageMode::PrefixMemory) {
            return static_cast<uint64_t>(prefix_packed_.size());
        }
        return static_cast<uint64_t>(full_packed_.size());
    }

    // 返回名称和描述文本列占用的常驻内存字节数。
    uint64_t resident_text_bytes() const noexcept {
        const uint64_t names = names_ ? names_->resident_bytes() : 0;
        const uint64_t descriptions =
            descriptions_ ? descriptions_->resident_bytes() : 0;
        return names + descriptions;
    }

    // 返回打开 Store 时预加载阶段消耗的纳秒数。
    uint64_t preload_ns() const noexcept { return preload_ns_; }

    // 判断名称文本列是否存在。
    bool has_names() const noexcept { return names_ != nullptr; }

    // 判断描述文本列是否存在。
    bool has_descriptions() const noexcept {
        return descriptions_ != nullptr;
    }

    // 组装只读资源视图，供批量与顺序读取模块借用 Store 持有的数据。
    StoreReadContext read_context() const noexcept {
        return StoreReadContext{
            &metadata_, storage_mode_,
            packed_descriptor_, full_packed_.data(), full_packed_.size(),
            prefix_packed_.data(), prefix_packed_.size()};
    }

    // 批量读取请求 ID 对应的名称文本。
    TextBatch batch_get_names(const std::vector<SequenceId>& ids,
                              const ReadOptions& options) const {
        if (!names_) {
            throw std::runtime_error("store has no name column");
        }
        return names_->batch_get(ids, options);
    }

    // 批量读取请求 ID 对应的描述文本。
    TextBatch batch_get_descriptions(
            const std::vector<SequenceId>& ids,
            const ReadOptions& options) const {
        if (!descriptions_) {
            throw std::runtime_error("store has no description column");
        }
        return descriptions_->batch_get(ids, options);
    }

    // 校验 ID 后返回序列的 residue 长度。
    uint64_t sequence_length(SequenceId id) const {
        if (id >= size()) throw std::out_of_range("sequence id out of range");
        return metadata_.start_position(id + 1) -
               metadata_.start_position(id);
    }

    // 把单个序列解码到由 Store 调整大小的字符串中。
    void get_into(SequenceId id, std::string& output) const {
        if (id >= size()) throw std::out_of_range("sequence id out of range");
        const uint64_t start = metadata_.start_position(id);
        const uint64_t length = metadata_.start_position(id + 1) - start;
        if (length > output.max_size()) {
            throw std::length_error("sequence exceeds output string capacity");
        }
        output.resize(static_cast<size_t>(length));
        const SequenceRequest request{id, 0, start, length};
        decode_single(request, output.data());
    }

    // 从完整常驻数据或固定前缀解码，未覆盖的序列使用 pread。
    void decode_single(const SequenceRequest& request,
                       char* output) const {
        if (request.length == 0) return;
        if (storage_mode_ == StorageMode::FullMemory) {
            decode_request_from_extent(
                request, full_packed_.data(), 0,
                full_packed_.size(), output, false);
            return;
        }
        const PackedByteExtent extent =
            packed_byte_extent(request);
        if (storage_mode_ == StorageMode::PrefixMemory &&
            extent.end <= prefix_packed_.size()) {
            decode_request_from_extent(
                request, prefix_packed_.data(), 0,
                prefix_packed_.size(), output, false);
            return;
        }
        const uint64_t bytes = extent.end - extent.begin;
        if (bytes > std::numeric_limits<size_t>::max()) {
            throw std::length_error("single packed read exceeds address space");
        }
        std::vector<uint8_t, NoInitAllocator<uint8_t>> packed(
            static_cast<size_t>(bytes));
        pread_exact(packed_descriptor_, packed.data(), bytes, extent.begin);
        decode_request_from_extent(
            request, packed.data(), extent.begin, extent.end,
            output, false);
    }

    // 创建批次结果并执行通用批量读取流程。
    Batch get(const std::vector<SequenceId>& ids,
              const ReadOptions& options) const {
        Batch batch;
        fill_batch(ids, options, batch);
        return batch;
    }

    // 校验输出数组后直接解码到调用方缓冲，仅返回本次读取统计。
    BatchStats get_into(const std::vector<SequenceId>& ids,
                        char* const* outputs, size_t output_count,
                        const ReadOptions& options) const {
        if (output_count != ids.size()) {
            throw std::invalid_argument("batch output count does not match IDs");
        }
        if (!ids.empty() && outputs == nullptr) {
            throw std::invalid_argument("batch outputs are null");
        }
        Batch batch;
        fill_batch(ids, options, batch, outputs);
        return batch.stats;
    }

    // 校验连续区间及输出数量，让各线程直接解码到调用方的逐序列缓冲。
    BatchStats sequential_get_into(SequenceId first_sequence,
                                   SequenceId end_sequence,
                                   char* const* outputs, size_t output_count,
                                   const ReadOptions& options) const {
        if (first_sequence > end_sequence || end_sequence > size()) {
            throw std::out_of_range("sequential sequence range out of range");
        }
        if (end_sequence - first_sequence != output_count) {
            throw std::invalid_argument("sequential output count does not match range");
        }
        if (output_count != 0 && outputs == nullptr) {
            throw std::invalid_argument("sequential outputs are null");
        }
        Batch batch;
        sequential_get_from_store(
            read_context(), first_sequence, end_sequence, options, batch, outputs);
        return batch.stats;
    }

    // 创建批次结果并读取一个连续 ID 区间。
    Batch sequential_get(SequenceId first_sequence,
                         SequenceId end_sequence,
                         const ReadOptions& options) const {
        Batch batch;
        sequential_get_from_store(
            read_context(), first_sequence, end_sequence, options, batch);
        return batch;
    }

    // 串联布局、缓存分流、范围规划和选定的 I/O 后端。
    void fill_batch(const std::vector<SequenceId>& ids,
                    const ReadOptions& options,
                    Batch& batch,
                    char* const* outputs = nullptr) const {
        fill_batch_from_store(read_context(), ids, options, batch, outputs);
    }
    // 把 packed 文件前缀并行读入内存。
    void preload_packed_prefix(uint64_t capacity_bytes,
                               size_t thread_count) {
        if (capacity_bytes == 0) {
            throw std::invalid_argument(
                "prefix-memory capacity must be greater than zero");
        }
        const uint64_t resident = std::min(
            capacity_bytes, metadata_.packed_bytes());
        if (resident > static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
            throw std::length_error(
                "prefix-memory store exceeds address space");
        }

        const auto preload_start = Clock::now();
        prefix_packed_.resize(static_cast<size_t>(resident));
        if (resident != 0) {
            pread_exact_parallel(
                packed_descriptor_, prefix_packed_.data(), resident, 0,
                std::max<size_t>(1, thread_count), true);
        }
        preload_ns_ = elapsed_ns(preload_start);
    }

    // 把完整 packed 文件并行预加载到内存。
    void preload_full_packed(size_t thread_count) {
        const uint64_t resident = metadata_.packed_bytes();
        if (resident > static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
            throw std::length_error("full-memory store exceeds address space");
        }

        const auto preload_start = Clock::now();
        full_packed_.resize(static_cast<size_t>(resident));
        if (resident != 0) {
            pread_exact_parallel(
                packed_descriptor_, full_packed_.data(), resident, 0,
                std::max<size_t>(1, thread_count), true);
        }
        preload_ns_ = elapsed_ns(preload_start);
    }

    // 持有 meta.bin 的只读映射，提供序列数和字符边界查询。
    Metadata metadata_;
    // 打开 Store 时确定的驻留模式，决定读取从文件、前缀还是全量内存取数据。
    StorageMode storage_mode_ = StorageMode::ByteRange;
    // 本对象拥有的 packed.bin 描述符；全量预加载后关闭并置为 -1。
    int packed_descriptor_ = -1;
    // 可选名称列读取器，列文件不存在时为空。
    std::unique_ptr<TextColumn> names_;
    // 可选描述列读取器，与名称列拥有独立的数据和索引。
    std::unique_ptr<TextColumn> descriptions_;
    // PrefixMemory 模式保留的 packed 文件连续前缀，读取期间内容不变。
    std::vector<uint8_t, NoInitAllocator<uint8_t>> prefix_packed_;
    // FullMemory 模式保留的完整 packed 数据，尚未解码为字符。
    std::vector<uint8_t, NoInitAllocator<uint8_t>> full_packed_;
    // packed 前缀或全量数据预加载的墙钟耗时，单位纳秒。
    uint64_t preload_ns_ = 0;
};

// 打开给定路径的存储并按选项初始化其内部实现。
Store::Store(const std::filesystem::path& path,
             const StoreOptions& options) {
    options.validate();
    impl_ = std::make_unique<Impl>(path, options);
}

// 销毁 Store 及其持有的文件和缓存资源。
Store::~Store() = default;

// 转移另一个 Store 的内部实现所有权。
Store::Store(Store&&) noexcept = default;

// 通过移动赋值接管另一个 Store 的内部实现。
Store& Store::operator=(Store&&) noexcept = default;

// 返回存储中的序列总数。
uint64_t Store::size() const noexcept { return impl_->size(); }

// 返回存储中的 residue 总数。
uint64_t Store::total_residues() const noexcept {
    return impl_->total_residues();
}

// 返回 packed 序列数据的字节数。
uint64_t Store::packed_bytes() const noexcept { return impl_->packed_bytes(); }

// 返回 Store 当前使用的存储模式。
StorageMode Store::storage_mode() const noexcept {
    return impl_->storage_mode();
}

// 返回 packed 序列数据的常驻内存字节数。
uint64_t Store::resident_bytes() const {
    return impl_->resident_bytes();
}

// 返回名称和描述列的常驻内存字节数。
uint64_t Store::resident_text_bytes() const {
    return impl_->resident_text_bytes();
}

// 返回 Store 初始化期间的预加载耗时。
uint64_t Store::preload_ns() const noexcept { return impl_->preload_ns(); }

// 判断名称文本列是否存在。
bool Store::has_names() const noexcept { return impl_->has_names(); }

// 判断描述文本列是否存在。
bool Store::has_descriptions() const noexcept {
    return impl_->has_descriptions();
}

// 返回指定序列的 residue 长度。
uint64_t Store::sequence_length(SequenceId id) const {
    return impl_->sequence_length(id);
}

// 返回运行时选中的 5-bit 解码内核名称。
const char* Store::decode_kernel_name() const noexcept {
    return packed_5bit_decode_kernel_name();
}

// 单条请求复用批量读取语义；仅有一个请求时不执行排序或启动额外工作线程。
Batch Store::get(SequenceId id, const ReadOptions& options) const {
    return get(std::vector<SequenceId>{id}, options);
}

// 把指定序列解码到可自动调整大小的字符串中。
void Store::get_into(SequenceId id, std::string& output) const {
    impl_->get_into(id, output);
}

// 按请求顺序批量读取并返回序列。
Batch Store::get(const std::vector<SequenceId>& ids,
                 const ReadOptions& options) const {
    options.validate();
    return impl_->get(ids, options);
}

// 将花括号列表转为显式 ID 数组，避免空列表被当作单条 ID。
Batch Store::get(std::initializer_list<SequenceId> ids,
                 const ReadOptions& options) const {
    return get(std::vector<SequenceId>(ids), options);
}

// 把序列直接写入逐条输出地址，保留输入顺序并返回读取统计。
BatchStats Store::get_into(const std::vector<SequenceId>& ids,
                           char* const* outputs, size_t output_count,
                           const ReadOptions& options) const {
    options.validate();
    return impl_->get_into(ids, outputs, output_count, options);
}

// 将指针 vector 转交给带显式数量的直接输出接口。
BatchStats Store::get_into(const std::vector<SequenceId>& ids,
                           const std::vector<char*>& outputs,
                           const ReadOptions& options) const {
    return get_into(ids, outputs.data(), outputs.size(), options);
}

// 读取连续 ID 区间并返回字符序列批次。
Batch Store::sequential_get(SequenceId first_sequence,
                            SequenceId end_sequence,
                            const ReadOptions& options) const {
    options.validate();
    return impl_->sequential_get(
        first_sequence, end_sequence, options);
}

// 把连续 ID 区间的字符序列写入逐条输出地址，不追加终止符。
BatchStats Store::sequential_get_into(SequenceId first_sequence,
                                      SequenceId end_sequence,
                                      char* const* outputs, size_t output_count,
                                      const ReadOptions& options) const {
    options.validate();
    return impl_->sequential_get_into(
        first_sequence, end_sequence, outputs, output_count, options);
}

// 将指针 vector 转交给带显式数量的连续区间输出接口。
BatchStats Store::sequential_get_into(SequenceId first_sequence,
                                      SequenceId end_sequence,
                                      const std::vector<char*>& outputs,
                                      const ReadOptions& options) const {
    return sequential_get_into(
        first_sequence, end_sequence, outputs.data(), outputs.size(), options);
}

// 按字段掩码批量读取名称、描述和序列记录。
RecordBatch Store::batch_get_records(
        const std::vector<SequenceId>& ids,
        RecordFields fields,
        const ReadOptions& options) const {
    options.validate();
    constexpr uint8_t known_fields =
        static_cast<uint8_t>(RecordFields::All);
    if ((static_cast<uint8_t>(fields) & ~known_fields) != 0) {
        throw std::invalid_argument("unknown record field mask");
    }
    RecordBatch batch;
    batch.size_ = ids.size();
    batch.fields_ = fields;
    if (has_field(fields, RecordFields::Name)) {
        batch.names = impl_->batch_get_names(ids, options);
    }
    if (has_field(fields, RecordFields::Description)) {
        batch.descriptions = impl_->batch_get_descriptions(ids, options);
    }
    if (has_field(fields, RecordFields::Sequence)) {
        batch.sequences = impl_->get(ids, options);
    }
    return batch;
}

}  // namespace aastore
