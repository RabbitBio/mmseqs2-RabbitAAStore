// 文件职责：声明建库使用的顺序写缓冲和紧凑元数据写入器。
// 序列与文本边界均以块基址加块内偏移存储，完成后发布临时元数据文件。
#ifndef AASTORE_BUILDER_FILE_HPP
#define AASTORE_BUILDER_FILE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace aastore {

inline constexpr size_t kBuildStreamBufferBytes = 4 * 1024 * 1024;

// 为建库输出提供带缓冲的顺序文件写入。
class BufferedFileWriter {
public:
    // 创建或截断目标文件，并分配指定容量的顺序写缓冲。
    explicit BufferedFileWriter(
        const std::filesystem::path& path,
        size_t buffer_bytes = kBuildStreamBufferBytes);
    // 尝试刷出剩余数据并关闭文件，析构期间不抛出写入异常。
    ~BufferedFileWriter();

    // 禁止复制文件描述符与写缓冲的所有权。
    BufferedFileWriter(const BufferedFileWriter&) = delete;
    // 禁止复制赋值，避免多个对象重复写入或关闭同一文件。
    BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;

    // 追加指定字节，大块输入可直接写出，零散输入先进入缓冲。
    void append(const char* data, size_t size);
    // 将字符串视图作为原始字节追加，不写入额外终止符。
    void append(std::string_view data);
    // 刷出缓冲并关闭文件，显式报告写入和关闭失败。
    void close();

private:
    // 循环写完指定数据，处理短写并重试被信号中断的系统调用。
    void write_all(const char* data, size_t size);
    // 写出当前缓冲内容，并将缓冲的有效长度归零。
    void flush_buffer();

    std::filesystem::path path_;
    int descriptor_ = -1;
    std::vector<char> buffer_;
    size_t buffered_ = 0;
};

// 以块基址加 32 位偏移写出 AAPCMP3 序列边界。
class CompactMetadataWriter {
public:
    // 创建临时序列元数据文件，预留文件头并写入起始边界零。
    explicit CompactMetadataWriter(const std::filesystem::path& path);
    // 关闭文件，并删除尚未成功发布的临时元数据。
    ~CompactMetadataWriter();

    // 禁止复制元数据写入状态及临时文件所有权。
    CompactMetadataWriter(const CompactMetadataWriter&) = delete;
    // 禁止复制赋值，保持边界计数与输出文件的一致性。
    CompactMetadataWriter& operator=(const CompactMetadataWriter&) = delete;

    // 追加单调递增或相等的全局残基边界，编码为块内 32 位偏移。
    void append(uint64_t position);
    // 校验末尾边界，补齐块基址与文件头，并发布正式元数据文件。
    void finish(uint64_t sequence_count, uint64_t total_residues);

private:
    // 按类型的原始字节表示向元数据流写入一个值。
    template <typename T>
    void write_value(const T& value) {
        output_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    // 将缓存的序列块内偏移批量写出，保留缓冲容量供后续复用。
    void flush_offsets();

    static constexpr uint64_t kBlockShift = 12;
    static constexpr uint64_t kBlockSize = uint64_t{1} << kBlockShift;
    static constexpr size_t kHeaderBytes = 8 + 9 * sizeof(uint64_t);
    static constexpr size_t kBufferEntries = 2 * 1024 * 1024;
    std::filesystem::path final_path_;
    std::filesystem::path temporary_path_;
    std::fstream output_;
    std::vector<uint32_t> offsets_;
    std::vector<uint64_t> bases_;
    uint64_t position_count_ = 0;
    uint64_t last_position_ = 0;
    uint64_t current_base_ = 0;
    bool finished_ = false;
};

// 以相同块索引方案写出 AATEXT1 文本字节边界。
class CompactTextMetadataWriter {
public:
    // 创建临时文本元数据文件，预留文件头并写入起始字节边界。
    explicit CompactTextMetadataWriter(const std::filesystem::path& path);
    // 关闭文件，并清理未完成的临时文本元数据。
    ~CompactTextMetadataWriter();

    // 禁止复制文本边界状态及其输出文件所有权。
    CompactTextMetadataWriter(const CompactTextMetadataWriter&) = delete;
    // 禁止复制赋值，避免不同写入器共用同一个临时文件。
    CompactTextMetadataWriter& operator=(
        const CompactTextMetadataWriter&) = delete;

    // 追加一条文本的结束字节偏移，并维护块基址。
    void append(uint64_t position);
    // 校验记录数和总字节数，写完索引与文件头后发布正式文件。
    void finish(uint64_t record_count, uint64_t total_bytes);

private:
    // 按类型的原始字节表示向文本元数据流写入一个值。
    template <typename T>
    void write_value(const T& value) {
        output_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    // 将缓存的文本块内偏移批量写出并清空有效内容。
    void flush_offsets();

    static constexpr uint64_t kBlockShift = 12;
    static constexpr uint64_t kBlockSize = uint64_t{1} << kBlockShift;
    static constexpr size_t kHeaderBytes = 8 + 6 * sizeof(uint64_t);
    static constexpr size_t kBufferEntries = 2 * 1024 * 1024;
    std::filesystem::path final_path_;
    std::filesystem::path temporary_path_;
    std::fstream output_;
    std::vector<uint32_t> offsets_;
    std::vector<uint64_t> bases_;
    uint64_t position_count_ = 0;
    uint64_t last_position_ = 0;
    uint64_t current_base_ = 0;
    bool finished_ = false;
};

}  // namespace aastore

#endif
