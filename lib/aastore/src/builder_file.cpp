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

BufferedFileWriter::~BufferedFileWriter() {
    if (descriptor_ >= 0) {
        try {
            close();
        } catch (...) {
        }
    }
}

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

void BufferedFileWriter::append(std::string_view data) {
    append(data.data(), data.size());
}

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

void BufferedFileWriter::flush_buffer() {
    if (buffered_ == 0) return;
    write_all(buffer_.data(), buffered_);
    buffered_ = 0;
}

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

CompactMetadataWriter::~CompactMetadataWriter() {
    if (output_.is_open()) output_.close();
    if (!finished_) {
        std::error_code error;
        std::filesystem::remove(temporary_path_, error);
    }
}

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

CompactTextMetadataWriter::~CompactTextMetadataWriter() {
    if (output_.is_open()) output_.close();
    if (!finished_) {
        std::error_code error;
        std::filesystem::remove(temporary_path_, error);
    }
}

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
