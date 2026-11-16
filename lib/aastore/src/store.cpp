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

ByteArena::ByteArena(ByteArena&& other) noexcept
    : data_(std::move(other.data_)),
      size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {}

ByteArena& ByteArena::operator=(ByteArena&& other) noexcept {
    if (this == &other) return *this;
    data_ = std::move(other.data_);
    size_ = std::exchange(other.size_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
    return *this;
}

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

std::string_view RecordBatch::name_view(size_t index) const {
    if (!has_field(fields_, RecordFields::Name)) {
        throw std::logic_error("record batch does not contain names");
    }
    return names.view(index);
}

std::string_view RecordBatch::description_view(size_t index) const {
    if (!has_field(fields_, RecordFields::Description)) {
        throw std::logic_error("record batch does not contain descriptions");
    }
    return descriptions.view(index);
}

std::string_view RecordBatch::sequence_view(size_t index) const {
    if (!has_field(fields_, RecordFields::Sequence)) {
        throw std::logic_error("record batch does not contain sequences");
    }
    return sequences.view(index);
}

namespace {

std::unique_ptr<TextColumn> open_text_column(
        const std::filesystem::path& store_path,
        const char* data_name,
        const char* metadata_name,
        const char* label,
        uint64_t sequence_count,
        bool resident,
        IndexLoadMode index_load_mode) {
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
        data_path, metadata_path, sequence_count, resident, index_load_mode);
}

}  // namespace

struct Store::Impl {
    friend class Store;

    Impl(const std::filesystem::path& path, const StoreOptions& options)
        : metadata_(path, options.index_load_mode),
          storage_mode_(options.preload_bytes == 0 ? StorageMode::NoMemory
              : options.preload_bytes >= metadata_.packed_bytes()
                  ? StorageMode::FullMemory : StorageMode::PartialMemory),
          index_load_mode_(options.index_load_mode) {
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
                options.resident_names, options.index_load_mode);
            descriptions_ = open_text_column(
                path, "descriptions.bin", "descriptions.meta",
                "description", size(), options.resident_descriptions, options.index_load_mode);
            if (storage_mode_ == StorageMode::FullMemory) {
                preload_full_packed(options.threads);
                ::close(packed_descriptor_);
                packed_descriptor_ = -1;
            } else if (storage_mode_ == StorageMode::PartialMemory) {
                preload_packed_prefix(
                    options.preload_bytes, options.threads);
            }
        } catch (...) {
            if (packed_descriptor_ >= 0) ::close(packed_descriptor_);
            packed_descriptor_ = -1;
            throw;
        }
    }

    ~Impl() {
        if (packed_descriptor_ >= 0) ::close(packed_descriptor_);
    }

    uint64_t size() const noexcept { return metadata_.sequence_count(); }

    uint64_t total_residues() const noexcept {
        return metadata_.total_residues();
    }

    uint64_t packed_bytes() const noexcept {
        return metadata_.packed_bytes();
    }

    StorageMode storage_mode() const noexcept { return storage_mode_; }
    IndexLoadMode index_load_mode() const noexcept { return index_load_mode_; }

    uint64_t resident_bytes() const {
        if (storage_mode_ == StorageMode::PartialMemory) {
            return static_cast<uint64_t>(prefix_packed_.size());
        }
        return static_cast<uint64_t>(full_packed_.size());
    }

    uint64_t resident_text_bytes() const noexcept {
        const uint64_t names = names_ ? names_->resident_bytes() : 0;
        const uint64_t descriptions =
            descriptions_ ? descriptions_->resident_bytes() : 0;
        return names + descriptions;
    }

    uint64_t preload_ns() const noexcept { return preload_ns_; }

    bool has_names() const noexcept { return names_ != nullptr; }

    bool has_descriptions() const noexcept {
        return descriptions_ != nullptr;
    }

    StoreReadContext read_context() const noexcept {
        return StoreReadContext{
            &metadata_, storage_mode_,
            packed_descriptor_, full_packed_.data(), full_packed_.size(),
            prefix_packed_.data(), prefix_packed_.size()};
    }

    TextBatch batch_get_names(const std::vector<SequenceId>& ids,
                              const ReadOptions& options) const {
        if (!names_) {
            throw std::runtime_error("store has no name column");
        }
        return names_->batch_get(ids, options);
    }

    TextBatch batch_get_descriptions(
            const std::vector<SequenceId>& ids,
            const ReadOptions& options) const {
        if (!descriptions_) {
            throw std::runtime_error("store has no description column");
        }
        return descriptions_->batch_get(ids, options);
    }

    uint64_t sequence_length(SequenceId id) const {
        if (id >= size()) throw std::out_of_range("sequence id out of range");
        return metadata_.start_position(id + 1) -
               metadata_.start_position(id);
    }

    uint64_t name_length(SequenceId id) const {
        if (!names_) throw std::runtime_error("store has no name column");
        return names_->length(id);
    }

    uint64_t description_length(SequenceId id) const {
        if (!descriptions_) throw std::runtime_error("store has no description column");
        return descriptions_->length(id);
    }

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
        if (storage_mode_ == StorageMode::PartialMemory &&
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

    Batch get(const std::vector<SequenceId>& ids,
              const ReadOptions& options) const {
        Batch batch;
        fill_batch(ids, options, batch);
        return batch;
    }

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

    Batch sequential_get(SequenceId first_sequence,
                         SequenceId end_sequence,
                         const ReadOptions& options) const {
        Batch batch;
        sequential_get_from_store(
            read_context(), first_sequence, end_sequence, options, batch);
        return batch;
    }

    void fill_batch(const std::vector<SequenceId>& ids,
                    const ReadOptions& options,
                    Batch& batch,
                    char* const* outputs = nullptr) const {
        fill_batch_from_store(read_context(), ids, options, batch, outputs);
    }
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

    Metadata metadata_;
    StorageMode storage_mode_ = StorageMode::NoMemory;
    IndexLoadMode index_load_mode_ = IndexLoadMode::Mmap;
    int packed_descriptor_ = -1;
    std::unique_ptr<TextColumn> names_;
    std::unique_ptr<TextColumn> descriptions_;
    std::vector<uint8_t, NoInitAllocator<uint8_t>> prefix_packed_;
    std::vector<uint8_t, NoInitAllocator<uint8_t>> full_packed_;
    uint64_t preload_ns_ = 0;
};

Store::Store(const std::filesystem::path& path,
             const StoreOptions& options) {
    options.validate();
    impl_ = std::make_unique<Impl>(path, options);
}

Store::~Store() = default;

Store::Store(Store&&) noexcept = default;

Store& Store::operator=(Store&&) noexcept = default;

uint64_t Store::size() const noexcept { return impl_->size(); }

uint64_t Store::total_residues() const noexcept {
    return impl_->total_residues();
}

uint64_t Store::packed_bytes() const noexcept { return impl_->packed_bytes(); }

StorageMode Store::storage_mode() const noexcept {
    return impl_->storage_mode();
}

IndexLoadMode Store::index_load_mode() const noexcept {
    return impl_->index_load_mode();
}

uint64_t Store::resident_bytes() const {
    return impl_->resident_bytes();
}

uint64_t Store::resident_text_bytes() const {
    return impl_->resident_text_bytes();
}

uint64_t Store::preload_ns() const noexcept { return impl_->preload_ns(); }

bool Store::has_names() const noexcept { return impl_->has_names(); }

bool Store::has_descriptions() const noexcept {
    return impl_->has_descriptions();
}

uint64_t Store::sequence_length(SequenceId id) const {
    return impl_->sequence_length(id);
}

uint64_t Store::name_length(SequenceId id) const {
    return impl_->name_length(id);
}

uint64_t Store::description_length(SequenceId id) const {
    return impl_->description_length(id);
}

const char* Store::decode_kernel_name() const noexcept {
    return packed_5bit_decode_kernel_name();
}

Batch Store::get(SequenceId id, const ReadOptions& options) const {
    return get(std::vector<SequenceId>{id}, options);
}

void Store::get_into(SequenceId id, std::string& output) const {
    impl_->get_into(id, output);
}

Batch Store::get(const std::vector<SequenceId>& ids,
                 const ReadOptions& options) const {
    options.validate();
    return impl_->get(ids, options);
}

Batch Store::get(std::initializer_list<SequenceId> ids,
                 const ReadOptions& options) const {
    return get(std::vector<SequenceId>(ids), options);
}

BatchStats Store::get_into(const std::vector<SequenceId>& ids,
                           char* const* outputs, size_t output_count,
                           const ReadOptions& options) const {
    options.validate();
    return impl_->get_into(ids, outputs, output_count, options);
}

BatchStats Store::get_into(const std::vector<SequenceId>& ids,
                           const std::vector<char*>& outputs,
                           const ReadOptions& options) const {
    return get_into(ids, outputs.data(), outputs.size(), options);
}

Batch Store::sequential_get(SequenceId first_sequence,
                            SequenceId end_sequence,
                            const ReadOptions& options) const {
    options.validate();
    return impl_->sequential_get(
        first_sequence, end_sequence, options);
}

BatchStats Store::sequential_get_into(SequenceId first_sequence,
                                      SequenceId end_sequence,
                                      char* const* outputs, size_t output_count,
                                      const ReadOptions& options) const {
    options.validate();
    return impl_->sequential_get_into(
        first_sequence, end_sequence, outputs, output_count, options);
}

BatchStats Store::sequential_get_into(SequenceId first_sequence,
                                      SequenceId end_sequence,
                                      const std::vector<char*>& outputs,
                                      const ReadOptions& options) const {
    return sequential_get_into(
        first_sequence, end_sequence, outputs.data(), outputs.size(), options);
}

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
