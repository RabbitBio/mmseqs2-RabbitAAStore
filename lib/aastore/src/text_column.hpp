#ifndef AASTORE_TEXT_COLUMN_HPP
#define AASTORE_TEXT_COLUMN_HPP

#include "aastore/store.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace aastore {

class TextColumn {
public:
    TextColumn(const std::filesystem::path& data_path,
               const std::filesystem::path& metadata_path,
               uint64_t expected_records,
               bool resident = false,
               IndexLoadMode index_load_mode = IndexLoadMode::Mmap);
    ~TextColumn();

    TextColumn(const TextColumn&) = delete;
    TextColumn& operator=(const TextColumn&) = delete;

    TextBatch batch_get(const std::vector<SequenceId>& ids,
                        const ReadOptions& options) const;
    uint64_t length(SequenceId id) const;
    uint64_t resident_bytes() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aastore

#endif
