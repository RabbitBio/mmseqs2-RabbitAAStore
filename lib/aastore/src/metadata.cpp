#include "metadata.hpp"

#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace aastore {
namespace {

template <typename T>
bool read_value(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}

size_t checked_size(uint64_t value, const char* description) {
    if (value > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error(description);
    }
    return static_cast<size_t>(value);
}

}  // namespace

Metadata::Metadata(const std::filesystem::path& store, IndexLoadMode mode) {
    open(store, mode);
}

Metadata::~Metadata() { clear(); }

void Metadata::clear() noexcept {
    index_data_.clear();
    offsets_ = nullptr;
    bases_ = nullptr;
    block_shift_ = 0;
}

uint64_t Metadata::packed_bytes() const noexcept {
    return (total_residues_ / 8) * 5 +
           ((total_residues_ % 8) * 5 + 7) / 8;
}

uint64_t Metadata::start_position(uint64_t index) const {
    if (index > sequence_count_) {
        throw std::out_of_range("sequence index out of range");
    }
    const uint64_t block = index >> block_shift_;
    uint64_t base = 0;
    std::memcpy(&base, bases_ + block * sizeof(base), sizeof(base));
    return base + offsets_[index];
}

void Metadata::open(const std::filesystem::path& store, IndexLoadMode mode) {
    const std::filesystem::path path = store / "meta.bin";
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    char magic[8] = {};
    uint64_t version = 0;
    uint64_t sequence_count = 0;
    uint64_t total_residues = 0;
    uint64_t reserved_zero_one = 0;
    uint64_t reserved_zero_two = 0;
    uint64_t block_shift = 0;
    uint64_t block_count = 0;
    uint64_t offset_count = 0;
    uint64_t encoding_flags = 0;
    constexpr char kMagic[8] = {'A', 'A', 'P', 'C', 'M', 'P', '3', '\0'};
    input.read(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(magic)) != 0 ||
        !read_value(input, version) ||
        version != 3 ||
        !read_value(input, sequence_count) ||
        !read_value(input, total_residues) ||
        !read_value(input, reserved_zero_one) ||
        !read_value(input, reserved_zero_two) ||
        !read_value(input, block_shift) ||
        !read_value(input, block_count) ||
        !read_value(input, offset_count) ||
        !read_value(input, encoding_flags)) {
        throw std::runtime_error(
            "invalid AAPCMP3 metadata header: " + path.string());
    }
    if (sequence_count == std::numeric_limits<uint64_t>::max() ||
        block_shift == 0 || block_shift > 31 || reserved_zero_one != 0 ||
        reserved_zero_two != 0 || encoding_flags != 3 ||
        offset_count != sequence_count + 1) {
        throw std::runtime_error(
            "invalid AAPCMP3 metadata layout: " + path.string());
    }
    const uint64_t block_size = uint64_t{1} << block_shift;
    const uint64_t expected_block_count = sequence_count == 0
        ? 0
        : (sequence_count - 1) / block_size + 1;
    if (block_count != expected_block_count) {
        throw std::runtime_error(
            "invalid AAPCMP3 block count: " + path.string());
    }

    constexpr uint64_t kHeaderBytes = 8 + 9 * sizeof(uint64_t);
    const uint64_t bases_bytes = (block_count + 1) * sizeof(uint64_t);
    const uint64_t offsets_bytes = offset_count * sizeof(uint32_t);
    const uint64_t expected_bytes = kHeaderBytes + bases_bytes + offsets_bytes;
    if (std::filesystem::file_size(path) != expected_bytes) {
        throw std::runtime_error(
            "invalid AAPCMP3 metadata size: " + path.string());
    }

    clear();
    index_data_.open(path, checked_size(expected_bytes, "metadata exceeds address space"), mode);
    const char* base = index_data_.data();
    offsets_ = reinterpret_cast<const uint32_t*>(base + kHeaderBytes);
    bases_ = base + kHeaderBytes + offsets_bytes;
    block_shift_ = static_cast<uint32_t>(block_shift);
    sequence_count_ = sequence_count;
    total_residues_ = total_residues;
    if (start_position(0) != 0 ||
        start_position(sequence_count_) != total_residues_) {
        clear();
        throw std::runtime_error(
            "invalid AAPCMP3 sequence offsets: " + path.string());
    }
}

}  // namespace aastore
