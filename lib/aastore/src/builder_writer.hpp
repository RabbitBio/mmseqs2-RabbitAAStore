#ifndef AASTORE_BUILDER_WRITER_HPP
#define AASTORE_BUILDER_WRITER_HPP

#include "aastore/builder.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace aastore {

struct BuildThreadPlan {
    size_t packed_encoder_threads = 0;
    bool packed_writer_async = false;
    bool packed_fused_worker = false;
    bool text_writer_async = false;
};

BuildThreadPlan plan_build_threads(size_t total_threads,
                                   bool write_text_columns);

class SequenceBuildWriter {
public:
    SequenceBuildWriter(const std::filesystem::path& packed_path,
                        const std::filesystem::path& metadata_path,
                        const BuildThreadPlan& threads);
    ~SequenceBuildWriter();

    SequenceBuildWriter(const SequenceBuildWriter&) = delete;
    SequenceBuildWriter& operator=(const SequenceBuildWriter&) = delete;

    void append_residues(std::string_view residues);
    void finish_sequence();
    void finish();
    const BuildStats& stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class TextBuildWriter {
public:
    TextBuildWriter(const std::filesystem::path& names_path,
                    const std::filesystem::path& names_metadata_path,
                    const std::filesystem::path& descriptions_path,
                    const std::filesystem::path& descriptions_metadata_path,
                    bool write_names,
                    bool write_descriptions,
                    bool asynchronous);
    ~TextBuildWriter();

    TextBuildWriter(const TextBuildWriter&) = delete;
    TextBuildWriter& operator=(const TextBuildWriter&) = delete;

    void append_header(std::string_view header);
    void finish(uint64_t record_count);
    uint64_t name_bytes() const noexcept;
    uint64_t description_bytes() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aastore

#endif
