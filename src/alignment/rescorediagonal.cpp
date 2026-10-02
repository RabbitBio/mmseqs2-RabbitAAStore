#include "DistanceCalculator.h"
#include "Util.h"
#include "Parameters.h"
#include "Matcher.h"
#include "Debug.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "QueryMatcher.h"
#include "CovSeqidQscPercMinDiag.lib.h"
#include "CovSeqidQscPercMinDiagTargetCov.lib.h"
#include "QueryMatcher.h"
#include "NucleotideMatrix.h"
#include "IndexReader.h"
#include "FastSort.h"
#include "AAStoreTargetReader.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <vector>

#ifdef OPENMP
#include <omp.h>
#endif

float parsePrecisionLib(const std::string &scoreFile, double targetSeqid, double targetCov, double targetPrecision) {
    std::stringstream in(scoreFile);
    std::string line;
    // find closest lower seq. id in a grid of size 5
    int intTargetSeqid = static_cast<int>((targetSeqid + 0.0001) * 100);
    int seqIdRest = (intTargetSeqid % 5);
    targetSeqid = static_cast<float>(intTargetSeqid - seqIdRest) / 100;
    // find closest lower cov. id in a grid of size 10
    targetCov = static_cast<float>(static_cast<int>((targetCov + 0.0001) * 10)) / 10;
    while (std::getline(in, line)) {
        std::vector<std::string> values = Util::split(line, " ");
        float cov = strtod(values[0].c_str(), NULL);
        float seqid = strtod(values[1].c_str(), NULL);
        float scorePerCol = strtod(values[2].c_str(), NULL);
        float precision = strtod(values[3].c_str(), NULL);
        if (MathUtil::AreSame(cov, targetCov) && MathUtil::AreSame(seqid, targetSeqid) && precision >= targetPrecision) {
            return scorePerCol;
        }
    }
    Debug(Debug::WARNING) << "Can not find any score per column for coverage "
                          << targetCov << " and sequence identity " << targetSeqid << ". No hit will be filtered.\n";

    return 0;
}

int doRescorediagonal(Parameters &par,
                      DBWriter &resultWriter,
                      DBReader<unsigned int> &resultReader,
              const size_t dbFrom, const size_t dbSize) {


    IndexReader * qDbrIdx = NULL;
    DBReader<unsigned int> * qdbr = NULL;
    DBReader<unsigned int> * tdbr = NULL;
    bool touch = (par.preloadMode != Parameters::PRELOAD_MODE_MMAP);
    const bool sameQTDB = (par.db2.compare(par.db1) == 0);
    AAStoreTargetReader aastoreSequences;
    const bool useAAStore = sameQTDB && aastoreSequences.openForDatabase(
        par.db2, par.threads, resultReader.getTotalDataSize());
    const unsigned int targetPreloadMode = useAAStore
        ? (touch ? IndexReader::PRELOAD_INDEX : IndexReader::PRELOAD_NO)
        : (touch ? (IndexReader::PRELOAD_INDEX | IndexReader::PRELOAD_DATA)
                 : IndexReader::PRELOAD_NO);
    const int targetDataMode = useAAStore
        ? DBReader<unsigned int>::USE_INDEX
        : (DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    IndexReader * tDbrIdx = new IndexReader(
        par.db2, par.threads, IndexReader::SEQUENCES,
        targetPreloadMode, targetDataMode);
    int querySeqType = 0;
    tdbr = tDbrIdx->sequenceReader;
    int targetSeqType = tDbrIdx->getDbtype();
    if (sameQTDB == true) {
        qDbrIdx = tDbrIdx;
        qdbr = tdbr;
        querySeqType = targetSeqType;
    } else {
        qDbrIdx = new IndexReader(par.db1, par.threads, IndexReader::SEQUENCES,
                                  touch ? IndexReader::PRELOAD_INDEX : 0);
        qdbr = qDbrIdx->sequenceReader;
        querySeqType = qdbr->getDbtype();
    }
    const bool aaStoreDirect = useAAStore && aastoreSequences.fullMemory();
    const bool useAAStoreThreadBatch = useAAStore && !aaStoreDirect;
    const size_t threadBytes = useAAStoreThreadBatch
        ? aastoreSequences.threadBatchBytes() : 0;
    const size_t threadQueueDepth = useAAStoreThreadBatch
        ? AAStoreTargetReader::threadBatchQueueDepth(par.threads) : 0;
    if (useAAStore) {
        if (!Parameters::isEqualDbtype(
                targetSeqType, Parameters::DBTYPE_AMINO_ACIDS)) {
            throw std::runtime_error(
                "AAStore rescorediagonal supports amino-acid databases only");
        }
        if (tdbr->getSize() != aastoreSequences.size()) {
            std::vector<uint32_t> targetKeys(tdbr->getSize());
            for (size_t id = 0; id < targetKeys.size(); ++id) {
                targetKeys[id] = tdbr->getDbKey(id);
            }
            aastoreSequences.mapFromMMseqsKeys(targetKeys);
        }
    }
    if (par.wrappedScoring)
    {
        if(!Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)){
            Debug(Debug::ERROR) << "Wrapped scoring is only supported for nucleotides.\n";
            EXIT(EXIT_FAILURE);
        }
    }


    if(!useAAStore && resultReader.isSortedByOffset() && qdbr->isSortedByOffset()){
        qdbr->setSequentialAdvice();
    }

    BaseMatrix *subMat;
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
        subMat = new NucleotideMatrix(par.scoringMatrixFile.values.nucleotide().c_str(), 1.0, 0.0);
    } else {
        // keep score bias at 0.0 (improved ROC)
        subMat = new SubstitutionMatrix(par.scoringMatrixFile.values.aminoacid().c_str(), 2.0, 0.0);
    }

    SubstitutionMatrix::FastMatrix fastMatrix = SubstitutionMatrix::createAsciiSubMat(*subMat);


    float scorePerColThr = 0.0;
    if (par.filterHits) {
        if (par.rescoreMode == Parameters::RESCORE_MODE_HAMMING) {
            Debug(Debug::WARNING) << "HAMMING distance can not be used to filter hits. Using --rescore-mode 1\n";
            par.rescoreMode = Parameters::RESCORE_MODE_SUBSTITUTION;
        }

        std::string libraryString = (par.covMode == Parameters::COV_MODE_BIDIRECTIONAL)
                                    ? std::string((const char*)CovSeqidQscPercMinDiag_lib, CovSeqidQscPercMinDiag_lib_len)
                                    : std::string((const char*)CovSeqidQscPercMinDiagTargetCov_lib, CovSeqidQscPercMinDiagTargetCov_lib_len);
        scorePerColThr = parsePrecisionLib(libraryString, par.seqIdThr, par.covThr, 0.99);
    }
    bool reversePrefilterResult = (Parameters::isEqualDbtype(resultReader.getDbtype(), Parameters::DBTYPE_PREFILTER_REV_RES));
    EvalueComputation evaluer(tdbr->getAminoAcidDBSize(), subMat);

    size_t totalMemory = Util::getTotalSystemMemory();
    size_t flushSize = 100000000;
    if (totalMemory > resultReader.getTotalDataSize()) {
        flushSize = resultReader.getSize();
    }
    if (aaStoreDirect) {
        Debug(Debug::INFO)
            << "AAStore rescorediagonal: full-memory thread-local get, threads="
            << par.threads << "\n";
    }
    if (useAAStoreThreadBatch) {
        Debug(Debug::INFO) << "AAStore rescorediagonal: thread-local batch_get_into, "
                           << "candidate_bytes_per_thread=" << threadBytes
                           << " query_limit=none"
                           << " aastore_threads=1 compute_threads=" << par.threads
                           << " io_queue_budget=" << AAStoreTargetReader::kThreadBatchIoBudget
                           << " io_queue_depth_per_thread=" << threadQueueDepth
                           << "\n";
    }

    size_t threadBatchCount = 0;
    std::vector<aastore::BatchStats> threadIoStats(useAAStoreThreadBatch ? par.threads : 0);

    size_t processedQueries = 0;
    while (processedQueries < dbSize) {
        const size_t start = dbFrom + processedQueries;
        const size_t bucketSize =
            std::min(dbSize - processedQueries, flushSize);
        Debug::Progress progress(bucketSize);

        auto collectThreadBatch = [&](AAStoreTargetReader::ThreadBatch& batch,
                                      size_t begin,
                                      size_t limit,
                                      size_t maxBytes,
                                      unsigned readerThread) {
            batch.ids.clear();
            batch.begin = begin;
            batch.end = begin;
            batch.candidateBytes = 0;
            size_t bytes = 0;
            char keyBuffer[1024];
            for (size_t query = begin; query < limit; ++query) {
                char* candidates = resultReader.getData(query, readerThread);
                if (*candidates != '\0') {
                    const size_t queryId = qdbr->getId(
                        resultReader.getDbKey(query));
                    batch.ids.push_back(queryId);
                    while (*candidates != '\0') {
                        Util::parseKey(candidates, keyBuffer);
                        const size_t targetId = tdbr->getId(
                            Util::fast_atoi<unsigned int>(keyBuffer));
                        batch.ids.push_back(targetId);
                        bytes += static_cast<size_t>(std::min<uint64_t>(
                            aastoreSequences.sequenceLength(targetId) + 1,
                            std::numeric_limits<size_t>::max() - bytes));
                        candidates = Util::skipLine(candidates);
                    }
                }
                batch.end = query + 1;
                if (bytes >= maxBytes) break;
            }
            batch.candidateBytes = bytes;
        };

        size_t nextThreadBatchQuery = start;
        std::mutex threadBatchCursorMutex;

#pragma omp parallel reduction(+: threadBatchCount)
        {
            unsigned int thread_idx = 0;
#ifdef OPENMP
            thread_idx = (unsigned int) omp_get_thread_num();
#endif
            char buffer[1024 + 32768*4];
            std::string resultBuffer;
            resultBuffer.reserve(1000000);
            std::string queryBuffer;
            queryBuffer.reserve(32768);
            std::string targetBuffer;
            if (aaStoreDirect) targetBuffer.reserve(32768);
            std::vector<Matcher::result_t> alnResults;
            alnResults.reserve(300);
            std::vector<hit_t> shortResults;
            shortResults.reserve(300);
            AAStoreTargetReader::ThreadBatch threadBatch;
            const AAStoreTargetReader::Batch* sequenceBatch = NULL;
            char *queryRevSeq = NULL;
            int queryRevSeqLen = par.maxSeqLen + 1;
            if (reversePrefilterResult == true) {
                queryRevSeq = static_cast<char*>(malloc(queryRevSeqLen));
            }
            auto scoreQuery = [&](size_t id) {
                progress.updateProgress();

                char *data = resultReader.getData(id, thread_idx);
                size_t queryKey = resultReader.getDbKey(id);

                char *querySeq = NULL;
                std::string queryToWrap; // needed only for wrapped end-start scoring
                unsigned int queryId = UINT_MAX;
                int queryLen = -1, origQueryLen = -1;
                if(*data != '\0'){
                    queryId = static_cast<unsigned int>(
                        qdbr->getId(queryKey));
                    if (aaStoreDirect) {
                        try {
                            aastoreSequences.get(queryId, queryBuffer);
                        } catch (const std::exception& error) {
                            Debug(Debug::ERROR) << error.what() << "\n";
                            EXIT(EXIT_FAILURE);
                        }
                        querySeq = queryBuffer.data();
                        queryLen = static_cast<int>(queryBuffer.size());
                    } else if (useAAStoreThreadBatch) {
                        std::string_view sequence;
                        if (!sequenceBatch->view(
                                queryId, sequence)) {
                            Debug(Debug::ERROR)
                                << "Query sequence is missing from AAStore batch\n";
                            EXIT(EXIT_FAILURE);
                        }
                        querySeq = const_cast<char*>(sequence.data());
                        queryLen = static_cast<int>(sequence.size());
                    } else {
                        querySeq = qdbr->getData(queryId, thread_idx);
                        queryLen = static_cast<int>(qdbr->getSeqLen(queryId));
                    }
                    origQueryLen = queryLen;

                    if (par.wrappedScoring){
                        queryToWrap = std::string(querySeq,queryLen);
                        queryToWrap = queryToWrap + queryToWrap;
                        querySeq = (char*)(queryToWrap).c_str();
                        queryLen = origQueryLen*2;
                    }

                    if(reversePrefilterResult == true && queryLen > queryRevSeqLen){
                        queryRevSeq = static_cast<char*>(realloc(queryRevSeq, queryLen+1));
                        queryRevSeqLen = queryLen+1;
                    }
                    if (reversePrefilterResult == true) {
                        NucleotideMatrix *nuclMatrix = (NucleotideMatrix *) subMat;
                        for (int pos = queryLen - 1; pos > -1; pos--) {
                            unsigned char res = subMat->aa2num[static_cast<int>(querySeq[pos])];
                            queryRevSeq[(queryLen - 1) - pos] = subMat->num2aa[nuclMatrix->reverseResidue(res)];
                        }
                    }
                    if (!useAAStore && sameQTDB && qdbr->isCompressed()) {
                        queryBuffer.clear();
                        queryBuffer.append(querySeq, queryLen);
                        querySeq = (char *) queryBuffer.c_str();
                    }
                }
//                if(par.rescoreMode != Parameters::RESCORE_MODE_HAMMING){
//                    query.mapSequence(id, queryId, querySeq);
//                    queryLen = query.L;
//                }else{
                // -2 because of \n\0 in sequenceDB
//                }

                std::vector<hit_t> results =
                    QueryMatcher::parsePrefilterHits(data);
                for (size_t entryIdx = 0; entryIdx < results.size(); entryIdx++) {
                    const hit_t& prefilterHit = results[entryIdx];
                    char *querySeqToAlign = querySeq;
                    bool isReverse = false;
                    if (reversePrefilterResult) {
                        if (prefilterHit.prefScore < 0) {
                            querySeqToAlign = queryRevSeq;
                            isReverse=true;
                        }
                    }

                    const unsigned int targetId = static_cast<unsigned int>(
                        tdbr->getId(prefilterHit.seqId));
                    const bool isIdentity = (queryId == targetId && (par.includeIdentity || sameQTDB)) ? true : false;
                    char *targetSeq = NULL;
                    int dbLen = 0;
                    if (aaStoreDirect) {
                        try {
                            aastoreSequences.get(targetId, targetBuffer);
                        } catch (const std::exception& error) {
                            Debug(Debug::ERROR) << error.what() << "\n";
                            EXIT(EXIT_FAILURE);
                        }
                        targetSeq = targetBuffer.data();
                        dbLen = static_cast<int>(targetBuffer.size());
                    } else if (useAAStoreThreadBatch) {
                        std::string_view sequence;
                        if (!sequenceBatch->view(
                                targetId, sequence)) {
                            Debug(Debug::ERROR)
                                << "Target sequence is missing from AAStore batch\n";
                            EXIT(EXIT_FAILURE);
                        }
                        targetSeq = const_cast<char*>(sequence.data());
                        dbLen = static_cast<int>(sequence.size());
                    } else {
                        targetSeq = tdbr->getData(targetId, thread_idx);
                        dbLen = static_cast<int>(tdbr->getSeqLen(targetId));
                    }

                    float queryLength = static_cast<float>(origQueryLen);
                    float targetLength = static_cast<float>(dbLen);
                    if (Util::canBeCovered(par.covThr, par.covMode, queryLength, targetLength) == false) {
                        continue;
                    }
                    DistanceCalculator::LocalAlignment alignment;
                    if (par.wrappedScoring) {
                        /*
                        if (dbLen > origQueryLen) {
                              Debug(Debug::WARNING) << "WARNING: target sequence " << targetId
                                                    << " is skipped, no valid wrapped scoring possible\n";
                              continue;
                          }
                        */
                        if (dbLen <= origQueryLen) {
                            alignment = DistanceCalculator::computeUngappedWrappedAlignment(
                                querySeqToAlign, queryLen, targetSeq, targetLength,
                                prefilterHit.diagonal, fastMatrix.matrix, par.rescoreMode);
                        }
                        else{
                            alignment = DistanceCalculator::computeUngappedAlignment(
                                querySeqToAlign, origQueryLen, targetSeq, targetLength,
                                prefilterHit.diagonal, fastMatrix.matrix, par.rescoreMode);
                        }
                    }
                    else {
                        alignment = DistanceCalculator::computeUngappedAlignment(
                                querySeqToAlign, queryLen, targetSeq, targetLength,
                                prefilterHit.diagonal, fastMatrix.matrix, par.rescoreMode);
                    }
                    unsigned int distanceToDiagonal = alignment.distToDiagonal;
                    int diagonalLen = alignment.diagonalLen;
                    int distance = alignment.score;
                    int diagonal = alignment.diagonal;
                    double seqId = 0;
                    double evalue = 0.0;
                    int bitScore = 0;
                    int alnLen = 0;
                    float targetCov = static_cast<float>(diagonalLen) / static_cast<float>(dbLen);
                    float queryCov = static_cast<float>(diagonalLen) / static_cast<float>(origQueryLen);

                    Matcher::result_t result;
                    if (par.rescoreMode == Parameters::RESCORE_MODE_HAMMING) {
                        int idCnt = (static_cast<float>(distance));
                        seqId = Util::computeSeqId(par.seqIdMode, idCnt, origQueryLen, dbLen, diagonalLen);
                        alnLen = diagonalLen;
                    } else if (par.rescoreMode == Parameters::RESCORE_MODE_SUBSTITUTION ||
                               par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT ||
                               par.rescoreMode == Parameters::RESCORE_MODE_END_TO_END_ALIGNMENT ||
                               par.rescoreMode == Parameters::RESCORE_MODE_WINDOW_QUALITY_ALIGNMENT) {
                        evalue = evaluer.computeEvalue(distance, origQueryLen);
                        bitScore = static_cast<int>(evaluer.computeBitScore(distance) + 0.5);

                        if (par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT ||
                            par.rescoreMode == Parameters::RESCORE_MODE_END_TO_END_ALIGNMENT ||
                            par.rescoreMode == Parameters::RESCORE_MODE_WINDOW_QUALITY_ALIGNMENT) {
                            alnLen = (alignment.endPos - alignment.startPos) + 1;
                            int qStartPos, qEndPos, dbStartPos, dbEndPos;
                            // -1 since diagonal is computed from sequence Len which starts by 1
                            if (diagonal >= 0) {
                                qStartPos = alignment.startPos + distanceToDiagonal;
                                qEndPos = alignment.endPos + distanceToDiagonal;
                                dbStartPos = alignment.startPos;
                                dbEndPos = alignment.endPos;
                            } else {
                                qStartPos = alignment.startPos;
                                qEndPos = alignment.endPos;
                                dbStartPos = alignment.startPos + distanceToDiagonal;
                                dbEndPos = alignment.endPos + distanceToDiagonal;
                            }
//                                int qAlnLen = std::max(qEndPos - qStartPos, static_cast<int>(1));
//                                int dbAlnLen = std::max(dbEndPos - dbStartPos, static_cast<int>(1));
//                                seqId = (alignment.score1 / static_cast<float>(std::max(qAlnLength, dbAlnLength)))  * 0.1656 + 0.1141;

                            // compute seq.id if hit fulfills e-value but not by seqId criteria
                            if (evalue <= par.evalThr || isIdentity) {
                                int idCnt = 0;
                                for (int i = qStartPos; i <= qEndPos; i++) {
                                    char qLetter = querySeqToAlign[i] & static_cast<unsigned char>(~0x20);
                                    char tLetter = targetSeq[dbStartPos + (i - qStartPos)] & static_cast<unsigned char>(~0x20);
                                    idCnt += (qLetter == tLetter) ? 1 : 0;
                                }
                                seqId = Util::computeSeqId(par.seqIdMode, idCnt, origQueryLen, dbLen, alnLen);
                            }
                            char *end = Itoa::i32toa_sse2(alnLen, buffer);
                            size_t len = end - buffer;
                            std::string backtrace = "";
                            if (par.addBacktrace) {
                                backtrace=std::string(buffer, len - 1);
                                backtrace.push_back('M');
                            }
                            queryCov = SmithWaterman::computeCov(qStartPos, qEndPos, origQueryLen);
                            targetCov = SmithWaterman::computeCov(dbStartPos, dbEndPos, dbLen);
                            if (isReverse) {
                                qStartPos = queryLen - qStartPos - 1;
                                qEndPos = queryLen - qEndPos - 1;
                            }
                            result = Matcher::result_t(prefilterHit.seqId, bitScore, queryCov, targetCov, seqId, evalue, alnLen,
                                                       qStartPos, qEndPos, origQueryLen, dbStartPos, dbEndPos, dbLen, backtrace);
                        }
                    }

                    //float maxSeqLen = std::max(static_cast<float>(targetLen), static_cast<float>(queryLen));
                    float currScorePerCol = static_cast<float>(distance) / static_cast<float>(diagonalLen);
                    // query/target cov mode
                    bool hasCov = Util::hasCoverage(par.covThr, par.covMode, queryCov, targetCov);
                    // --min-seq-id
                    bool hasSeqId = seqId >= (par.seqIdThr - std::numeric_limits<float>::epsilon());
                    bool hasEvalue = (evalue <= par.evalThr);
                    bool hasAlnLen = (alnLen >= par.alnLenThr);

                    // --filter-hits
                    bool hasToFilter = (par.filterHits == true && currScorePerCol >= scorePerColThr);
                    if (isIdentity || hasToFilter || (hasAlnLen && hasCov && hasSeqId && hasEvalue)) {
                        if (par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT ||
                            par.rescoreMode == Parameters::RESCORE_MODE_END_TO_END_ALIGNMENT ||
                            par.rescoreMode == Parameters::RESCORE_MODE_WINDOW_QUALITY_ALIGNMENT) {
                            alnResults.emplace_back(result);
                        } else if (par.rescoreMode == Parameters::RESCORE_MODE_SUBSTITUTION) {
                            hit_t hit;
                            hit.seqId = prefilterHit.seqId;
                            hit.prefScore = (isReverse) ? -bitScore : bitScore;
                            hit.diagonal = diagonal;
                            shortResults.emplace_back(hit);
                        } else {
                            hit_t hit;
                            hit.seqId = prefilterHit.seqId;
                            hit.prefScore = 100 * seqId;
                            hit.prefScore = (isReverse) ? -hit.prefScore : hit.prefScore;
                            hit.diagonal = diagonal;
                            shortResults.emplace_back(hit);
                        }
                    }
                }

                if (par.sortResults > 0 && alnResults.size() > 1) {
                    SORT_SERIAL(alnResults.begin(), alnResults.end(), Matcher::compareHits);
                }
                for (size_t i = 0; i < alnResults.size(); ++i) {
                    size_t len = Matcher::resultToBuffer(buffer, alnResults[i], par.addBacktrace, false);
                    resultBuffer.append(buffer, len);
                }

                if (par.sortResults > 0 && shortResults.size() > 1) {
                    SORT_SERIAL(shortResults.begin(), shortResults.end(), hit_t::compareHitsByScoreAndId);
                }
                for (size_t i = 0; i < shortResults.size(); ++i) {
                    size_t len = QueryMatcher::prefilterHitToBuffer(buffer, shortResults[i]);
                    resultBuffer.append(buffer, len);
                }

                resultWriter.writeData(resultBuffer.c_str(), resultBuffer.length(), queryKey, thread_idx);
                resultBuffer.clear();
                shortResults.clear();
                alnResults.clear();
            };
            if (useAAStoreThreadBatch) {
                while (true) {
                    try {
                        {
                            std::lock_guard<std::mutex> lock(
                                threadBatchCursorMutex);
                            if (nextThreadBatchQuery >= start + bucketSize) {
                                break;
                            }
                            const size_t begin = nextThreadBatchQuery;
                            collectThreadBatch(
                                threadBatch, begin, start + bucketSize,
                                threadBytes, thread_idx);
                            nextThreadBatchQuery = threadBatch.end;
                        }
                        aastoreSequences.fetch_into(threadBatch, par.threads);
                        ++threadBatchCount;
                        AAStoreTargetReader::accumulateIoStats(
                            threadIoStats[thread_idx],
                            threadBatch.sequences.fetched.stats);
                        sequenceBatch = &threadBatch.sequences;
                        for (size_t id = threadBatch.begin;
                             id < threadBatch.end; ++id) {
                            scoreQuery(id);
                        }
                    } catch (const std::exception& error) {
                        Debug(Debug::ERROR) << error.what() << "\n";
                        EXIT(EXIT_FAILURE);
                    }
                }
            } else {
#pragma omp for schedule(dynamic, 1)
                for (size_t id = start; id < (start + bucketSize); id++) {
                    scoreQuery(id);
                }
            }
            if (reversePrefilterResult == true) {
                free(queryRevSeq);
            }
        }
        processedQueries += bucketSize;
        resultReader.remapData();
    }

    if (useAAStoreThreadBatch) {
        Debug(Debug::INFO) << "AAStore rescore thread batches="
                           << threadBatchCount << "\n";
        AAStoreTargetReader::logIoStats(threadIoStats);
    }
    if (tDbrIdx != NULL) {
        delete tDbrIdx;
    }

    if (sameQTDB == false) {
        if(qDbrIdx != NULL){
            delete qDbrIdx;
        }
    }

    delete[] fastMatrix.matrix;
    delete[] fastMatrix.matrixData;
    delete subMat;
    return 0;
}

int rescorediagonal(int argc, const char **argv, const Command &command) {
    MMseqsMPI::init(argc, argv);
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, 0, 0);

    DBReader<unsigned int> resultReader(par.db3.c_str(), par.db3Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    resultReader.open(DBReader<unsigned int>::LINEAR_ACCCESS);
    int dbtype = resultReader.getDbtype(); // this is DBTYPE_PREFILTER_RES || DBTYPE_PREFILTER_REV_RES
    if(par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT ||
       par.rescoreMode == Parameters::RESCORE_MODE_END_TO_END_ALIGNMENT ||
       par.rescoreMode == Parameters::RESCORE_MODE_WINDOW_QUALITY_ALIGNMENT){
        dbtype = Parameters::DBTYPE_ALIGNMENT_RES;
    }
#ifdef HAVE_MPI
    size_t dbFrom = 0;
    size_t dbSize = 0;

    resultReader.decomposeDomainByAminoAcid(MMseqsMPI::rank, MMseqsMPI::numProc, &dbFrom, &dbSize);
    std::string outfile = par.db4;
    std::string outfileIndex = par.db4Index;
    std::pair<std::string, std::string> tmpOutput = Util::createTmpFileNames(outfile, outfileIndex, MMseqsMPI::rank);

    DBWriter resultWriter(tmpOutput.first.c_str(), tmpOutput.second.c_str(), par.threads, par.compressed, dbtype);
    resultWriter.open();
    int status = doRescorediagonal(par, resultWriter, resultReader, dbFrom, dbSize);
    resultWriter.close(true);

    MPI_Barrier(MPI_COMM_WORLD);
    if(MMseqsMPI::rank == 0) {
        std::vector<std::pair<std::string, std::string>> splitFiles;
        for(int proc = 0; proc < MMseqsMPI::numProc; ++proc){
            std::pair<std::string, std::string> tmpFile = Util::createTmpFileNames(outfile, outfileIndex, proc);
            splitFiles.push_back(std::make_pair(tmpFile.first,  tmpFile.second));
        }
        DBWriter::mergeResults(par.db4, par.db4Index, splitFiles);
    }
#else
    DBWriter resultWriter(par.db4.c_str(), par.db4Index.c_str(), par.threads, par.compressed, dbtype);
    resultWriter.open();
    int status = doRescorediagonal(par, resultWriter, resultReader, 0, resultReader.getSize());
    resultWriter.close();

#endif
    resultReader.close();
    return status;
}
