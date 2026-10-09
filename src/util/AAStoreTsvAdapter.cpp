#include "AAStoreTsvAdapter.h"
#include "AAStoreMemoryBudget.h"
#include "AAStoreRecordHeader.h"
#include "AAStoreTargetReader.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "FileUtil.h"
#include "Parameters.h"
#include "TsvRowFormatter.h"
#include "Util.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The field provider holds one bounded text batch. The native row formatter
// owns the representative state, including when a result record spans batches.
int aastoreCreateTsv(Parameters& par, bool hasTarget) {
    const std::string result = hasTarget ? par.db3 : par.db2;
    DBReader<unsigned int> reader(result.c_str(), (result + ".index").c_str(), 1,
        DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    reader.open(DBReader<unsigned int>::LINEAR_ACCCESS);
    if (DBReader<unsigned int>::getExtendedDbtype(reader.getDbtype()) & Parameters::DBTYPE_EXTENDED_SET) {
        throw std::runtime_error("AAStore TSV export does not support set databases");
    }
    AAStoreTargetReader source;
    const uint64_t reserve = aastore_memory::add_bytes(reader.getTotalDataSize(), AAStoreTargetReader::kThreadBatchBytes);
    if (!source.openForDatabase(par.db1, 1, reserve, true, true, UINT64_C(8) * 1024 * 1024 * 1024) || !source.recordsDenseIds()) {
        throw std::runtime_error("AAStore TSV export needs certified dense record IDs");
    }
    const std::string output = hasTarget ? par.db4 : par.db3;
    DBWriter writer(output.c_str(), (output + ".index").c_str(), 1,
        par.dbOut && par.compressed, par.dbOut ? Parameters::DBTYPE_GENERIC_DB : Parameters::DBTYPE_OMIT_FILE);
    writer.open();
    const size_t column = par.targetTsvColumn ? par.targetTsvColumn - 1 : std::numeric_limits<size_t>::max();
    TsvRowFormatter formatter(column, par.firstSeqRepr);
    struct Row {
        unsigned int query, target;
        size_t at, length, keyLength;
        bool first, last, empty;
    };
    std::vector<size_t> ids;
    std::vector<Row> rows;
    std::string arena, buffer;
    AAStoreTargetReader::RecordThreadBatch headers;
    uint64_t bytes = 0, batches = 0;
    const auto header = [&](unsigned int key) {
        size_t position;
        if (!headers.positions.find(key, position)) throw std::runtime_error("Missing AAStore TSV header");
        const auto description = headers.records.description_view(position);
        return aastore_export::tsvLabel(headers.records.name_view(position), description,
                                       source.headerSeparator(description), par.fullHeader);
    };
    const auto addId = [&](unsigned int id) {
        if (id >= source.size()) throw std::runtime_error("TSV key is outside AAStore source");
        ids.push_back(id);
        bytes = aastore_memory::add_bytes(bytes, aastore_memory::add_bytes(
            source.recordLength(id) - source.sequenceLength(id), AAStoreTargetReader::kRecordBatchOverheadBytes));
    };
    const auto flush = [&]() {
        if (rows.empty()) return;
        source.fetch_headers(ids, headers, par.threads);
        for (const auto& row : rows) {
            if (row.first) {
                writer.writeStart(0);
                formatter.begin(header(row.query));
            }
            if (!row.empty) {
                const std::string_view line(arena.data() + row.at, row.length);
                std::string target;
                if (column != std::numeric_limits<size_t>::max()) {
                    if (hasTarget) target = header(row.target);
                    else {
                        const char* words[255];
                        Util::getWordsOfLine(line.data(), words, 255);
                        char key[255]; Util::parseKey(words[column], key); target = key;
                    }
                }
                formatter.append(buffer, target, line, row.keyLength);
                // As in native writeData, avoid disabling compression with
                // a short first write when the whole record is >= 60 bytes.
                if (buffer.size() >= 60 || row.last) {
                    writer.writeAdd(buffer.data(), buffer.size(), 0);
                    buffer.clear();
                }
            } else {
                // Match native writeData for an empty compressed record.
                writer.writeAdd("", 0, 0);
            }
            if (row.last) writer.writeEnd(row.query, 0, par.dbOut);
        }
        ++batches; ids.clear(); rows.clear(); arena.clear(); bytes = 0;
    };
    for (size_t i = 0; i < reader.getSize(); ++i) {
        const unsigned int query = reader.getDbKey(i);
        char* data = reader.getData(i, 0);
        bool first = true;
        addId(query);
        if (*data == '\0') rows.push_back({query, 0, 0, 0, 0, true, true, true});
        while (*data != '\0') {
            char* next = Util::skipLine(data);
            unsigned int target = 0;
            size_t keyLength = 0;
            if (column != std::numeric_limits<size_t>::max()) {
                const char* words[255];
                const size_t found = Util::getWordsOfLine(data, words, 255);
                if (column >= found) throw std::runtime_error("TSV target column is missing");
                char key[255]; Util::parseKey(words[column], key);
                keyLength = std::strlen(key);
                target = static_cast<unsigned int>(std::strtoul(key, nullptr, 10));
                if (hasTarget) addId(target);
            }
            const size_t length = static_cast<size_t>(next - data) - 1;
            rows.push_back({query, target, arena.size(), length, keyLength, first, *next == '\0', false});
            arena.append(data, length);
            // Preserve each line's terminator for token parsing after packing
            // several rows into the same arena.
            arena += '\n';
            bytes = aastore_memory::add_bytes(bytes, length + 1 + sizeof(Row));
            first = false; data = next;
            if (bytes >= AAStoreTargetReader::kThreadBatchBytes) flush();
        }
        if (bytes >= AAStoreTargetReader::kThreadBatchBytes) flush();
    }
    flush(); writer.close(!par.dbOut); reader.close();
    if (!par.dbOut) FileUtil::remove((output + ".index").c_str());
    Debug(Debug::INFO) << "AAStore createtsv: batch_get_records fields=name,description native_header_db=0 batches=" << batches << "\n";
    return EXIT_SUCCESS;
}
