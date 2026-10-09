#ifndef TSV_ROW_FORMATTER_H
#define TSV_ROW_FORMATTER_H

#include <string>
#include <string_view>
#include <utility>

// Both field providers use the native TSV layout and representative rule.
// State survives a bounded AAStore batch that splits one result record.
class TsvRowFormatter {
public:
    TsvRowFormatter(size_t targetColumn, bool firstSeqRepr)
        : targetColumn_(targetColumn), firstSeqRepr_(firstSeqRepr) {}

    void begin(std::string queryHeader) {
        queryHeader_ = std::move(queryHeader);
        first_ = true;
    }

    void append(std::string& output, std::string_view target,
                std::string_view line, size_t keyLength) {
        if (firstSeqRepr_ && first_) queryHeader_ = target;
        output.append(queryHeader_);
        output += '\t';
        output.append(target);
        const size_t offset = targetColumn_ == 0 ? keyLength : 0;
        if (targetColumn_ != 0) output += '\t';
        output.append(line.substr(offset));
        output += '\n';
        first_ = false;
    }

private:
    const size_t targetColumn_;
    const bool firstSeqRepr_;
    std::string queryHeader_;
    bool first_ = true;
};

#endif
