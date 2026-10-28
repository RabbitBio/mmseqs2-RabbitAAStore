#include "Parameters.h"
#include "FileUtil.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "Util.h"
#include "AAStoreTargetReader.h"

#include <climits>
#include <exception>
#include <filesystem>
#include <utility>
#include <vector>

int createsubdb(int argc, const char **argv, const Command& command) {
    Parameters& par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, 0, 0);

    FILE *orderFile = NULL;
    if (FileUtil::fileExists(par.db1Index.c_str())) {
        orderFile = fopen(par.db1Index.c_str(), "r");
    } else {
        if(FileUtil::fileExists(par.db1.c_str())){
            orderFile = fopen(par.db1.c_str(), "r");
        }else{
            Debug(Debug::ERROR) << "File " << par.db1 << " does not exist.\n";
            EXIT(EXIT_FAILURE);
        }
    }

    const bool lookupMode = par.dbIdMode == Parameters::ID_MODE_LOOKUP;
    std::string aaStorePath;
    try {
        aaStorePath = AAStoreTargetReader::associatedStorePath(par.db2);
    } catch (const std::exception& error) {
        // 关联只用于传递元数据，损坏的关联不应阻止原生子库创建。
        Debug(Debug::WARNING) << "Skipping RabbitAAStore association: " << error.what() << "\n";
    }
    int dbMode = DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA;
    if (lookupMode) {
        dbMode |= DBReader<unsigned int>::USE_LOOKUP_REV;
    }
    DBReader<unsigned int> reader(par.db2.c_str(), par.db2Index.c_str(), 1, dbMode);
    reader.open(DBReader<unsigned int>::NOSORT);
    const bool isCompressed = reader.isCompressed();
    const int sourceDbtype = reader.getDbtype();

    if (aaStorePath.empty()) {
        try {
            if (std::filesystem::exists(par.db3 + ".aastore")) {
                Debug(Debug::ERROR) << "Output retains a RabbitAAStore directory: " << par.db3
                    << ".aastore; use a fresh output path\n";
                EXIT(EXIT_FAILURE);
            }
            std::filesystem::remove(par.db3 + ".aastore.path");
            std::filesystem::remove(par.db3 + ".aastore.ids");
        } catch (const std::exception& error) {
            Debug(Debug::ERROR) << "Cannot clear stale RabbitAAStore output association: " << error.what() << "\n";
            EXIT(EXIT_FAILURE);
        }
    }
    DBWriter writer(par.db3.c_str(), par.db3Index.c_str(), 1, 0, Parameters::DBTYPE_OMIT_FILE);
    writer.open();
    // getline reallocs automatic
    char *line = NULL;
    size_t len = 0;
    char dbKey[256];
    unsigned int prevKey = 0;
    bool isOrdered = true;
    std::vector<uint32_t> aaStoreMembers;
    while (getline(&line, &len, orderFile) != -1) {
        Util::parseKey(line, dbKey);
        unsigned int key;
        if (lookupMode) {
            size_t lookupId = reader.getLookupIdByAccession(dbKey);
            if (lookupId == SIZE_MAX) {
                Debug(Debug::WARNING) << "Could not find name " << dbKey << " in lookup\n";
                continue;
            }
            key = reader.getLookupKey(lookupId);
        } else {
            key = Util::fast_atoi<unsigned int>(dbKey);
        }

        isOrdered &= (prevKey <= key);
        prevKey = key;
        const size_t id = reader.getId(key);
        if (id >= UINT_MAX) {
            Debug(Debug::WARNING) << "Key " << dbKey << " not found in database\n";
            continue;
        }
        if (!aaStorePath.empty()) aaStoreMembers.push_back(key);
        if (par.subDbMode == Parameters::SUBDB_MODE_SOFT) {
            writer.writeIndexEntry(key, reader.getOffset(id), reader.getEntryLen(id), 0);
        } else {
            char* data = reader.getDataUncompressed(id);
            size_t originalLength = reader.getEntryLen(id);
            size_t entryLength = std::max(originalLength, static_cast<size_t>(1)) - 1;

            if (isCompressed) {
                // copy also the null byte since it contains the information if compressed or not
                entryLength = *(reinterpret_cast<unsigned int *>(data)) + sizeof(unsigned int) + 1;
                writer.writeData(data, entryLength, key, 0, false, false);
            } else {
                writer.writeData(data, entryLength, key, 0, true, false);
            }
            // do not write null byte since
            writer.writeIndexEntry(key, writer.getStart(0), originalLength, 0);
        }
    }
    // All source records are copied or indexed now. Release the native index
    // before sorting output or opening the shared RabbitAAStore metadata.
    reader.close();
    // merge any kind of sequence database
    const bool shouldMerge = Parameters::isEqualDbtype(sourceDbtype, Parameters::DBTYPE_HMM_PROFILE)
                             || Parameters::isEqualDbtype(sourceDbtype, Parameters::DBTYPE_AMINO_ACIDS)
                             || Parameters::isEqualDbtype(sourceDbtype, Parameters::DBTYPE_NUCLEOTIDES);
    writer.close(shouldMerge, !isOrdered);
    if (par.subDbMode == Parameters::SUBDB_MODE_SOFT) {
        DBReader<unsigned int>::softlinkDb(par.db2, par.db3, DBFiles::DATA);
    }
    if (!aaStorePath.empty()) {
        try {
            AAStoreTargetReader::associateSubsetDatabase(par.db3, aaStorePath,
                                                         std::move(aaStoreMembers));
        } catch (const std::exception& error) {
            Debug(Debug::ERROR) << "Cannot write RabbitAAStore subset membership: " << error.what() << "\n";
            EXIT(EXIT_FAILURE);
        }
    }
    DBWriter::writeDbtypeFile(par.db3.c_str(), sourceDbtype, isCompressed);
    DBReader<unsigned int>::softlinkDb(par.db2, par.db3, DBFiles::SEQUENCE_ANCILLARY);

    free(line);
    if (fclose(orderFile) != 0) {
        Debug(Debug::ERROR) << "Cannot close file " << par.db1 << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
