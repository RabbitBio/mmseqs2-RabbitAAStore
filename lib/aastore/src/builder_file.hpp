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

    // 输出路径，用于在写入或关闭失败时报告具体文件。
    std::filesystem::path path_;
    // 本对象持有的可写文件描述符；-1 表示未打开或已经关闭。
    int descriptor_ = -1;
    // 固定大小的顺序写缓冲，暂存尚未调用 write 写出的字节。
    std::vector<char> buffer_;
    // buffer_ 中待写出的有效字节数，范围为 [0, buffer_.size()]。
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

    // 每块边界数的 log2，用移位和掩码定位块边界。
    static constexpr uint64_t kBlockShift = 12;
    // 每 4096 个边界共享一个 64 位字符基址。
    static constexpr uint64_t kBlockSize = uint64_t{1} << kBlockShift;
    // AAPCMP3 文件头预留字节数，finish 时回填。
    static constexpr size_t kHeaderBytes = 8 + 9 * sizeof(uint64_t);
    // 每次批量写出的 32 位偏移项数上限，不是字节数。
    static constexpr size_t kBufferEntries = 2 * 1024 * 1024;
    // finish 成功后发布元数据的目标路径。
    std::filesystem::path final_path_;
    // 写入中的临时路径，未成功发布时由析构清理。
    std::filesystem::path temporary_path_;
    // 临时文件流，顺序写数据并在结束时回填文件头。
    std::fstream output_;
    // 尚未刷出的 32 位块内字符偏移；刷出后复用容量。
    std::vector<uint32_t> offsets_;
    // 累计的 64 位块字符基址，结束时写在偏移数组之后。
    std::vector<uint64_t> bases_;
    // 已接收边界数，包含最初的零边界和各序列的结束边界。
    uint64_t position_count_ = 0;
    // 最近一次追加的全局字符位置，用于检查单调性和末尾长度。
    uint64_t last_position_ = 0;
    // 当前块的全局字符基址，新边界减去它得到 32 位偏移。
    uint64_t current_base_ = 0;
    // 元数据是否已成功完成并发布，决定析构是否清理临时文件。
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

    // 每块文本边界数的 log2，与序列索引使用相同分块粒度。
    static constexpr uint64_t kBlockShift = 12;
    // 每 4096 个边界共享一个 64 位字节基址。
    static constexpr uint64_t kBlockSize = uint64_t{1} << kBlockShift;
    // AATEXT1 文件头预留字节数，finish 时回填。
    static constexpr size_t kHeaderBytes = 8 + 6 * sizeof(uint64_t);
    // 偏移写缓冲最多容纳的 uint32_t 项数。
    static constexpr size_t kBufferEntries = 2 * 1024 * 1024;
    // 完成后发布的文本索引路径，例如 names.meta。
    std::filesystem::path final_path_;
    // 构建中的临时索引路径，失败时删除。
    std::filesystem::path temporary_path_;
    // 用于写偏移、块基址和回填文件头的临时文件流。
    std::fstream output_;
    // 尚未刷出的块内字节偏移，单位与序列索引的字符偏移不同。
    std::vector<uint32_t> offsets_;
    // 各块在文本数据文件中的 64 位字节基址。
    std::vector<uint64_t> bases_;
    // 已接收的文本边界数，包含起始零边界。
    uint64_t position_count_ = 0;
    // 最近一次追加的文本结束字节位置。
    uint64_t last_position_ = 0;
    // 当前块的文本字节基址，用于计算新边界的相对偏移。
    uint64_t current_base_ = 0;
    // 是否已成功发布文件；成功后析构不再尝试清理临时文件。
    bool finished_ = false;
};

}  // namespace aastore

#endif
