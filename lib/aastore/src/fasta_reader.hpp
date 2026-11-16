#ifndef AASTORE_FASTA_READER_HPP
#define AASTORE_FASTA_READER_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace aastore {

class FastaLineReader {
public:
    explicit FastaLineReader(const std::filesystem::path& path);
    ~FastaLineReader();

    FastaLineReader(const FastaLineReader&) = delete;
    FastaLineReader& operator=(const FastaLineReader&) = delete;

    bool next(std::string_view& line, bool& normalized_sequence);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

void normalize_sequence_line(std::string_view line,
                             std::string& normalized_line);

}  // namespace aastore

#endif
