#include "Alignment.h"
#include "Util.h"
#include "Debug.h"
#include "Matcher.h"
#include "QueryMatcher.h"
#include "DBWriter.h"
#include "IndexReader.h"
#include "NucleotideMatrix.h"
#include "SubstitutionMatrix.h"
#include "SubstitutionMatrixProfileStates.h"
#include "FileUtil.h"
#include "Parameters.h"
#include "FastSort.h"
#include "Sequence.h"

#ifdef OPENMP
#include <omp.h>
#endif

#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>

Alignment::Alignment(const std::string &querySeqDB, const std::string &targetSeqDB,
                     const std::string &prefDB, const std::string &prefDBIndex,
                     const std::string &outDB, const std::string &outDBIndex, const Parameters &par, const bool lcaAlign) :
        covThr(par.covThr), canCovThr(par.covThr), covMode(par.covMode), seqIdMode(par.seqIdMode), evalThr(par.evalThr), seqIdThr(par.seqIdThr),
        alnLenThr(par.alnLenThr), includeIdentity(par.includeIdentity), addBacktrace(par.addBacktrace), realign(par.realign), scoreBias(par.scoreBias), realignScoreBias(par.realignScoreBias), realignMaxSeqs(par.realignMaxSeqs),
        threads(static_cast<unsigned int>(par.threads)), compressed(par.compressed), outDB(outDB), outDBIndex(outDBIndex),
        maxSeqLen(par.maxSeqLen), compBiasCorrection(par.compBiasCorrection), compBiasCorrectionScale(par.compBiasCorrectionScale), altAlignment(par.altAlignment), alignmentOutputMode(par.alignmentOutputMode),
        maxAccept(static_cast<unsigned int>(par.maxAccept)), maxReject(static_cast<unsigned int>(par.maxRejected)), wrappedScoring(par.wrappedScoring),
        lcaAlign(lcaAlign), qdbr(NULL), qDbrIdx(NULL), tdbr(NULL), tDbrIdx(NULL),
        useAAStoreTarget(false), aaStoreDirect(false) {
    unsigned int alignmentMode = par.alignmentMode;
    if (alignmentMode == Parameters::ALIGNMENT_MODE_UNGAPPED) {
        Debug(Debug::ERROR) << "Use rescorediagonal for ungapped alignment mode.\n";
        EXIT(EXIT_FAILURE);
    }

    if (lcaAlign == false && addBacktrace == true) {
        alignmentMode = Parameters::ALIGNMENT_MODE_SCORE_COV_SEQID;
    }

    if (lcaAlign == true) {
        lcaSwMode = initSWMode(std::max(alignmentMode, (unsigned int)Parameters::ALIGNMENT_MODE_SCORE_ONLY), 0.0f, 0.0f);
        realign = true;
        realignScoreBias = 0.0f;
        realignMaxSeqs = 1;
        addBacktrace = false;
    }

    if (realign == true) {
        realignSwMode = initSWMode(std::max(alignmentMode, (unsigned int)Parameters::ALIGNMENT_MODE_SCORE_COV), 0.0f, 0.0f);
        alignmentMode = Parameters::ALIGNMENT_MODE_SCORE_ONLY;
        realignCov = par.covThr;
        covThr = 0.0;
        if (addBacktrace == false && lcaAlign == false) {
            Debug(Debug::WARNING) << "Turn on backtrace for realign.\n";
            addBacktrace = true;
        }
    }

    uint16_t extended = DBReader<unsigned int>::getExtendedDbtype(FileUtil::parseDbType(prefDB.c_str()));
    bool touch = (par.preloadMode != Parameters::PRELOAD_MODE_MMAP);
    uint64_t candidateWorkingSet = 0;
    const auto addCandidateFile = [&candidateWorkingSet](
            const std::string& path) {
        const uint64_t bytes = FileUtil::getFileSize(path);
        candidateWorkingSet = bytes >
                std::numeric_limits<uint64_t>::max() - candidateWorkingSet
            ? std::numeric_limits<uint64_t>::max()
            : candidateWorkingSet + bytes;
    };
    for (const std::string& path : FileUtil::findDatafiles(prefDB.c_str())) {
        addCandidateFile(path);
    }
    addCandidateFile(prefDBIndex);
    useAAStoreTarget = aastoreTarget.openForDatabase(
        targetSeqDB, par.threads, candidateWorkingSet, par.aastore);
    aaStoreDirect = useAAStoreTarget && aastoreTarget.fullMemory();
    const unsigned int targetPreloadMode = useAAStoreTarget
        ? (touch ? IndexReader::PRELOAD_INDEX : IndexReader::PRELOAD_NO)
        : (touch ? (IndexReader::PRELOAD_INDEX | IndexReader::PRELOAD_DATA)
                 : IndexReader::PRELOAD_NO);
    const int targetDataMode = useAAStoreTarget
        ? DBReader<unsigned int>::USE_INDEX
        : (DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    tDbrIdx = new IndexReader(targetSeqDB, par.threads,
                              extended & Parameters::DBTYPE_EXTENDED_INDEX_NEED_SRC ? IndexReader::SRC_SEQUENCES : IndexReader::SEQUENCES,
                              targetPreloadMode, targetDataMode);
    tdbr = tDbrIdx->sequenceReader;
    targetSeqType = tdbr->getDbtype();
    sameQTDB = (targetSeqDB.compare(querySeqDB) == 0);
    if (sameQTDB == true) {
        qDbrIdx = tDbrIdx;
        qdbr = tdbr;
        querySeqType = targetSeqType;
    } else {
        // open the sequence, prefiltering and output databases
        qDbrIdx = new IndexReader(par.db1, par.threads,
                                  extended & Parameters::DBTYPE_EXTENDED_INDEX_NEED_SRC ? IndexReader::SRC_SEQUENCES : IndexReader::SEQUENCES,
                                  (touch) ? IndexReader::PRELOAD_INDEX : 0);
        qdbr = qDbrIdx->sequenceReader;
        querySeqType = qdbr->getDbtype();
    }

    if (altAlignment > 0) {
        if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
            Debug(Debug::ERROR) << "Alternative alignments are not supported for nucleotides.\n";
            EXIT(EXIT_FAILURE);
        }
//        if(realign==true){
//            Debug(Debug::ERROR) << "Alternative alignments do not supported realignment.\n";
//            EXIT(EXIT_FAILURE);
//        }
        alignmentMode = std::max(alignmentMode, (unsigned int)Parameters::ALIGNMENT_MODE_SCORE_COV);
    }
    swMode = initSWMode(lcaAlign ? lcaSwMode : alignmentMode, par.covThr, par.seqIdThr);
    // print out mode and check for errors
    switch (swMode) {
        case Matcher::SCORE_ONLY:
            Debug(Debug::INFO) << "Compute score only\n";
            break;
        case Matcher::SCORE_COV:
            Debug(Debug::INFO) << "Compute score and coverage\n";
            break;
        case Matcher::SCORE_COV_SEQID:
            Debug(Debug::INFO) << "Compute score, coverage and sequence identity\n";
            break;
        default:
            Debug(Debug::ERROR) << "Wrong swMode mode\n";
            EXIT(EXIT_FAILURE);
    }

    if (wrappedScoring) {
        maxSeqLen = maxSeqLen * 2;
        if (!Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
            Debug(Debug::ERROR) << "Wrapped scoring is only supported for nucleotides\n";
            EXIT(EXIT_FAILURE);
        }

        if (realign == true) {
            Debug(Debug::ERROR) << "Alternative alignments do not support wrapped scoring\n";
            EXIT(EXIT_FAILURE);
        }
    }

    //qdbr->readMmapedDataInMemory();
    // make sure to touch target after query, so if there is not enough memory for the query, at least the targets
    // might have had enough space left to be residung in the page cache
    if (sameQTDB == false && tDbrIdx == NULL && par.preloadMode != Parameters::PRELOAD_MODE_MMAP) {
        tdbr->readMmapedDataInMemory();
    }

    if (qdbr->getSize() <= threads) {
        threads = qdbr->getSize();
    }

    if (querySeqType == -1 || targetSeqType == -1) {
        Debug(Debug::ERROR) << "Please recreate your database or add a .dbtype file to your sequence/profile database.\n";
        EXIT(EXIT_FAILURE);
    }

    Debug(Debug::INFO) << "Query database size: "  << qdbr->getSize() << " type: " << Parameters::getDbTypeName(querySeqType) << "\n";
    Debug(Debug::INFO) << "Target database size: " << tdbr->getSize() << " type: " << Parameters::getDbTypeName(targetSeqType) << "\n";
    if (useAAStoreTarget) {
        if (!Parameters::isEqualDbtype(targetSeqType, Parameters::DBTYPE_AMINO_ACIDS)) {
            throw std::runtime_error("AAStore alignment targets must be amino-acid sequences");
        }
        bool needsKeyMap = tdbr->getSize() != aastoreTarget.size();
        for (size_t id = 0; !needsKeyMap && id < tdbr->getSize(); ++id) {
            needsKeyMap = tdbr->getDbKey(id) != id;
        }
        if (needsKeyMap) {
            std::vector<uint32_t> targetKeys(tdbr->getSize());
            for (size_t id = 0; id < targetKeys.size(); ++id) {
                targetKeys[id] = tdbr->getDbKey(id);
            }
            aastoreTarget.mapFromMMseqsKeys(targetKeys);
        }
    }
    aastoreTarget.validateSize(tdbr->getSize());

    prefdbr = new DBReader<unsigned int>(prefDB.c_str(), prefDBIndex.c_str(), threads, DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_INDEX);
    prefdbr->open(DBReader<unsigned int>::LINEAR_ACCCESS);
    reversePrefilterResult = Parameters::isEqualDbtype(prefdbr->getDbtype(), Parameters::DBTYPE_PREFILTER_REV_RES);

    correlationScoreWeight = par.correlationScoreWeight;
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
        m = new NucleotideMatrix(par.scoringMatrixFile.values.nucleotide().c_str(), 1.0, scoreBias);
        gapOpen = par.gapOpen.values.nucleotide();
        gapExtend = par.gapExtend.values.nucleotide();
        zdrop = par.zdrop;
    } else {
        // keep score bias at 0.0 (improved ROC)
        // this is where profile-profile alignment drops to
        m = new SubstitutionMatrix(par.scoringMatrixFile.values.aminoacid().c_str(), 2.0, scoreBias);
        gapOpen = par.gapOpen.values.aminoacid();
        gapExtend = par.gapExtend.values.aminoacid();
    }

    realign_m = NULL;
    if (realign == true && realignScoreBias != 0.0f) {
        if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
            realign_m = new NucleotideMatrix(par.scoringMatrixFile.values.nucleotide().c_str(), 1.0, scoreBias + realignScoreBias);
        } else {
            realign_m = new SubstitutionMatrix(par.scoringMatrixFile.values.aminoacid().c_str(), 2.0, scoreBias + realignScoreBias);
        }
    }
}

unsigned int Alignment::initSWMode(unsigned int alignmentMode, float covThr, float seqIdThr) {
    unsigned int swMode = Matcher::SCORE_ONLY;
    switch (alignmentMode) {
        case Parameters::ALIGNMENT_MODE_FAST_AUTO:
            if(covThr > 0.0 && seqIdThr == 0.0) {
                swMode = Matcher::SCORE_COV; // fast
            } else if(covThr > 0.0 && seqIdThr > 0.0) { // if seq id is needed
                swMode = Matcher::SCORE_COV_SEQID; // slowest
            } else {
                swMode = Matcher::SCORE_ONLY;
            }
            break;
        case Parameters::ALIGNMENT_MODE_SCORE_COV:
            swMode = Matcher::SCORE_COV; // fast
            break;
        case Parameters::ALIGNMENT_MODE_SCORE_COV_SEQID:
            swMode = Matcher::SCORE_COV_SEQID; // slowest
            break;
        default:
            swMode = Matcher::SCORE_ONLY;
            break;
    }

    return swMode;
}

bool Alignment::mapTargetSequence(
        Sequence& sequence, size_t dbId, unsigned int dbKey,
        unsigned int threadIdx, std::string& scratch,
        const AAStoreTargetReader::Batch* batch) {
    size_t length = 0;
    char* data = getTargetSequenceData(dbId, threadIdx, length, scratch, batch);
    if (data == NULL) return false;
    sequence.mapSequence(dbId, dbKey, data, length);
    return true;
}

char* Alignment::getTargetSequenceData(
        size_t dbId, unsigned int threadIdx, size_t& seqLen,
        std::string& scratch,
        const AAStoreTargetReader::Batch* batch) {
    if (dbId >= tdbr->getSize()) return NULL;
    if (!useAAStoreTarget) {
        seqLen = tdbr->getSeqLen(dbId);
        return tdbr->getData(dbId, threadIdx);
    }
    if (aaStoreDirect) {
        try {
            if (aastoreTarget.sequenceLength(dbId) >
                static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                throw std::length_error("AAStore sequence exceeds alignment length range");
            }
            aastoreTarget.get(dbId, scratch);
        } catch (const std::exception& error) {
            Debug(Debug::ERROR) << error.what() << "\n";
            EXIT(EXIT_FAILURE);
        }
        seqLen = scratch.size();
        return scratch.data();
    }
    std::string_view sequence;
    if (batch == NULL || !batch->view(dbId, sequence)) {
        Debug(Debug::ERROR) << "Target sequence is missing from AAStore batch\n";
        EXIT(EXIT_FAILURE);
    }
    seqLen = sequence.size();
    return const_cast<char*>(sequence.data());
}

Alignment::~Alignment() {
    if (realign_m != NULL) {
        delete realign_m;
    }
    delete m;

    if (tDbrIdx != NULL) {
        delete tDbrIdx;
    }else{
        tdbr->close();
        delete tdbr;
    }

    if (sameQTDB == false) {
        if(qDbrIdx != NULL){
            delete qDbrIdx;
        }else{
            qdbr->close();
            delete qdbr;
        }
    }

    prefdbr->close();
    delete prefdbr;
}

void Alignment::run(const unsigned int mpiRank, const unsigned int mpiNumProc) {

    size_t dbFrom = 0;
    size_t dbSize = 0;
    prefdbr->decomposeDomainByAminoAcid(mpiRank, mpiNumProc, &dbFrom, &dbSize);

    Debug(Debug::INFO) << "Compute split from " << dbFrom << " to " << (dbFrom + dbSize) << "\n";
    std::pair<std::string, std::string> tmpOutput = Util::createTmpFileNames(outDB, outDBIndex, mpiRank);
    run(tmpOutput.first, tmpOutput.second, dbFrom, dbSize, true);

#ifdef HAVE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    if (MMseqsMPI::isMaster()) {
        std::vector<std::pair<std::string, std::string> > splitFiles;
        for (unsigned int proc = 0; proc < mpiNumProc; proc++) {
            splitFiles.push_back(Util::createTmpFileNames(outDB, outDBIndex, proc));
        }

        // merge output databases
        DBWriter::mergeResults(outDB, outDBIndex, splitFiles);
    }
}

void Alignment::run() {
    run(outDB, outDBIndex, 0, prefdbr->getSize(), false);
}

void Alignment::run(const std::string &outDB, const std::string &outDBIndex, const size_t dbFrom, const size_t dbSize, bool merge) {
    int dbtype = Parameters::DBTYPE_ALIGNMENT_RES;
    if (alignmentOutputMode == Parameters::ALIGNMENT_OUTPUT_CLUSTER) {
        dbtype = Parameters::DBTYPE_CLUSTER_RES;
    }
    dbtype = DBReader<unsigned int>::setExtendedDbtype(dbtype, DBReader<unsigned int>::getExtendedDbtype(prefdbr->getDbtype()));
    DBWriter dbw(outDB.c_str(), outDBIndex.c_str(), threads, compressed, dbtype);
    dbw.open();

    // handle no alignment case early, below would divide by 0 otherwise
    if (dbSize == 0) {
        dbw.close(merge);
        return;
    }

    const size_t targetResidues = useAAStoreTarget
        ? static_cast<size_t>(aastoreTarget.totalLength())
        : tdbr->getAminoAcidDBSize();
    EvalueComputation evaluer(targetResidues, this->m, gapOpen, gapExtend);

    size_t totalMemory = Util::getTotalSystemMemory();
    const bool useAAStoreThreadBatch = useAAStoreTarget && !aaStoreDirect;
    constexpr size_t queryScheduleChunk = 5;
    const size_t threadBytes = useAAStoreThreadBatch
        ? AAStoreTargetReader::kThreadBatchBytes : 0;
    const size_t threadQueueDepth = useAAStoreThreadBatch
        ? AAStoreTargetReader::threadBatchQueueDepth(threads) : 0;
    size_t flushSize = 1000000;
    if (totalMemory > prefdbr->getTotalDataSize()) {
        flushSize = dbSize;
    }

    size_t alignmentsNum = 0;
    size_t totalPassedNum = 0;

    if (aaStoreDirect) {
        Debug(Debug::INFO) << "AAStore alignment: full-memory thread-local get, threads="
                           << threads << " query=" << (sameQTDB ? "aastore" : "native")
                           << " target=aastore decode=ascii\n";
    } else if (useAAStoreThreadBatch) {
        Debug(Debug::INFO) << "AAStore alignment: dynamic thread-local batch_get_into, "
                           << "candidate_bytes_per_batch=" << threadBytes
                           << " aastore_threads=1 compute_threads=" << threads
                           << " io_queue_budget=" << AAStoreTargetReader::kThreadBatchIoBudget
                           << " io_queue_depth_per_thread=" << threadQueueDepth
                           << " query=" << (sameQTDB ? "aastore" : "native")
                           << " target=aastore\n";
    }
    size_t processed = 0;
    size_t threadBatchCount = 0;
    std::vector<aastore::BatchStats> threadIoStats(useAAStoreThreadBatch ? threads : 0);
    while (processed < dbSize) {
        const size_t start = dbFrom + processed;
        const size_t bucketSize = std::min(dbSize - processed, flushSize);
        const bool hasMore = processed + bucketSize < dbSize;
        Debug::Progress progress(bucketSize);
        // Let idle workers claim complete queries by candidate bytes. A fixed
        // query chunk can strand most threads behind one unusually heavy chunk.
        size_t nextThreadBatchQuery = start;
        std::mutex threadBatchCursorMutex;

        auto collectThreadBatch = [&](AAStoreTargetReader::ThreadBatch& batch,
                                      size_t begin,
                                      size_t limit,
                                      unsigned readerThread) {
            batch.ids.clear();
            batch.begin = begin;
            batch.end = begin;
            size_t bytes = 0;
            char keyBuffer[1024];
            for (size_t query = begin; query < limit; ++query) {
                char* candidates = prefdbr->getData(query, readerThread);
                if (sameQTDB && *candidates != '\0') {
                    batch.ids.push_back(
                        qdbr->getId(prefdbr->getDbKey(query)));
                }
                while (*candidates != '\0') {
                    Util::parseKey(candidates, keyBuffer);
                    const size_t targetId = tdbr->getId(
                        Util::fast_atoi<unsigned int>(keyBuffer));
                    batch.ids.push_back(targetId);
                    const uint64_t length =
                        aastoreTarget.sequenceLength(targetId);
                    bytes += static_cast<size_t>(std::min<uint64_t>(
                        length + 1,
                        std::numeric_limits<size_t>::max() - bytes));
                    candidates = Util::skipLine(candidates);
                }
                batch.end = query + 1;
                if (bytes >= threadBytes) break;
            }
        };

#pragma omp parallel num_threads(threads) reduction(+: threadBatchCount, alignmentsNum, totalPassedNum)
        {
            unsigned int thread_idx = 0;
#ifdef OPENMP
            thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif
            std::string alnResultsOutString;
            alnResultsOutString.reserve(1024*1024);
            char buffer[1024 + 32768*4];
            Sequence qSeq(maxSeqLen, querySeqType, m, 0, false, compBiasCorrection);
            Sequence dbSeq(maxSeqLen, targetSeqType, m, 0, false, compBiasCorrection);
            const size_t maxMatcherSeqLen = Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)
                                            ? maxSeqLen : std::max(tdbr->getMaxSeqLen(), qdbr->getMaxSeqLen());

            std::vector<Matcher::result_t> swResults;
            swResults.reserve(300);
            Matcher matcher(querySeqType, targetSeqType, maxMatcherSeqLen, m, &evaluer, compBiasCorrection, compBiasCorrectionScale, gapOpen, gapExtend, correlationScoreWeight, zdrop);

            std::vector<Matcher::result_t> swRealignResults;
            Matcher *realigner = NULL;
            if (realign == true) {
                swRealignResults.reserve(300);
                realigner = &matcher;
                if (realign_m != NULL) {
                    realigner = new Matcher(querySeqType, targetSeqType, maxMatcherSeqLen, realign_m, &evaluer, compBiasCorrection, compBiasCorrectionScale, gapOpen, gapExtend, 0.0, zdrop);
                }
            }

            std::string queryToWrap;
            queryToWrap.reserve(maxSeqLen * 2);
            std::string queryBuffer;
            std::string targetBuffer;
            if (aaStoreDirect) {
                queryBuffer.reserve(32768);
                targetBuffer.reserve(32768);
            }

            const char* words[10];
            AAStoreTargetReader::ThreadBatch threadBatch;
            const AAStoreTargetReader::Batch* workerBatch = NULL;

            auto alignQuery = [&](size_t id) {
                progress.updateProgress();

                // get the prefiltering list
                char *data, *origData;
                data = origData = prefdbr->getData(id, thread_idx);
                unsigned int queryDbKey = prefdbr->getDbKey(id);
                size_t origQueryLen = 0;
                // only load query data if data != \0
                if (*data != '\0') {
                    size_t qId = qdbr->getId(queryDbKey);
                    size_t queryLen = 0;
                    char *querySeqData = NULL;
                    if (useAAStoreTarget && sameQTDB) {
                        querySeqData = getTargetSequenceData(
                            qId, thread_idx, queryLen, queryBuffer, workerBatch);
                    } else if (qId != UINT_MAX) {
                        querySeqData = qdbr->getData(qId, thread_idx);
                        queryLen = qdbr->getSeqLen(qId);
                    }
                    if (querySeqData == NULL) {
                        Debug(Debug::ERROR) << "Query sequence " << queryDbKey
                                            << " is required in the prefiltering, but is not contained in the query sequence database.\nPlease check your database.\n";
                        EXIT(EXIT_FAILURE);
                    }
                    origQueryLen = queryLen;
                    if (wrappedScoring) {
                        queryToWrap = std::string(querySeqData, queryLen);
                        queryToWrap = queryToWrap + queryToWrap;
                        querySeqData = (char*)(queryToWrap).c_str();
                        queryLen = origQueryLen*2;
                    }

                    qSeq.mapSequence(qId, queryDbKey, querySeqData, queryLen);
                    matcher.initQuery(&qSeq);
                }

                // parse the prefiltering list and calculate a Smith-Waterman alignment for each sequence in the list
                size_t passedNum = 0;
                unsigned int rejected = 0;
                while (*data != '\0' && passedNum < maxAccept && rejected < maxReject) {
                    Util::parseKey(data, buffer);
                    const unsigned int dbKey = (unsigned int) strtoul(buffer, NULL, 10);
                    size_t elements = Util::getWordsOfLine(data, words, 10);

                    short diagonal = 0;
                    bool isReverse = false;
                    // Prefilter result (need to make this better)
                    if (elements == 3) {
                        hit_t hit = QueryMatcher::parsePrefilterHit(data);
                        isReverse = reversePrefilterResult && (hit.prefScore < 0);
                        diagonal = static_cast<short>(hit.diagonal);
                    }
                    data = Util::skipLine(data);

                    size_t dbId = tdbr->getId(dbKey);
                    if (!mapTargetSequence(dbSeq, dbId, dbKey, thread_idx,
                                           targetBuffer, workerBatch)) {
                        Debug(Debug::ERROR) << "Sequence " << dbKey << " is required in the prefiltering, but is not contained in the target sequence database!\nPlease check your database.\n";
                        EXIT(EXIT_FAILURE);
                    }

                    // check if the sequences could pass the coverage threshold
                    if (Util::canBeCovered(canCovThr, covMode, static_cast<float>(origQueryLen), static_cast<float>(dbSeq.L)) == false) {
                        rejected++;
                        continue;
                    }

                    const bool isIdentity = (queryDbKey == dbKey && (includeIdentity || sameQTDB)) ? true : false;

                    // calculate Smith-Waterman alignment

                    Matcher::result_t res = matcher.getSWResult(&dbSeq, static_cast<int>(diagonal), isReverse, covMode, covThr, evalThr, swMode, seqIdMode, isIdentity, wrappedScoring);
                    alignmentsNum++;

                    if (isIdentity) {
                        // set coverage and seqid of identity
                        res.qcov = 1.0f;
                        res.dbcov = 1.0f;
                        res.seqId = 1.0f;
                    }

                    if (checkCriteria(res, isIdentity, evalThr, seqIdThr, alnLenThr, covMode, covThr)) {
                        swResults.emplace_back(res);
                        passedNum++;
                        totalPassedNum++;
                        rejected = 0;
                    } else {
                        rejected++;
                    }
                }

                if (altAlignment > 0 && realign == false && wrappedScoring == false) {
                    computeAlternativeAlignment(queryDbKey, dbSeq, swResults, matcher, covThr, evalThr, swMode, thread_idx,
                                                targetBuffer, workerBatch);
                }

                if (swResults.size() > 1) {
                    SORT_SERIAL(swResults.begin(), swResults.end(), Matcher::compareHits);
                }

                std::vector<Matcher::result_t> *returnRes = &swResults;
                if (realign == true && *origData != '\0') {
                    realigner->initQuery(&qSeq);
                    int realignAccepted = 0;
                    for (size_t result = 0; result < swResults.size() && realignAccepted < realignMaxSeqs; result++) {
                        size_t dbId = tdbr->getId(swResults[result].dbKey);
                        if (!mapTargetSequence(dbSeq, dbId, swResults[result].dbKey,
                                               thread_idx, targetBuffer, workerBatch)) {
                            Debug(Debug::ERROR) << "Sequence " << swResults[result].dbKey <<" is required in the prefiltering, but is not contained in the target sequence database!\nPlease check your database.\n";
                            EXIT(EXIT_FAILURE);
                        }

                        // recompute alignment boundaries (without changing evalue)
                        const bool isIdentity = (queryDbKey == swResults[result].dbKey && (includeIdentity || sameQTDB)) ? true : false;
                        Matcher::result_t res = realigner->getSWResult(&dbSeq, INT_MAX, false, covMode, realignCov, FLT_MAX, realignSwMode, seqIdMode, isIdentity);

                        const bool covOK = Util::hasCoverage(realignCov, covMode, res.qcov, res.dbcov);
                        if (covOK == true || isIdentity) {
                            res.score = swResults[result].score;
                            res.eval  = swResults[result].eval;
                            swRealignResults.emplace_back(res);
                            realignAccepted++;
                        }
                    }

                    if (altAlignment > 0) {
                        computeAlternativeAlignment(queryDbKey, dbSeq, swRealignResults, *realigner, realignCov, FLT_MAX, realignSwMode, thread_idx,
                                                    targetBuffer, workerBatch);
                    }

                    if (swRealignResults.size() > 1) {
                        SORT_SERIAL(swRealignResults.begin(), swRealignResults.end(), Matcher::compareHits);
                    }

                    returnRes = &swRealignResults;
                }

                if (lcaAlign == true && swRealignResults.size() > 0) {
                    Matcher::result_t& topHit = swRealignResults[0];
                    const unsigned int topHitKey = topHit.dbKey;
                    size_t dbId = tdbr->getId(topHitKey);
                    size_t qSeqLen = 0;
                    char *qSeqData = getTargetSequenceData(
                        dbId, thread_idx, qSeqLen, queryBuffer, workerBatch);
                    if (qSeqData == NULL) {
                        Debug(Debug::ERROR) << "Sequence " << topHitKey << " is required in the prefiltering, but is not contained in the target sequence database!\nPlease check your database.\n";
                        EXIT(EXIT_FAILURE);
                    }
                    qSeq.mapSequence(dbId, topHitKey, qSeqData + topHit.dbStartPos, topHit.dbEndPos - topHit.dbStartPos + 1);
                    realigner->initQuery(&qSeq);

                    const double topHitEval = topHit.eval;
                    swRealignResults.clear();

                    data = origData;
                    unsigned int rejected = 0;
                    while (*data != '\0' && rejected < maxReject) {
                        Util::parseKey(data, buffer);
                        const unsigned int dbKey = (unsigned int) strtoul(buffer, NULL, 10);
//                        size_t elements = Util::getWordsOfLine(data, words, 10);
//                        short diagonal = 0;
//                        bool isReverse = false;
//                        // Prefilter result (need to make this better)
//                        if (elements == 3) {
//                            hit_t hit = QueryMatcher::parsePrefilterHit(data);
//                            isReverse = reversePrefilterResult && (hit.prefScore < 0);
//                            diagonal = static_cast<short>(hit.diagonal);
//                        }
                        data = Util::skipLine(data);

                        dbId = tdbr->getId(dbKey);
                        if (!mapTargetSequence(dbSeq, dbId, dbKey, thread_idx,
                                               targetBuffer, workerBatch)) {
                            Debug(Debug::ERROR) << "Sequence " << dbKey << " is required in the prefiltering, but is not contained in the target sequence database!\nPlease check your database.\n";
                            EXIT(EXIT_FAILURE);
                        }

                        Matcher::result_t res = realigner->getSWResult(&dbSeq, INT_MAX, false, covMode, realignCov, topHitEval, lcaSwMode, seqIdMode, false);

                        if (checkCriteria(res, false, topHitEval, seqIdThr, alnLenThr, covMode, realignCov)) {
                            swRealignResults.emplace_back(res);
                            rejected = 0;
                        } else {
                            rejected++;
                        }
                    }

                    if (swRealignResults.size() > 1) {
                        SORT_SERIAL(swRealignResults.begin(), swRealignResults.end(), Matcher::compareHits);
                    }

                    returnRes = &swRealignResults;
                }
                if(alignmentOutputMode == Parameters::ALIGNMENT_OUTPUT_CLUSTER) {
                    for (size_t result = 0; result < returnRes->size(); result++) {
                        alnResultsOutString.append(SSTR((*returnRes)[result].dbKey));
                        alnResultsOutString.push_back('\n');
                    }
                }else{
                    for (size_t result = 0; result < returnRes->size(); result++) {
                        size_t len = Matcher::resultToBuffer(buffer, (*returnRes)[result], addBacktrace);
                        alnResultsOutString.append(buffer, len);
                    }
                }
                dbw.writeData(alnResultsOutString.c_str(), alnResultsOutString.length(), queryDbKey, thread_idx);
                alnResultsOutString.clear();
                swResults.clear();
                swRealignResults.clear();
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
                            collectThreadBatch(
                                threadBatch, nextThreadBatchQuery,
                                start + bucketSize, thread_idx);
                            nextThreadBatchQuery = threadBatch.end;
                        }
                        aastoreTarget.fetch_into(threadBatch, threads);
                        ++threadBatchCount;
                        AAStoreTargetReader::accumulateIoStats(
                            threadIoStats[thread_idx],
                            threadBatch.sequences.fetched.stats);
                        workerBatch = &threadBatch.sequences;
                        for (size_t id = threadBatch.begin;
                             id < threadBatch.end; ++id) {
                            alignQuery(id);
                        }
                    } catch (const std::exception& error) {
                        Debug(Debug::ERROR) << error.what() << "\n";
                        EXIT(EXIT_FAILURE);
                    }
                }
            } else {
#pragma omp for schedule(dynamic, queryScheduleChunk)
                for (size_t id = start; id < (start + bucketSize); ++id) {
                    alignQuery(id);
                }
            }
            if (realigner != NULL && realigner != &matcher) {
                delete realigner;
            }
            if (hasMore) {
#pragma omp barrier
                if (thread_idx == 0) {
                    prefdbr->remapData();
                }
#pragma omp barrier
            }
        }
        processed += bucketSize;
    }
    dbw.close(merge);

    if (useAAStoreThreadBatch) {
        Debug(Debug::INFO) << "AAStore alignment thread batches=" << threadBatchCount << "\n";
        AAStoreTargetReader::logIoStats(threadIoStats);
    }
    Debug(Debug::INFO) << alignmentsNum << " alignments calculated\n";
    Debug(Debug::INFO) << totalPassedNum << " sequence pairs passed the thresholds";
    if (alignmentsNum > 0) {
        Debug(Debug::INFO) << " (" << ((float) totalPassedNum / (float) alignmentsNum) << " of overall calculated)";
    }
    Debug(Debug::INFO) << "\n";
    if (dbSize > 0) {
        size_t hits = totalPassedNum / dbSize;
        size_t hits_rest = totalPassedNum % dbSize;
        float hits_f = ((float) hits) + ((float) hits_rest) / (float) dbSize;
        Debug(Debug::INFO) << hits_f << " hits per query sequence\n";
    }
}

size_t Alignment::estimateHDDMemoryConsumption(int dbSize, int maxSeqs) {
    return 2 * (dbSize * maxSeqs * 21 * 1.75);
}

bool Alignment::checkCriteria(Matcher::result_t &res, bool isIdentity, double evalThr, double seqIdThr, int alnLenThr, int covMode, float covThr) {
    const bool evalOk = (res.eval <= evalThr); // -e
    const bool seqIdOK = (res.seqId >= seqIdThr); // --min-seq-id
    const bool covOK = Util::hasCoverage(covThr, covMode, res.qcov, res.dbcov);
    const bool alnLenOK = Util::hasAlignmentLength(alnLenThr, res.alnLength);

    // check first if it is identity
    if (isIdentity
        ||
        // general accaptance criteria
        ( evalOk   &&
          seqIdOK  &&
          covOK    &&
          alnLenOK
        )) {
        return true;
    } else {
        return false;
    }
}

void Alignment::computeAlternativeAlignment(unsigned int queryDbKey, Sequence &dbSeq, std::vector<Matcher::result_t> &swResults,
                                            Matcher &matcher, float covThr, float evalThr, int swMode, int thread_idx,
                                            std::string& scratch, const AAStoreTargetReader::Batch* batch) {
    const unsigned char xIndex = m->aa2num[static_cast<int>('X')];
    const size_t firstItResSize = swResults.size();
    for (size_t i = 0; i < firstItResSize; i++) {
        const bool isIdentity = (queryDbKey == swResults[i].dbKey && (includeIdentity || sameQTDB)) ? true : false;
        if (isIdentity == true) {
            continue;
        }
        size_t dbId = tdbr->getId(swResults[i].dbKey);
        if (!mapTargetSequence(dbSeq, dbId, swResults[i].dbKey, thread_idx,
                               scratch, batch)) {
            Debug(Debug::ERROR) << "Sequence " << swResults[i].dbKey << " is required in the prefiltering, but is not contained in the target sequence database!\nPlease check your database.\n";
            EXIT(EXIT_FAILURE);
        }

        for (int pos = swResults[i].dbStartPos; pos < swResults[i].dbEndPos; ++pos) {
            dbSeq.numSequence[pos] = xIndex;
        }
        bool nextAlignment = true;
        for (int altAli = 0; altAli < altAlignment && nextAlignment; altAli++) {
            Matcher::result_t res = matcher.getSWResult(&dbSeq, INT_MAX, false, covMode, covThr, evalThr, swMode, seqIdMode, isIdentity);
            nextAlignment = checkCriteria(res, isIdentity, evalThr, seqIdThr, alnLenThr, covMode, covThr);
            if (nextAlignment == true) {
                swResults.emplace_back(res);
                for (int pos = res.dbStartPos; pos < res.dbEndPos; pos++) {
                    dbSeq.numSequence[pos] = xIndex;
                }
            }
        }
    }
}
