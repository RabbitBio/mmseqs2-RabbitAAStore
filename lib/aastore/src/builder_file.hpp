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

class BufferedFileWriter {
public:
    explicit BufferedFileWriter(
        const std::filesystem::path& path,
        size_t buffer_bytes = kBuildStreamBufferBytes);
    ~BufferedFileWriter();

    BufferedFileWriter(const BufferedFileWriter&) = delete;
    BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;

    void append(const char* data, size_t size);
    void append(std::string_view data);
    void close();

private:
    void write_all(const char* data, size_t size);
    void flush_buffer();

    std::filesystem::path path_;
    int descriptor_ = -1;
    std::vector<char> buffer_;
    size_t buffered_ = 0;
};

class CompactMetadataWriter {
public:
    explicit CompactMetadataWriter(const std::filesystem::path& path);
    ~CompactMetadataWriter();

    CompactMetadataWriter(const CompactMetadataWriter&) = delete;
    CompactMetadataWriter& operator=(const CompactMetadataWriter&) = delete;

    void append(uint64_t position);
    void finish(uint64_t sequence_count, uint64_t total_residues);

private:
    template <typename T>
    void write_value(const T& value) {
        output_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

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

class CompactTextMetadataWriter {
public:
    explicit CompactTextMetadataWriter(const std::filesystem::path& path);
    ~CompactTextMetadataWriter();

    CompactTextMetadataWriter(const CompactTextMetadataWriter&) = delete;
    CompactTextMetadataWriter& operator=(
        const CompactTextMetadataWriter&) = delete;

    void append(uint64_t position);
    void finish(uint64_t record_count, uint64_t total_bytes);

private:
    template <typename T>
    void write_value(const T& value) {
        output_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

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
