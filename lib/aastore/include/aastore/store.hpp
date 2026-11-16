#ifndef AASTORE_STORE_HPP
#define AASTORE_STORE_HPP

#include "aastore/builder.hpp"
#include "aastore/read_options.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace aastore {

using SequenceId = uint64_t;

template <typename T>
class NoInitAllocator {
public:
    using value_type = T;

    NoInitAllocator() noexcept = default;
    template <typename U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}

    T* allocate(size_t count) {
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }
    template <typename U, typename... Args>
    void construct(U* pointer, Args&&... args) {
        if constexpr (sizeof...(Args) != 0 ||
                      !std::is_trivially_default_constructible_v<U>) {
            ::new (static_cast<void*>(pointer))
                U(std::forward<Args>(args)...);
        } else {
            (void)pointer;
        }
    }
};

template <typename T, typename U>
bool operator==(const NoInitAllocator<T>&,
                const NoInitAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const NoInitAllocator<T>&,
                const NoInitAllocator<U>&) noexcept {
    return false;
}

class ByteArena {
public:
    ByteArena() = default;
    ByteArena(ByteArena&& other) noexcept;
    ByteArena& operator=(ByteArena&& other) noexcept;
    ByteArena(const ByteArena&) = delete;
    ByteArena& operator=(const ByteArena&) = delete;

    void resize_uninitialized(size_t size);
    char* data() noexcept { return data_.get(); }
    const char* data() const noexcept { return data_.get(); }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    char& operator[](size_t index) noexcept { return data_[index]; }
    const char& operator[](size_t index) const noexcept { return data_[index]; }

private:
    std::unique_ptr<char[]> data_;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

class Batch {
public:
    size_t size() const noexcept { return lengths.size(); }
    bool empty() const noexcept { return lengths.empty(); }
    std::string operator[](size_t index) const {
        return std::string(view(index));
    }
    std::string_view view(size_t index) const;

    ByteArena storage;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> offsets;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> lengths;
    BatchStats stats;
};

class TextBatch {
public:
    size_t size() const noexcept { return lengths.size(); }
    bool empty() const noexcept { return lengths.empty(); }
    std::string operator[](size_t index) const {
        return std::string(view(index));
    }
    std::string_view view(size_t index) const;

    ByteArena storage;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> offsets;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> lengths;
    TextBatchStats stats;
};

class RecordBatch {
public:
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    RecordFields fields() const noexcept { return fields_; }

    std::string_view name_view(size_t index) const;
    std::string_view description_view(size_t index) const;
    std::string_view sequence_view(size_t index) const;
    std::string name(size_t index) const { return std::string(name_view(index)); }
    std::string description(size_t index) const {
        return std::string(description_view(index));
    }
    std::string sequence(size_t index) const {
        return std::string(sequence_view(index));
    }

    TextBatch names;
    TextBatch descriptions;
    Batch sequences;

private:
    friend class Store;
    size_t size_ = 0;
    RecordFields fields_ = RecordFields::None;
};

class Store {
public:
    explicit Store(const std::filesystem::path& path,
                   const StoreOptions& options = {});
    ~Store();

    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    uint64_t size() const noexcept;
    uint64_t total_residues() const noexcept;
    uint64_t packed_bytes() const noexcept;
    StorageMode storage_mode() const noexcept;
    IndexLoadMode index_load_mode() const noexcept;
    uint64_t resident_bytes() const;
    uint64_t resident_text_bytes() const;
    uint64_t preload_ns() const noexcept;
    bool has_names() const noexcept;
    bool has_descriptions() const noexcept;
    uint64_t sequence_length(SequenceId id) const;
    uint64_t name_length(SequenceId id) const;
    uint64_t description_length(SequenceId id) const;
    const char* decode_kernel_name() const noexcept;
    Batch get(SequenceId id, const ReadOptions& options = {}) const;
    void get_into(SequenceId id, std::string& output) const;
    Batch get(const std::vector<SequenceId>& ids,
              const ReadOptions& options = {}) const;
    Batch get(std::initializer_list<SequenceId> ids,
              const ReadOptions& options = {}) const;
    BatchStats get_into(const std::vector<SequenceId>& ids,
                        char* const* outputs, size_t output_count,
                        const ReadOptions& options = {}) const;
    BatchStats get_into(const std::vector<SequenceId>& ids,
                        const std::vector<char*>& outputs,
                        const ReadOptions& options = {}) const;
    Batch sequential_get(SequenceId first_sequence,
                         SequenceId end_sequence,
                         const ReadOptions& options = {}) const;
    BatchStats sequential_get_into(SequenceId first_sequence,
                                   SequenceId end_sequence,
                                   char* const* outputs, size_t output_count,
                                   const ReadOptions& options = {}) const;
    BatchStats sequential_get_into(SequenceId first_sequence,
                                   SequenceId end_sequence,
                                   const std::vector<char*>& outputs,
                                   const ReadOptions& options = {}) const;
    RecordBatch batch_get_records(
        const std::vector<SequenceId>& ids,
        RecordFields fields = RecordFields::All,
        const ReadOptions& options = {}) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aastore

#endif
