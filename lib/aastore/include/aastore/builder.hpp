#ifndef AASTORE_BUILDER_HPP
#define AASTORE_BUILDER_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace aastore {

struct BuildOptions {
    bool overwrite = false;
    bool write_names = true;
    bool write_descriptions = true;
    size_t threads = 1;

    void validate() const;
};

struct BuildStats {
    uint64_t sequences = 0;
    uint64_t residues = 0;
    uint64_t packed_bytes = 0;
    uint64_t name_bytes = 0;
    uint64_t description_bytes = 0;
};

BuildStats build_fasta(const std::filesystem::path& fasta,
                       const std::filesystem::path& store,
                       const BuildOptions& options = {});

}  // namespace aastore

#endif
