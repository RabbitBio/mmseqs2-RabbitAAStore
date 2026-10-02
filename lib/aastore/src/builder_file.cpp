// 文件职责：实现建库文件的缓冲写入，以及序列、文本边界的紧凑索引写出。
// 处理短写和中断，补齐索引文件头，并在完成时将临时元数据重命名为正式文件。
#include "builder_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace aastore {

// 创建或截断输出文件，并初始化至少一个字节的写缓冲。
BufferedFileWriter::BufferedFileWriter(const std::filesystem::path& path,
                                       size_t buffer_bytes)
    : path_(path), buffer_(std::max<size_t>(1, buffer_bytes)) {
    descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC |
                                          O_CLOEXEC, 0644);
    if (descriptor_ < 0) {
        throw std::runtime_error("cannot create store file: " +
                                 path.string() + ": " +
                                 std::strerror(errno));
    }
}

// 尽力刷出缓冲并关闭文件；析构期间不传播 I/O 异常。
BufferedFileWriter::~BufferedFileWriter() {
    if (descriptor_ >= 0) {
        try {
            close();
        } catch (...) {
            // 外层建库操作负责报告原始失败。
        }
    }
}

// 将零散数据合入缓冲，大块且无待刷数据时直接写出。
void BufferedFileWriter::append(const char* data, size_t size) {
    if (descriptor_ < 0) {
        throw std::logic_error("append on closed store file");
    }
    while (size != 0) {
        if (buffered_ == 0 && size >= buffer_.size()) {
            write_all(data, size);
            return;
        }
        const size_t available = buffer_.size() - buffered_;
        const size_t copied = std::min(available, size);
        std::memcpy(buffer_.data() + buffered_, data, copied);
        buffered_ += copied;
        data += copied;
        size -= copied;
        if (buffered_ == buffer_.size()) flush_buffer();
    }
}

// 将字符串视图转交给原始字节追加接口，不额外写入终止符。
void BufferedFileWriter::append(std::string_view data) {
    append(data.data(), data.size());
}

// 刷出尚未写入的数据并关闭描述符，已关闭时直接返回。
void BufferedFileWriter::close() {
    if (descriptor_ < 0) return;
    flush_buffer();
    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (::close(descriptor) < 0) {
        throw std::runtime_error("close failed for " + path_.string() +
                                 ": " + std::strerror(errno));
    }
}

// 重试中断和短写，直到所有字节写完或遇到不可恢复错误。
void BufferedFileWriter::write_all(const char* data, size_t size) {
    while (size != 0) {
        const ssize_t written = ::write(descriptor_, data, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("write failed for " + path_.string() +
                                     ": " + std::strerror(errno));
        }
        if (written == 0) {
            throw std::runtime_error("zero-length write for " +
                                     path_.string());
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
}

// 写出缓冲的有效部分，保留分配好的空间供后续追加使用。
void BufferedFileWriter::flush_buffer() {
    if (buffered_ == 0) return;
    write_all(buffer_.data(), buffered_);
    buffered_ = 0;
}

// 建立临时序列索引文件，预留文件头并初始化零号边界。
CompactMetadataWriter::CompactMetadataWriter(
        const std::filesystem::path& path)
    : final_path_(path), temporary_path_(path.string() + ".tmp") {
    output_.open(temporary_path_, std::ios::binary | std::ios::in |
                                      std::ios::out | std::ios::trunc);
    if (!output_) throw std::runtime_error("cannot create compact metadata");
    std::array<char, kHeaderBytes> placeholder{};
    output_.write(placeholder.data(), placeholder.size());
    offsets_.reserve(kBufferEntries);
    append(0);
}

// 关闭序列索引流，并删除尚未成功完成的临时文件。
CompactMetadataWriter::~CompactMetadataWriter() {
    if (output_.is_open()) output_.close();
    if (!finished_) {
        std::error_code error;
        std::filesystem::remove(temporary_path_, error);
    }
}

// 校验边界单调性，将残基位置拆为块基址和 32 位相对偏移。
void CompactMetadataWriter::append(uint64_t position) {
    if (position_count_ != 0 && position < last_position_) {
        throw std::runtime_error("non-monotonic sequence position");
    }
    if ((position_count_ & (kBlockSize - 1)) == 0) {
        current_base_ = position;
        bases_.push_back(position);
    }
    const uint64_t relative = position - current_base_;
    if (relative > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("compact metadata block exceeds uint32");
    }
    offsets_.push_back(static_cast<uint32_t>(relative));
    ++position_count_;
    last_position_ = position;
    if (offsets_.size() == kBufferEntries) flush_offsets();
}

// 校验序列数和末尾位置，写出块基址、回填文件头并发布索引。
void CompactMetadataWriter::finish(uint64_t sequence_count,
                                   uint64_t total_residues) {
    if (position_count_ != sequence_count + 1 ||
        last_position_ != total_residues) {
        throw std::runtime_error("incomplete compact metadata");
    }
    flush_offsets();
    const uint64_t block_count = sequence_count == 0
        ? 0
        : (sequence_count + kBlockSize - 1) / kBlockSize;
    if (bases_.size() == block_count) bases_.push_back(total_residues);
    if (bases_.size() != block_count + 1) {
        throw std::runtime_error("invalid compact metadata bases");
    }
    output_.write(reinterpret_cast<const char*>(bases_.data()),
                  static_cast<std::streamsize>(
                      bases_.size() * sizeof(uint64_t)));
    output_.seekp(0, std::ios::beg);
    char magic[8] = {};
    std::memcpy(magic, "AAPCMP3", 7);
    output_.write(magic, sizeof(magic));
    write_value(uint64_t{3});
    write_value(sequence_count);
    write_value(total_residues);
    write_value(uint64_t{0});
    write_value(uint64_t{0});
    write_value(kBlockShift);
    write_value(block_count);
    write_value(sequence_count + 1);
    write_value(uint64_t{3});
    output_.close();
    if (!output_) throw std::runtime_error("failed to finalize metadata");
    std::filesystem::rename(temporary_path_, final_path_);
    finished_ = true;
}

// 批量写出待处理的序列偏移，写入失败时抛出异常。
void CompactMetadataWriter::flush_offsets() {
    if (offsets_.empty()) return;
    output_.write(reinterpret_cast<const char*>(offsets_.data()),
                  static_cast<std::streamsize>(
                      offsets_.size() * sizeof(uint32_t)));
    if (!output_) {
        throw std::runtime_error("failed to write metadata offsets");
    }
    offsets_.clear();
}

// 建立临时文本索引文件，预留文件头并初始化起始字节位置。
CompactTextMetadataWriter::CompactTextMetadataWriter(
        const std::filesystem::path& path)
    : final_path_(path), temporary_path_(path.string() + ".tmp") {
    output_.open(temporary_path_, std::ios::binary | std::ios::in |
                                      std::ios::out | std::ios::trunc);
    if (!output_) throw std::runtime_error("cannot create text metadata");
    std::array<char, kHeaderBytes> placeholder{};
    output_.write(placeholder.data(), placeholder.size());
    offsets_.reserve(kBufferEntries);
    append(0);
}

// 关闭文本索引流，并删除未发布的临时文件。
CompactTextMetadataWriter::~CompactTextMetadataWriter() {
    if (output_.is_open()) output_.close();
    if (!finished_) {
        std::error_code error;
        std::filesystem::remove(temporary_path_, error);
    }
}

// 校验文本字节边界并追加块内偏移，缓冲满时批量写出。
void CompactTextMetadataWriter::append(uint64_t position) {
    if (position_count_ != 0 && position < last_position_) {
        throw std::runtime_error("non-monotonic text position");
    }
    if ((position_count_ & (kBlockSize - 1)) == 0) {
        current_base_ = position;
        bases_.push_back(position);
    }
    const uint64_t relative = position - current_base_;
    if (relative > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("text metadata block exceeds uint32");
    }
    offsets_.push_back(static_cast<uint32_t>(relative));
    ++position_count_;
    last_position_ = position;
    if (offsets_.size() == kBufferEntries) flush_offsets();
}

// 校验文本记录数和总字节数，补齐索引与文件头后发布文件。
void CompactTextMetadataWriter::finish(uint64_t record_count,
                                       uint64_t total_bytes) {
    if (position_count_ != record_count + 1 ||
        last_position_ != total_bytes) {
        throw std::runtime_error("incomplete text metadata");
    }
    flush_offsets();
    const uint64_t block_count = record_count == 0
        ? 0
        : (record_count + kBlockSize - 1) / kBlockSize;
    if (bases_.size() == block_count) bases_.push_back(total_bytes);
    if (bases_.size() != block_count + 1) {
        throw std::runtime_error("invalid text metadata bases");
    }
    output_.write(reinterpret_cast<const char*>(bases_.data()),
                  static_cast<std::streamsize>(
                      bases_.size() * sizeof(uint64_t)));
    output_.seekp(0, std::ios::beg);
    char magic[8] = {};
    std::memcpy(magic, "AATEXT1", 7);
    output_.write(magic, sizeof(magic));
    write_value(uint64_t{1});
    write_value(record_count);
    write_value(total_bytes);
    write_value(kBlockShift);
    write_value(block_count);
    write_value(record_count + 1);
    output_.close();
    if (!output_) {
        throw std::runtime_error("failed to finalize text metadata");
    }
    std::filesystem::rename(temporary_path_, final_path_);
    finished_ = true;
}

// 将文本偏移缓冲写入临时文件，并清空缓冲的有效内容。
void CompactTextMetadataWriter::flush_offsets() {
    if (offsets_.empty()) return;
    output_.write(reinterpret_cast<const char*>(offsets_.data()),
                  static_cast<std::streamsize>(
                      offsets_.size() * sizeof(uint32_t)));
    if (!output_) {
        throw std::runtime_error("failed to write text metadata offsets");
    }
    offsets_.clear();
}

}  // namespace aastore
