#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"

#include "Util.h"
#include "AAStoreMemoryBudget.h"
#include "AAStoreTargetReader.h"

#include <stdexcept>

#ifdef OPENMP
#include <omp.h>
#endif

int createseqfiledb(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, 0, 0);

    DBReader<unsigned int> headerDb(par.hdr1.c_str(), par.hdr1Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    headerDb.open(DBReader<unsigned int>::NOSORT);
    if (par.preloadMode != Parameters::PRELOAD_MODE_MMAP) {
        headerDb.readMmapedDataInMemory();
    }

    DBReader<unsigned int> resultDb(par.db2.c_str(), par.db2Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    resultDb.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    // Header pages and native output strings must not compete with the packed prefix.
    uint64_t stageReserve = aastore_memory::add_bytes(
        headerDb.getTotalDataSize(), resultDb.getTotalDataSize());
    stageReserve = aastore_memory::add_bytes(stageReserve,
        static_cast<uint64_t>(par.threads) * AAStoreTargetReader::kThreadBatchBytes);
    AAStoreTargetReader sequences;
    const bool useAAStore = sequences.openForDatabase(
        par.db1, par.threads, stageReserve, par.aastore);
    const int seqMode = DBReader<unsigned int>::USE_INDEX |
        (useAAStore ? 0 : DBReader<unsigned int>::USE_DATA);
    DBReader<unsigned int> seqDb(par.db1.c_str(), par.db1Index.c_str(), par.threads, seqMode);
    seqDb.open(DBReader<unsigned int>::NOSORT);
    if (!useAAStore && par.preloadMode != Parameters::PRELOAD_MODE_MMAP) {
        seqDb.readMmapedDataInMemory();
    }
    if (useAAStore) {
        if (!Parameters::isEqualDbtype(seqDb.getDbtype(), Parameters::DBTYPE_AMINO_ACIDS)) {
            throw std::runtime_error("AAStore createseqfiledb supports amino-acid databases only");
        }
        if (seqDb.getSize() != sequences.size()) {
            std::vector<uint32_t> keys(seqDb.getSize());
            for (size_t id = 0; id < keys.size(); ++id) keys[id] = seqDb.getDbKey(id);
            sequences.mapFromMMseqsKeys(keys);
        }
        sequences.validateSize(seqDb.getSize());
        AAStoreTargetReader::threadBatchQueueDepth(par.threads);
        Debug(Debug::INFO) << "AAStore createseqfiledb: thread-local get_into, header=native "
                           << "sequence_header_bytes_per_thread=" << AAStoreTargetReader::kThreadBatchBytes
                           << " oversized_clusters=chunked\n";
    }

    DBWriter writer(par.db3.c_str(), par.db3Index.c_str(), static_cast<unsigned int>(par.threads), par.compressed, Parameters::DBTYPE_GENERIC_DB);
    writer.open();

    Debug::Progress progress(resultDb.getSize());
    size_t batchCount = 0;
#pragma omp parallel reduction(+:batchCount)
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif
        std::string result;
        result.reserve(1024);

        AAStoreTargetReader::ThreadBatch batch;
        std::vector<size_t> headerIds;
        struct ClusterFragment {
            unsigned int key;
            size_t end;
            bool first;
            bool last;
        };
        std::vector<ClusterFragment> fragments;
        uint64_t batchBytes = 0;
        const auto appendMember = [&](size_t headerId, size_t seqId, bool first) {
            std::string_view sequence;
            if (useAAStore) {
                if (!batch.sequences.view(seqId, sequence)) {
                    throw std::runtime_error("Member is missing from AAStore batch");
                }
            } else {
                sequence = std::string_view(seqDb.getData(headerId, thread_idx),
                                            seqDb.getEntryLen(headerId) - 1);
            }
            const auto appendSequence = [&]() {
                result.append(sequence.data(), sequence.size());
                if (useAAStore) result.push_back('\n');
            };
            if (first && par.hhFormat) {
                char *header = headerDb.getData(headerId, thread_idx);
                size_t headerLen = headerDb.getEntryLen(headerId) - 1;
                size_t accessionLen = Util::skipNoneWhitespace(header);
                result.append(1, '#');
                result.append(header, headerLen);
                result.append(1, '>');
                result.append(header, accessionLen);
                result.append("_consensus\n");
                appendSequence();
                result.append(1, '>');
                result.append(header, headerLen);
                appendSequence();
            } else {
                result.append(1, '>');
                result.append(headerDb.getData(headerId, thread_idx), headerDb.getEntryLen(headerId) - 1);
                appendSequence();
            }
        };
        const auto flushBatch = [&]() {
            if (fragments.empty()) return;
            try {
                sequences.fetch_into(batch, par.threads);
                size_t begin = 0;
                for (const auto& fragment : fragments) {
                    if (fragment.first) writer.writeStart(thread_idx);
                    for (size_t member = begin; member < fragment.end; ++member) {
                        appendMember(headerIds[member], batch.ids[member],
                                     fragment.first && member == begin);
                    }
                    // DBWriter disables record compression if its first write is < 60 bytes.
                    if (result.size() >= 60 || fragment.last) {
                        writer.writeAdd(result.data(), result.size(), thread_idx);
                        result.clear();
                    }
                    if (fragment.last) writer.writeEnd(fragment.key, thread_idx);
                    begin = fragment.end;
                }
                ++batchCount;
                batch.ids.clear();
                headerIds.clear();
                fragments.clear();
                batchBytes = 0;
            } catch (const std::exception& error) {
                Debug(Debug::ERROR) << error.what() << "\n";
                EXIT(EXIT_FAILURE);
            }
        };

        char dbKey[255];
#pragma omp for schedule(dynamic, 100) nowait
        for (size_t i = 0; i < resultDb.getSize(); ++i) {
            progress.updateProgress();

            unsigned int key = resultDb.getDbKey(i);
            char *data = resultDb.getData(i, thread_idx);

            size_t entries = Util::countLines(data, resultDb.getEntryLen(i) - 1);
            if (entries < (unsigned int) par.minSequences || entries > (unsigned int) par.maxSequences) {
                continue;
            }

            size_t entries_num = 0;
            size_t fragmentBegin = batch.ids.size();
            bool firstFragment = true;
            while (*data != '\0') {
                entries_num++;
                Util::parseKey(data, dbKey);
                data = Util::skipLine(data);

                const unsigned int memberKey = (unsigned int) strtoul(dbKey, NULL, 10);
                size_t headerId = headerDb.getId(memberKey);
                if (headerId == UINT_MAX) {
                    Debug(Debug::ERROR) << "Entry " << key << " does not contain a sequence!" << "\n";
                    EXIT(EXIT_FAILURE);
                }
                size_t seqId = seqDb.getId(memberKey);
                if (seqId == UINT_MAX) {
                    Debug(Debug::ERROR) << "Entry " << key << " does not contain a sequence!" << "\n";
                    EXIT(EXIT_FAILURE);
                }
                if (useAAStore) {
                    batch.ids.push_back(seqId);
                    headerIds.push_back(headerId);
                    batchBytes = aastore_memory::add_bytes(batchBytes,
                        aastore_memory::add_bytes(sequences.sequenceLength(seqId) + 1,
                                                 headerDb.getEntryLen(headerId)));
                    if (batchBytes >= AAStoreTargetReader::kThreadBatchBytes) {
                        fragments.push_back({key, batch.ids.size(), firstFragment, *data == '\0'});
                        flushBatch();
                        firstFragment = false;
                        fragmentBegin = 0;
                    }
                } else {
                    appendMember(headerId, seqId, entries_num == 1);
                }
            }
            if (useAAStore) {
                if (batch.ids.size() != fragmentBegin || firstFragment) {
                    fragments.push_back({key, batch.ids.size(), firstFragment, true});
                }
            } else {
                writer.writeData(result.c_str(), result.length(), key, thread_idx);
                result.clear();
            }
        }
        flushBatch();
    }
    if (useAAStore) Debug(Debug::INFO) << "AAStore createseqfiledb batches=" << batchCount << "\n";

    writer.close();
    resultDb.close();
    seqDb.close();
    headerDb.close();

    return EXIT_SUCCESS;
}
