#include "AAStoreMemoryBudget.h"
#include "AAStoreRecordHeader.h"
#include "AAStoreTargetReader.h"
#include "DBReader.h"
#include "Debug.h"
#include "Parameters.h"
#include "Util.h"

#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void write_bytes(FILE* output, std::string_view bytes) {
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), output) != bytes.size()) {
        throw std::runtime_error("Cannot write AAStore FASTA output");
    }
}

void write_header(FILE* output, std::string_view name, std::string_view description,
                  std::string_view separator, std::string_view ending = "\n") {
    for (const auto part : aastore_export::headerParts(name, description, separator)) write_bytes(output, part);
    write_bytes(output, ending);
}

void write_sequence(FILE* output, std::string_view sequence) {
    write_bytes(output, sequence);
    write_bytes(output, "\n");
}

}  // namespace

// Single ordered writer, with one bounded records batch. It emits the same
// default result2flat cluster header and createseqfiledb member body directly,
// without creating an ASCII database or copying an output shard afterwards.
int aastore_export_fasta(int argc, const char** argv, const Command& command) {
    Parameters& par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, 0, 0);
    if (!par.aastore || !AAStoreTargetReader::hasNativeRecordHeaders(par.db1)) {
        throw std::runtime_error("aastoreexportfasta requires --aastore 1 and certified AAStore record columns");
    }
    if (par.aastoreRepresentativeOnly && par.hhFormat) {
        throw std::runtime_error("Representative-only FASTA does not support HH format");
    }
    if (!Parameters::isEqualDbtype(FileUtil::parseDbType(par.db1.c_str()), Parameters::DBTYPE_AMINO_ACIDS)) {
        throw std::runtime_error("AAStore FASTA export supports amino-acid databases only");
    }
    AAStoreTargetReader::threadBatchQueueDepth(par.threads);
    DBReader<unsigned int> clusters(par.db2.c_str(), par.db2Index.c_str(), 1,
        DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    clusters.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    // The cluster index has already been allocated and is reflected in the
    // available-memory probe. Keep the full cluster data working set, compact
    // metadata, one records workspace and a conservative writer workspace.
    const uint64_t stageReserve = aastore_memory::add_bytes(clusters.getTotalDataSize(),
        AAStoreTargetReader::kThreadBatchBytes);
    AAStoreTargetReader source;
    if (!source.openForDatabase(par.db1, 1, stageReserve, true, true, UINT64_C(8) * 1024 * 1024 * 1024) ||
        !source.recordsEnabled() || !source.recordsDenseIds()) {
        throw std::runtime_error("AAStore direct export requires a verified dense source-ID certificate");
    }
    Debug(Debug::INFO) << "AAStore aastoreexportfasta: batch_get_records fields=all header=AAStore "
        << "single_writer=1 native_header_index=0 native_sequence_index=0 batch_bytes="
        << AAStoreTargetReader::kThreadBatchBytes << " read_threads=" << par.threads
        << " representative_only=" << (par.aastoreRepresentativeOnly ? 1 : 0) << "\n";
    std::cout.flush();

    // Validate the backend before opening the output, so unsupported input
    // cannot leave a misleading final FASTA behind.
    std::vector<char> outputBuffer(1024 * 1024);
    std::unique_ptr<FILE, int (*)(FILE*)> output(std::fopen(par.db3.c_str(), "wb"), std::fclose);
    if (!output) throw std::runtime_error("Cannot open AAStore FASTA output: " + par.db3);
    if (std::setvbuf(output.get(), outputBuffer.data(), _IOFBF, outputBuffer.size()) != 0) {
        throw std::runtime_error("Cannot buffer AAStore FASTA output");
    }

    struct Fragment {
        unsigned int key;
        size_t begin;
        size_t end;
        bool first;
    };
    std::vector<size_t> ids;
    std::vector<Fragment> fragments;
    std::vector<size_t> missingHeaders;
    AAStoreTargetReader::RecordThreadBatch records, headers;
    uint64_t batchBytes = 0, batches = 0, selectedClusters = 0, members = 0, headerBatches = 0;
    const auto flush = [&]() {
        if (fragments.empty()) return;
        source.fetch_records(ids, records, par.threads, par.threads);
        missingHeaders.clear();
        AAStoreTargetReader::IdPositionMap pending;
        pending.prepare(fragments.size());
        for (const auto& fragment : fragments) {
            size_t position;
            if (fragment.first && !records.positions.find(fragment.key, position) &&
                pending.insert_if_absent(fragment.key, missingHeaders.size())) {
                missingHeaders.push_back(fragment.key);
            }
        }
        if (!missingHeaders.empty()) {
            // Request text fields only for cluster keys absent from the member
            // batch. This never reads or decodes their sequence payloads.
            source.fetch_headers(missingHeaders, headers, par.threads);
            ++headerBatches;
        }
        for (const auto& fragment : fragments) {
            if (fragment.first) {
                size_t position;
                const aastore::RecordBatch* outer;
                if (records.positions.find(fragment.key, position)) outer = &records.records;
                else if (headers.positions.find(fragment.key, position)) outer = &headers.records;
                else throw std::runtime_error("Cluster header is missing from AAStore batch");
                const auto name = outer->name_view(position);
                const auto description = outer->description_view(position);
                const auto separator = source.headerSeparator(description);
                write_bytes(output.get(), ">");
                if (par.aastoreRepresentativeOnly) {
                    // Native result2flat --use-fasta-header replaces the
                    // original header newline with a space before its newline.
                    write_header(output.get(), name, description, separator, " \n");
                    write_sequence(output.get(), records.records.sequence_view(fragment.begin));
                    continue;
                }
                write_bytes(output.get(), aastore_export::accession(name, description, separator));
                write_bytes(output.get(), "\n");
            }
            for (size_t member = fragment.begin; member < fragment.end; ++member) {
                const auto name = records.records.name_view(member);
                const auto description = records.records.description_view(member);
                const auto sequence = records.records.sequence_view(member);
                const auto separator = source.headerSeparator(description);
                if (fragment.first && member == fragment.begin && par.hhFormat) {
                    write_bytes(output.get(), "#");
                    write_header(output.get(), name, description, separator);
                    write_bytes(output.get(), ">");
                    write_bytes(output.get(), aastore_export::token(name, description, separator));
                    write_bytes(output.get(), "_consensus\n");
                    write_sequence(output.get(), sequence);
                }
                write_bytes(output.get(), ">");
                write_header(output.get(), name, description, separator);
                write_sequence(output.get(), sequence);
            }
        }
        ++batches;
        ids.clear();
        fragments.clear();
        batchBytes = 0;
    };

    Debug::Progress progress(clusters.getSize());
    char keyBuffer[255];
    for (size_t cluster = 0; cluster < clusters.getSize(); ++cluster) {
        progress.updateProgress();
        char* data = clusters.getData(cluster, 0);
        if (par.aastoreRepresentativeOnly && *data == '\0') continue;
        const size_t count = Util::countLines(data, clusters.getEntryLen(cluster) - 1);
        if (count < static_cast<unsigned int>(par.minSequences) ||
            count > static_cast<unsigned int>(par.maxSequences)) continue;
        const unsigned int key = clusters.getDbKey(cluster);
        if (key >= source.size()) throw std::runtime_error("Cluster key is outside certified AAStore source");
        ++selectedClusters;
        const uint64_t headerBytes = source.recordLength(key) - source.sequenceLength(key);
        batchBytes = aastore_memory::add_bytes(batchBytes,
            aastore_memory::add_bytes(headerBytes, AAStoreTargetReader::kRecordBatchOverheadBytes));
        size_t begin = ids.size();
        bool first = true;
        while (*data != '\0') {
            Util::parseKey(data, keyBuffer);
            data = Util::skipLine(data);
            const unsigned int memberKey = static_cast<unsigned int>(strtoul(keyBuffer, nullptr, 10));
            if (memberKey >= source.size()) throw std::runtime_error("Member key is outside certified AAStore source");
            ids.push_back(memberKey);
            ++members;
            batchBytes = aastore_memory::add_bytes(batchBytes,
                aastore_memory::add_bytes(source.recordLength(memberKey),
                    AAStoreTargetReader::kRecordBatchOverheadBytes + 3));
            if (par.aastoreRepresentativeOnly) break;
            if (batchBytes >= AAStoreTargetReader::kThreadBatchBytes) {
                fragments.push_back(Fragment{key, begin, ids.size(), first});
                flush();
                first = false;
                begin = 0;
            }
        }
        if (ids.size() != begin || first) {
            fragments.push_back(Fragment{key, begin, ids.size(), first});
        }
        if (batchBytes >= AAStoreTargetReader::kThreadBatchBytes) flush();
    }
    flush();
    FILE* file = output.release();
    if (std::fclose(file) != 0) throw std::runtime_error("Cannot close AAStore FASTA output");
    clusters.close();
    Debug(Debug::INFO) << "AAStore aastoreexportfasta batches=" << batches
        << " clusters=" << selectedClusters << " members=" << members
        << " additional_header_batches=" << headerBatches << "\n";
    return EXIT_SUCCESS;
}
