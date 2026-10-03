#include "Parameters.h"
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

int result2repseq(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, 0, 0);

    DBReader<unsigned int> resultReader(par.db2.c_str(), par.db2Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    resultReader.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    AAStoreTargetReader sequences;
    const bool useAAStore = sequences.openForDatabase(
        par.db1, par.threads, resultReader.getTotalDataSize(), par.aastore);
    const int seqMode = DBReader<unsigned int>::USE_INDEX |
        (useAAStore ? 0 : DBReader<unsigned int>::USE_DATA);
    DBReader<unsigned int> seqReader(par.db1.c_str(), par.db1Index.c_str(), par.threads, seqMode);
    seqReader.open(DBReader<unsigned int>::NOSORT);
    if (!useAAStore && par.preloadMode != Parameters::PRELOAD_MODE_MMAP) {
        seqReader.readMmapedDataInMemory();
    }
    if (useAAStore) {
        if (!Parameters::isEqualDbtype(seqReader.getDbtype(), Parameters::DBTYPE_AMINO_ACIDS)) {
            throw std::runtime_error("AAStore result2repseq supports amino-acid databases only");
        }
        if (seqReader.getSize() != sequences.size()) {
            std::vector<uint32_t> keys(seqReader.getSize());
            for (size_t id = 0; id < keys.size(); ++id) keys[id] = seqReader.getDbKey(id);
            sequences.mapFromMMseqsKeys(keys);
        }
        sequences.validateSize(seqReader.getSize());
        AAStoreTargetReader::threadBatchQueueDepth(par.threads);
        Debug(Debug::INFO) << "AAStore result2repseq: thread-local get_into, "
                           << "sequence_bytes_per_thread=" << AAStoreTargetReader::kThreadBatchBytes
                           << " cluster_limit=none\n";
    }

    DBWriter resultWriter(par.db3.c_str(), par.db3Index.c_str(), par.threads, par.compressed, seqReader.getDbtype());
    resultWriter.open();

    Debug::Progress progress(resultReader.getSize());
    size_t batchCount = 0;
#pragma omp parallel reduction(+:batchCount)
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif

        AAStoreTargetReader::ThreadBatch batch;
        std::vector<unsigned int> outputKeys;
        uint64_t batchBytes = 0;
        const auto flushBatch = [&]() {
            if (outputKeys.empty()) return;
            try {
                sequences.fetch_into(batch, par.threads);
                for (size_t i = 0; i < outputKeys.size(); ++i) {
                    std::string_view sequence;
                    if (!batch.sequences.view(batch.ids[i], sequence)) {
                        throw std::runtime_error("Representative is missing from AAStore batch");
                    }
                    resultWriter.writeStart(thread_idx);
                    resultWriter.writeAdd(sequence.data(), sequence.size(), thread_idx);
                    resultWriter.writeAdd("\n", 1, thread_idx);
                    resultWriter.writeEnd(outputKeys[i], thread_idx);
                }
                ++batchCount;
                batch.ids.clear();
                outputKeys.clear();
                batchBytes = 0;
            } catch (const std::exception& error) {
                Debug(Debug::ERROR) << error.what() << "\n";
                EXIT(EXIT_FAILURE);
            }
        };

        char dbKey[255];
#pragma omp for schedule(dynamic, 100) nowait
        for (size_t id = 0; id < resultReader.getSize(); ++id) {
            progress.updateProgress();

            char *results = resultReader.getData(id, thread_idx);
            if (*results == '\0') {
                continue;
            }

            Util::parseKey(results, dbKey);
            const unsigned int key = (unsigned int) strtoul(dbKey, NULL, 10);
            const size_t edgeId = seqReader.getId(key);
            if (useAAStore) {
                if (edgeId == UINT_MAX) {
                    Debug(Debug::ERROR) << "Representative " << key << " does not contain a sequence!\n";
                    EXIT(EXIT_FAILURE);
                }
                batch.ids.push_back(edgeId);
                outputKeys.push_back(resultReader.getDbKey(id));
                batchBytes = aastore_memory::add_bytes(
                    batchBytes, sequences.sequenceLength(edgeId) + 1);
                if (batchBytes >= AAStoreTargetReader::kThreadBatchBytes) flushBatch();
            } else {
                resultWriter.writeData(seqReader.getData(edgeId, thread_idx), seqReader.getEntryLen(edgeId) - 1, resultReader.getDbKey(id), thread_idx);
            }
        }
        flushBatch();
    }
    if (useAAStore) Debug(Debug::INFO) << "AAStore result2repseq batches=" << batchCount << "\n";
    resultWriter.close(true);
    resultReader.close();
    seqReader.close();
    DBReader<unsigned int>::softlinkDb(par.db1, par.db3, DBFiles::SEQUENCE_ANCILLARY);

    return EXIT_SUCCESS;
}
