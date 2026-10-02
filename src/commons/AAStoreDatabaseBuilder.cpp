// 文件职责：在原生 shuffle 完成后写出 AAStore，保持 key 与物理 ID 一致。
// 直接复用内置 AAStore 编码写入器，不创建额外 FASTA 或顺序版本的 AAStore。
#include "AAStoreDatabaseBuilder.h"

#include "AAStoreTargetReader.h"
#include "DBReader.h"
#include "Debug.h"
#include "FileUtil.h"
#include "Parameters.h"
#include "builder_writer.hpp"
#include "fasta_reader.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace {

// 确保持久化文件或目录，关闭描述符后报告同步失败。
void syncPath(const std::filesystem::path& path, bool directory = false) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC |
                                  (directory ? O_DIRECTORY : 0));
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "cannot open " + path.string());
    }
    int result;
    do {
        result = ::fsync(descriptor);
    } while (result < 0 && errno == EINTR);
    const int error = result < 0 ? errno : 0;
    const int closed = ::close(descriptor);
    if (error != 0 || closed < 0) {
        throw std::system_error(error != 0 ? error : errno,
                                std::generic_category(),
                                "cannot sync " + path.string());
    }
}

// DBReader 要求显式 close，异常退出时也释放映射和索引。
struct DatabaseCloser {
    DBReader<unsigned int>& reader;
    ~DatabaseCloser() { reader.close(); }
};

// 按 key 递增顺序编码；这里读取的是原生已经完成 shuffle 的序列。
void buildDatabase(const std::string& database, size_t threads) {
    namespace fs = std::filesystem;
    const std::string index = database + ".index";
    DBReader<unsigned int> reader(database.c_str(), index.c_str(), 1,
        DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    reader.open(DBReader<unsigned int>::NOSORT);
    DatabaseCloser closer{reader};
    for (size_t id = 0; id < reader.getSize(); ++id) {
        if (reader.getDbKey(id) != id) {
            throw std::runtime_error(
                "automatic AAStore creation requires dense keys starting at 0; "
                "create the source database with --id-offset 0 first");
        }
    }

    const fs::path store = database + ".aastore";
    const fs::path temporary = store.string() + ".tmp." +
                               std::to_string(::getpid());
    if (!fs::create_directory(temporary)) {
        throw std::runtime_error("temporary AAStore already exists: " +
                                 temporary.string());
    }
    try {
        Debug(Debug::INFO) << "Building AAStore in final MMseqs ID order "
                           << "(shuffle already applied): " << store.string() << "\n";
        aastore::BuildStats stats;
        {
            aastore::SequenceBuildWriter writer(temporary / "packed.bin",
                temporary / "meta.bin", aastore::plan_build_threads(threads, false));
            std::string normalized;
            for (size_t id = 0; id < reader.getSize(); ++id) {
                const std::string_view sequence(reader.getData(id, 0),
                                                 reader.getSeqLen(id));
                aastore::normalize_sequence_line(sequence, normalized);
                if (normalized.size() != sequence.size()) {
                    throw std::runtime_error(
                        "AAStore normalization would change sequence length at key " +
                        std::to_string(id));
                }
                writer.append_residues(normalized);
                writer.finish_sequence();
            }
            writer.finish();
            stats = writer.stats();
        }
        syncPath(temporary / "packed.bin");
        syncPath(temporary / "meta.bin");
        syncPath(temporary, true);
        if (!fs::exists(store)) {
            fs::rename(temporary, store);
        } else {
            // 与公开 FASTA 建库器相同，只替换库文件，不删除目录中的其他内容。
            fs::rename(temporary / "packed.bin", store / "packed.bin");
            fs::rename(temporary / "meta.bin", store / "meta.bin");
            for (const char* column : {"names.bin", "names.meta",
                                       "descriptions.bin", "descriptions.meta"}) {
                fs::remove(store / column);
            }
            fs::remove(temporary);
        }
        syncPath(store, true);
        syncPath(store.parent_path().empty() ? fs::path(".") : store.parent_path(), true);
        AAStoreTargetReader::associateDatabase(database, store.string());
        Debug(Debug::INFO) << "AAStore built: " << stats.sequences << " sequences, "
                           << stats.packed_bytes << " packed bytes\n";
    } catch (...) {
        std::error_code error;
        fs::remove_all(temporary, error);
        throw;
    }
}

} // namespace

// 显式启用时校验数据库类型，复用关联库或完成自动构建。
void prepareAAStoreDatabase(const std::string& database, size_t threads,
                            bool rebuild) {
    try {
        const int dbType = FileUtil::parseDbType(database.c_str());
        if (!Parameters::isEqualDbtype(dbType, Parameters::DBTYPE_AMINO_ACIDS) ||
            (DBReader<unsigned int>::getExtendedDbtype(dbType) &
             Parameters::DBTYPE_EXTENDED_GPU) != 0) {
            throw std::runtime_error(
                "--aastore requires a non-GPU amino-acid sequence database");
        }
        if (!rebuild) {
            const std::string store = AAStoreTargetReader::associatedStorePath(database);
            if (!store.empty()) {
                namespace fs = std::filesystem;
                // 原生 createdb 可能单独重写过数据库；本地旧库须重新生成。
                const bool staleLocalStore =
                    fs::absolute(store).lexically_normal() ==
                        fs::absolute(database + ".aastore").lexically_normal() &&
                    fs::last_write_time(database + ".index") >
                        fs::last_write_time(fs::path(store) / "meta.bin");
                if (!staleLocalStore) {
                    Debug(Debug::INFO) << "Using existing AAStore: " << store << "\n";
                    return;
                }
            }
        }
        buildDatabase(database, threads);
    } catch (const std::exception& error) {
        Debug(Debug::ERROR) << "AAStore preparation failed: " << error.what() << "\n";
        EXIT(EXIT_FAILURE);
    }
}
