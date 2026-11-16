#ifndef AASTORE_METADATA_HPP
#define AASTORE_METADATA_HPP

#include "index_data.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace aastore {

class Metadata {
public:
    explicit Metadata(const std::filesystem::path& store,
                      IndexLoadMode mode = IndexLoadMode::Mmap);
    ~Metadata();

    Metadata(const Metadata&) = delete;
    Metadata& operator=(const Metadata&) = delete;

    uint64_t sequence_count() const noexcept { return sequence_count_; }
    uint64_t total_residues() const noexcept { return total_residues_; }
    uint64_t packed_bytes() const noexcept;
    uint64_t start_position(uint64_t sequence_index) const;

private:
    void open(const std::filesystem::path& store, IndexLoadMode mode);
    void clear() noexcept;

    uint64_t sequence_count_ = 0;
    uint64_t total_residues_ = 0;

    IndexData index_data_;
    const uint32_t* offsets_ = nullptr;
    const char* bases_ = nullptr;
    uint32_t block_shift_ = 0;
};

}  // namespace aastore

#endif
