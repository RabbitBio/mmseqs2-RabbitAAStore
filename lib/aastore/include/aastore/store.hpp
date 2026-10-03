// 文件职责：定义 AAStore 的读取接口、结果批次和可复用输出缓冲。
// Store 管理数据库资源，Batch、TextBatch 和 RecordBatch 提供结果访问及生命周期约定。
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

    // 创建无状态分配器，普通平凡元素的默认构造可省略。
    NoInitAllocator() noexcept = default;
    // 允许不同元素类型的无状态分配器相互构造。
    template <typename U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}

    // 分配 count 个元素的原始空间，不初始化元素。
    T* allocate(size_t count) {
        return std::allocator<T>{}.allocate(count);
    }
    // 释放由兼容分配器分配的原始空间，count 须与分配时一致。
    void deallocate(T* pointer, size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }
    // 仅在有构造参数或类型非平凡时构造对象；省略初始化的元素须先写后读。
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

// 无状态分配器之间可互相释放内存，因此始终相等。
template <typename T, typename U>
bool operator==(const NoInitAllocator<T>&,
                const NoInitAllocator<U>&) noexcept {
    return true;
}

// 无状态分配器没有不同的资源实例，因此始终返回 false。
template <typename T, typename U>
bool operator!=(const NoInitAllocator<T>&,
                const NoInitAllocator<U>&) noexcept {
    return false;
}

class ByteArena {
public:
    // 创建未分配缓冲的空 arena。
    ByteArena() = default;
    // 转移缓冲所有权，并清空源对象的尺寸信息。
    ByteArena(ByteArena&& other) noexcept;
    // 通过移动赋值接管缓冲，并释放当前缓冲。
    ByteArena& operator=(ByteArena&& other) noexcept;
    // 禁止复制 arena，避免隐式复制大块数据。
    ByteArena(const ByteArena&) = delete;
    // 禁止复制赋值，缓冲所有权只能通过移动转移。
    ByteArena& operator=(const ByteArena&) = delete;

    // 调整有效字节数而不初始化；扩容时不保留原内容，并使旧视图失效。
    void resize_uninitialized(size_t size);
    // 返回可写缓冲地址，尚未分配时为空。
    char* data() noexcept { return data_.get(); }
    // 返回只读缓冲地址，尚未分配时为空。
    const char* data() const noexcept { return data_.get(); }
    // 返回有效字节数，不是已分配容量。
    size_t size() const noexcept { return size_; }
    // 判断有效字节数是否为零。
    bool empty() const noexcept { return size_ == 0; }
    // 访问可写字节，不做边界检查，index 必须小于 size()。
    char& operator[](size_t index) noexcept { return data_[index]; }
    // 访问只读字节，不做边界检查，index 必须小于 size()。
    const char& operator[](size_t index) const noexcept { return data_[index]; }

private:
    std::unique_ptr<char[]> data_;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

class Batch {
public:
    // 返回批次中的序列数量。
    size_t size() const noexcept { return lengths.size(); }
    // 判断批次是否不含任何序列。
    bool empty() const noexcept { return lengths.empty(); }
    // 返回拥有独立存储的序列副本，批次销毁后仍有效；避免复制可用 view。
    std::string operator[](size_t index) const {
        return std::string(view(index));
    }
    // 返回借用批次存储的序列视图；批次销毁或存储重用后失效。
    std::string_view view(size_t index) const;

    ByteArena storage;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> offsets;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> lengths;
    BatchStats stats;
};

class TextBatch {
public:
    // 返回批次中的文本数量。
    size_t size() const noexcept { return lengths.size(); }
    // 判断文本批次是否为空。
    bool empty() const noexcept { return lengths.empty(); }
    // 返回拥有独立存储的文本副本。
    std::string operator[](size_t index) const {
        return std::string(view(index));
    }
    // 校验索引后返回文本视图；批次销毁或存储重用后失效，不保证 NUL 结尾。
    std::string_view view(size_t index) const;

    ByteArena storage;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> offsets;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> lengths;
    TextBatchStats stats;
};

class RecordBatch {
public:
    // 返回记录批次的记录数量。
    size_t size() const noexcept { return size_; }
    // 判断记录批次是否为空。
    bool empty() const noexcept { return size_ == 0; }
    // 返回本批次实际包含的字段标志。
    RecordFields fields() const noexcept { return fields_; }

    // 返回名称视图；缺少名称字段时抛出异常，视图借用 names 的存储。
    std::string_view name_view(size_t index) const;
    // 返回描述视图；缺少描述字段时抛出异常，视图借用 descriptions 的存储。
    std::string_view description_view(size_t index) const;
    // 返回序列视图；缺少序列字段时抛出异常，视图借用 sequences 的存储。
    std::string_view sequence_view(size_t index) const;
    // 复制指定记录的名称，返回独立字符串。
    std::string name(size_t index) const { return std::string(name_view(index)); }
    // 复制指定记录的描述，返回独立字符串。
    std::string description(size_t index) const {
        return std::string(description_view(index));
    }
    // 复制指定记录的序列，返回独立字符串。
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
    // 打开数据库，按存储和文本选项建立映射或预加载缓存。
    explicit Store(const std::filesystem::path& path,
                   const StoreOptions& options = {});
    // 释放数据库文件、映射和缓存；调用方须先结束借用它们的读取操作。
    ~Store();

    // 转移数据库实现的所有权。
    Store(Store&&) noexcept;
    // 移动赋值接管数据库并释放原有实现；原有借用操作须先结束。
    Store& operator=(Store&&) noexcept;
    // 禁止复制数据库资源。
    Store(const Store&) = delete;
    // 禁止复制赋值，数据库只能通过移动转移所有权。
    Store& operator=(const Store&) = delete;

    // 返回数据库中的序列总数。
    uint64_t size() const noexcept;
    // 返回全部序列的残基总数。
    uint64_t total_residues() const noexcept;
    // 返回 packed 数据的预期有效字节数。
    uint64_t packed_bytes() const noexcept;
    // 返回当前配置的序列存储模式。
    StorageMode storage_mode() const noexcept;
    // 返回已常驻的 packed 数据字节数，不含元数据。
    uint64_t resident_bytes() const;
    // 返回已载入自有缓冲的文本数据字节数，不含文本元数据。
    uint64_t resident_text_bytes() const;
    // 返回打开数据库时序列数据预加载耗时，单位为纳秒。
    uint64_t preload_ns() const noexcept;
    // 判断名称列是否存在。
    bool has_names() const noexcept;
    // 判断描述列是否存在。
    bool has_descriptions() const noexcept;
    // 校验 ID 后返回该序列的残基长度。
    uint64_t sequence_length(SequenceId id) const;
    // 返回当前运行时选择的解码内核名称。
    const char* decode_kernel_name() const noexcept;
    // 读取单条序列，返回含一个元素的批次；读取选项与多条读取一致。
    Batch get(SequenceId id, const ReadOptions& options = {}) const;
    // 调整并复用调用方字符串读取单条序列；并发调用须使用独立输出。
    void get_into(SequenceId id, std::string& output) const;
    // 批量读取序列，结果按输入 ID 顺序访问，并保留重复请求。
    Batch get(const std::vector<SequenceId>& ids,
              const ReadOptions& options = {}) const;
    // 明确把花括号参数视为 ID 列表，尤其 get({}) 表示空批次而不是 ID 0。
    Batch get(std::initializer_list<SequenceId> ids,
              const ReadOptions& options = {}) const;
    // 把序列直接解码到 outputs[i]，返回读取统计，不分配输出 arena 或追加 NUL。
    // output_count 必须等于 ids.size()；各非空序列的输出区不得重叠，
    // 容量至少为 sequence_length(ids[i])；空序列可使用空指针。
    BatchStats get_into(const std::vector<SequenceId>& ids,
                        char* const* outputs, size_t output_count,
                        const ReadOptions& options = {}) const;
    // 使用指针数组提供逐序列输出区，缓冲所有权和容量由调用方管理。
    BatchStats get_into(const std::vector<SequenceId>& ids,
                        const std::vector<char*>& outputs,
                        const ReadOptions& options = {}) const;
    // 读取连续半开 ID 区间，输出为无逐序列 NUL 的连续残基流，通过 view 访问。
    // 每个线程读取并解码自己的分片；文件读取使用 pread，未使用 io_uring。
    Batch sequential_get(SequenceId first_sequence,
                         SequenceId end_sequence,
                         const ReadOptions& options = {}) const;
    // 连续读取 [first_sequence, end_sequence)，直接解码到 outputs[i] 并返回统计。
    // output_count 必须等于 end_sequence - first_sequence；输出区约定同 get_into。
    // outputs[i] 对应 first_sequence + i，不分配输出 arena，不追加 NUL，使用 pread。
    BatchStats sequential_get_into(SequenceId first_sequence,
                                   SequenceId end_sequence,
                                   char* const* outputs, size_t output_count,
                                   const ReadOptions& options = {}) const;
    // 使用指针数组提供连续区间内逐序列输出区，缓冲所有权和容量由调用方管理。
    BatchStats sequential_get_into(SequenceId first_sequence,
                                   SequenceId end_sequence,
                                   const std::vector<char*>& outputs,
                                   const ReadOptions& options = {}) const;
    // 读取指定字段的记录批次，返回结果按输入 ID 顺序访问。
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
