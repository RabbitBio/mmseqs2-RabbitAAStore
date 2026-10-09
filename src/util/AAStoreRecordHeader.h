#ifndef AASTORE_RECORD_HEADER_H
#define AASTORE_RECORD_HEADER_H

#include "Util.h"
#include <array>
#include <string>
#include <string_view>

namespace aastore_export {

inline std::array<std::string_view, 3> headerParts(
        std::string_view name, std::string_view description, std::string_view separator) {
    return {name, separator, description};
}

// Native headers use only space/tab/newline/NUL as token separators. A legacy
// raw separator can put CR/VT/FF between fields, so scan the joined token only.
inline std::string token(std::string_view name, std::string_view description,
                         std::string_view separator) {
    std::string result;
    for (const auto field : headerParts(name, description, separator)) {
        const size_t end = field.find_first_of(" \t\n\0", 0, 4);
        const auto part = field.substr(0, end);
        if (!part.empty()) result.append(part.data(), part.size());
        if (end != std::string_view::npos) break;
    }
    return result;
}

inline std::string header(std::string_view name, std::string_view description,
                          std::string_view separator) {
    std::string result;
    for (const auto field : headerParts(name, description, separator)) result.append(field);
    return result;
}

inline std::string accession(std::string_view name, std::string_view description,
                             std::string_view separator) {
    return Util::parseFastaHeader(token(name, description, separator).c_str());
}

inline std::string tsvLabel(std::string_view name, std::string_view description,
                            std::string_view separator, bool fullHeader) {
    return fullHeader ? '"' + header(name, description, separator) + '"'
                      : accession(name, description, separator);
}

} // namespace aastore_export

#endif
