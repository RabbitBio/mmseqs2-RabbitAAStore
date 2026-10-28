#include "Parameters.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "Util.h"
#include "IndexReader.h"
#include "FileUtil.h"
#include "AAStoreTargetReader.h"
#include "AAStoreRecordHeader.h"
#include "TsvRowFormatter.h"

#include <memory>

#ifdef OPENMP
#include <omp.h>

#endif

#ifndef SIZE_T_MAX
#define SIZE_T_MAX ((size_t) -1)
#endif

int createtsv(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, Parameters::PARSE_VARIADIC, 0);

    const bool hasTargetDB = par.filenames.size() > 3;
    DBReader<unsigned int> *reader;
    if (hasTargetDB) {
        reader = new DBReader<unsigned int>(par.db3.c_str(), par.db3Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    } else {
        reader = new DBReader<unsigned int>(par.db2.c_str(), par.db2Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    }
    reader->open(DBReader<unsigned int>::LINEAR_ACCCESS);

    AAStoreTargetReader aaHeaders;
    const bool useAAStoreHeaders = par.aastore && (!hasTargetDB || par.db1 == par.db2) &&
        AAStoreTargetReader::hasNativeRecordHeaders(par.db1) &&
        aaHeaders.openForHeaders(par.db1, par.threads, reader->getTotalDataSize(), par.fullHeader);
    if (useAAStoreHeaders) {
        Debug(Debug::INFO) << "RabbitAAStore createtsv: fields="
            << (aaHeaders.headerDescriptionsEnabled() ? "name,description" : "name")
            << " native_header_db=0 threads=" << par.threads
            << " packed_resident_bytes=0 packed_preload_seconds=0\n";
    }

    bool queryNucs = Parameters::isEqualDbtype(FileUtil::parseDbType(par.db1.c_str()), Parameters::DBTYPE_NUCLEOTIDES);
    bool targetNucs = Parameters::isEqualDbtype(FileUtil::parseDbType(par.db2.c_str()), Parameters::DBTYPE_NUCLEOTIDES);
    const bool touch = (par.preloadMode != Parameters::PRELOAD_MODE_MMAP);
    int queryHeaderType = (queryNucs) ? IndexReader::SRC_HEADERS : IndexReader::HEADERS;
    queryHeaderType = (par.idxSeqSrc == 0) ? queryHeaderType :  (par.idxSeqSrc == 1) ?  IndexReader::HEADERS : IndexReader::SRC_HEADERS;
    std::unique_ptr<IndexReader> qDbrHeader;
    if (!useAAStoreHeaders) {
        qDbrHeader.reset(new IndexReader(par.db1, par.threads, queryHeaderType, (touch) ? (IndexReader::PRELOAD_INDEX | IndexReader::PRELOAD_DATA) : 0));
    }
    IndexReader * tDbrHeader=NULL;
    DBReader<unsigned int> * queryDB = qDbrHeader ? qDbrHeader->sequenceReader : NULL;
    DBReader<unsigned int> * targetDB = NULL;
    bool sameDB = (par.db2.compare(par.db1) == 0);
    DBReader<unsigned int>::Index * qHeaderIndex = queryDB ? queryDB->getIndex() : NULL;
    DBReader<unsigned int>::Index * tHeaderIndex = NULL;

    if (hasTargetDB && !useAAStoreHeaders) {
        if (sameDB) {
            tDbrHeader = qDbrHeader.get();
            tHeaderIndex = qHeaderIndex;
            targetDB = queryDB;
        } else {

            int targetHeaderType = (targetNucs) ? IndexReader::SRC_HEADERS : IndexReader::HEADERS;
            targetHeaderType = (par.idxSeqSrc == 0) ? targetHeaderType :  (par.idxSeqSrc == 1) ?  IndexReader::HEADERS : IndexReader::SRC_HEADERS;

            tDbrHeader = new IndexReader(par.db2, par.threads, targetHeaderType, touch);
            tHeaderIndex = tDbrHeader->sequenceReader->getIndex();
            targetDB = tDbrHeader->sequenceReader;
        }
    }

    uint16_t extended = DBReader<unsigned int>::getExtendedDbtype(reader->getDbtype());
    bool needSET = false;
    std::map<unsigned int, std::string> qSetToSource, tSetToSource;
    if (extended & Parameters::DBTYPE_EXTENDED_SET) {
        needSET = true;
        qSetToSource = Util::readLookup((par.db1 + ".source"), false);
        tSetToSource = Util::readLookup((par.db2 + ".source"), false);
    }

    const std::string& dataFile = hasTargetDB ? par.db4 : par.db3;
    const std::string& indexFile = hasTargetDB ? par.db4Index : par.db3Index;
    const bool shouldCompress = par.dbOut == true && par.compressed == true;
    const int dbType = par.dbOut == true ? Parameters::DBTYPE_GENERIC_DB : Parameters::DBTYPE_OMIT_FILE;
    DBWriter writer(dataFile.c_str(), indexFile.c_str(), par.threads, shouldCompress, dbType);
    writer.open();

    const size_t targetColumn = (par.targetTsvColumn == 0) ? SIZE_T_MAX :  par.targetTsvColumn - 1;
#pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif

        const char *columnPointer[255];
        char *dbKey = new char[par.maxSeqLen + 1];

        std::string outputBuffer;
        outputBuffer.reserve(10 * 1024);
        std::string name, description;
        const auto aaLabel = [&](unsigned int key) {
            aaHeaders.getHeaderFields(key, name, description);
            if (!par.fullHeader && !aaHeaders.headerDescriptionsEnabled()) {
                return Util::parseFastaHeader(name.c_str());
            }
            return aastore_export::tsvLabel(name, description,
                aaHeaders.headerSeparator(description), par.fullHeader);
        };

#pragma omp for schedule(dynamic, 1000)
        for (size_t i = 0; i < reader->getSize(); ++i) {
            unsigned int queryKey = reader->getDbKey(i);
            std::string queryHeader;
            if (useAAStoreHeaders) {
                queryHeader = needSET ? qSetToSource[queryKey] : aaLabel(queryKey);
            } else {
                size_t queryIndex = queryDB->getId(queryKey);
                char *headerData = queryDB->getData(queryIndex, thread_idx);
                if (headerData == NULL) {
                    Debug(Debug::WARNING) << "Invalid header entry in query " << queryKey << "!\n";
                    continue;
                }
                if (needSET == true) {
                    queryHeader = qSetToSource[queryKey];
                } else if (par.fullHeader) {
                    queryHeader = "\"";
                    queryHeader.append(headerData, qHeaderIndex[queryIndex].length - 2);
                    queryHeader.append("\"");
                } else {
                    queryHeader = Util::parseFastaHeader(headerData);
                }
            }

            TsvRowFormatter formatter(targetColumn, par.firstSeqRepr);
            formatter.begin(std::move(queryHeader));

            char *data = reader->getData(i, thread_idx);
            while (*data != '\0') {
                if(targetColumn != SIZE_T_MAX){
                    size_t foundElements = Util::getWordsOfLine(data, columnPointer, 255);
                    if (foundElements < targetColumn) {
                        Debug(Debug::WARNING) << "Not enough columns!" << "\n";
                        continue;
                    }
                    Util::parseKey(columnPointer[targetColumn], dbKey);
                }
                std::string targetAccession;
                if(targetColumn == SIZE_T_MAX){
                    targetAccession = "";
                } else if (hasTargetDB) {
                    unsigned int targetKey = (unsigned int) strtoul(dbKey, NULL, 10);
                    if (useAAStoreHeaders) {
                        targetAccession = needSET ? tSetToSource[targetKey] : aaLabel(targetKey);
                    } else {
                        size_t targetIndex = targetDB->getId(targetKey);
                        char *targetData = targetDB->getData(targetIndex, thread_idx);
                        if (targetData == NULL) {
                            Debug(Debug::WARNING) << "Invalid header entry in query " << queryKey << " and target " << targetKey << "!\n";
                            continue;
                        }
                        if(needSET == true) {
                            targetAccession = tSetToSource[targetKey];
                        } else if (par.fullHeader) {
                            targetAccession = "\"";
                            targetAccession.append(targetData, tHeaderIndex[targetIndex].length - 2);
                            targetAccession.append("\"");
                        } else {
                            targetAccession = Util::parseFastaHeader(targetData);
                        }
                    }
                } else {
                    targetAccession = dbKey;
                }

                char *nextLine = Util::skipLine(data);
                formatter.append(outputBuffer, targetAccession,
                                 std::string_view(data, nextLine - data - 1),
                                 targetColumn == 0 ? strlen(dbKey) : 0);
                data = nextLine;
            }
            writer.writeData(outputBuffer.c_str(), outputBuffer.length(), queryKey, thread_idx, par.dbOut);
            outputBuffer.clear();
        }
        delete[] dbKey;
    }
    writer.close(par.dbOut == false);

    if (par.dbOut == false) {
        if (hasTargetDB) {
            FileUtil::remove(par.db4Index.c_str());
        } else {
            FileUtil::remove(par.db3Index.c_str());
        }
    }

    reader->close();
    delete reader;
    if (hasTargetDB) {
        if (sameDB == false) {
            delete tDbrHeader;
        }
    }
    qSetToSource.clear();
    tSetToSource.clear();

    return EXIT_SUCCESS;
}
#undef SIZE_T_MAX
