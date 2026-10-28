// 文件职责：在原生 shuffle 完成后写出 RabbitAAStore，保持 key 与物理 ID 一致。
// 直接复用内置 RabbitAAStore 编码写入器，不创建额外 FASTA 或顺序版本的 RabbitAAStore。
#include "AAStoreDatabaseBuilder.h"

#include "AAStoreTargetReader.h"
#include "DBReader.h"
#include "Debug.h"
#include "FileUtil.h"
#include "Parameters.h"
#include "Util.h"
#include "builder_file.hpp"
#include "builder_writer.hpp"
#include "fasta_reader.hpp"
#include "text_column.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
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

// Reuse RabbitAAStore's text reader when joining native shuffle partitions.
template <typename Callback>
void visitText(const std::filesystem::path& path, uint64_t count,
               bool withDescriptions, Callback callback) {
    const auto indexMode = AAStoreTargetReader::configuredIndexLoadMode();
    aastore::TextColumn names(path / "names.bin", path / "names.meta", count, false, indexMode);
    std::unique_ptr<aastore::TextColumn> descriptions;
    if (withDescriptions) descriptions = std::make_unique<aastore::TextColumn>(
        path / "descriptions.bin", path / "descriptions.meta", count, false, indexMode);
    aastore::ReadOptions options;
    options.threads = 1;
    options.input_sorted = true;
    std::vector<aastore::SequenceId> ids;
    for (uint64_t begin = 0; begin < count;) {
        ids.clear();
        const uint64_t end = std::min<uint64_t>(count, begin + 4096);
        for (uint64_t id = begin; id < end; ++id) ids.push_back(id);
        const auto nameBatch = names.batch_get(ids, options);
        const auto descriptionBatch = descriptions
            ? descriptions->batch_get(ids, options) : aastore::TextBatch{};
        for (size_t i = 0; i < ids.size(); ++i) callback(
            ids[i], nameBatch.view(i), descriptions ? descriptionBatch.view(i) : std::string_view{});
        begin = end;
    }
}

void writeRecordsCertificate(const std::filesystem::path& directory,
                             uint64_t count) {
    std::ofstream out(directory / "mmseqs_headers.lossless");
    out << "MMSEQS_AASTORE_HEADERS_V2\norigin=createdb_records\ndense_keys=1\nheader_newline=excluded\n"
        << "reconstruct=name+space+description+newline\nrecords=" << count << '\n';
    for (const auto pair : {std::pair<const char*, const char*>{"packed", "packed.bin"},
             {"sequence_meta", "meta.bin"}, {"names_meta", "names.meta"},
             {"descriptions_meta", "descriptions.meta"}, {"names", "names.bin"},
             {"descriptions", "descriptions.bin"}}) {
        struct stat file;
        if (::stat((directory / pair.second).c_str(), &file) != 0 || !S_ISREG(file.st_mode)) {
            throw std::runtime_error("Cannot stat RabbitAAStore record file");
        }
        const std::string prefix = std::string(pair.first) + '_';
        out << prefix << "size=" << file.st_size << '\n'
            << prefix << "inode=" << file.st_ino << '\n'
            << prefix << "mtime_ns=" << static_cast<uint64_t>(file.st_mtim.tv_sec) * UINT64_C(1000000000) + file.st_mtim.tv_nsec << '\n';
    }
    out.close();
    if (!out) throw std::runtime_error("Cannot publish createdb records certificate");
}

// 按 key 递增顺序编码；这里读取的是原生已经完成 shuffle 的序列。
void buildDatabase(const std::string& database, size_t threads, AAStoreCreatedbTextWriter* texts) {
    namespace fs = std::filesystem;
    const std::string index = database + ".index";
    DBReader<unsigned int> reader(database.c_str(), index.c_str(), 1,
        DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA);
    reader.open(DBReader<unsigned int>::NOSORT);
    DatabaseCloser closer{reader};
    for (size_t id = 0; id < reader.getSize(); ++id) {
        if (reader.getDbKey(id) != id) {
            throw std::runtime_error(
                "automatic RabbitAAStore creation requires dense keys starting at 0; "
                "create the source database with --id-offset 0 first");
        }
    }

    const fs::path store = database + ".aastore";
    const fs::path temporary = store.string() + ".tmp." +
                               std::to_string(::getpid());
    if (!fs::create_directory(temporary)) {
        throw std::runtime_error("temporary RabbitAAStore already exists: " +
                                 temporary.string());
    }
    try {
        Debug(Debug::INFO) << "Building RabbitAAStore in final MMseqs ID order "
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
                        "RabbitAAStore normalization would change sequence length at key " +
                        std::to_string(id));
                }
                writer.append_residues(normalized);
                writer.finish_sequence();
            }
            writer.finish();
            stats = writer.stats();
        }
        if (texts) {
            texts->installColumns(temporary.string(), stats.sequences);
            writeRecordsCertificate(temporary, stats.sequences);
        }
        syncPath(temporary / "packed.bin");
        syncPath(temporary / "meta.bin");
        if (texts) for (const char* column : {"names.bin", "names.meta", "descriptions.bin", "descriptions.meta", "mmseqs_headers.lossless"}) syncPath(temporary / column);
        syncPath(temporary, true);
        if (!fs::exists(store)) {
            fs::rename(temporary, store);
        } else {
            // 与公开 FASTA 建库器相同，只替换库文件，不删除目录中的其他内容。
            fs::rename(temporary / "packed.bin", store / "packed.bin");
            fs::rename(temporary / "meta.bin", store / "meta.bin");
            for (const char* column : {"names.bin", "names.meta", "descriptions.bin", "descriptions.meta", "mmseqs_headers.lossless"}) {
                if (texts) fs::rename(temporary / column, store / column);
                else fs::remove(store / column);
            }
            fs::remove(temporary);
        }
        syncPath(store, true);
        syncPath(store.parent_path().empty() ? fs::path(".") : store.parent_path(), true);
        AAStoreTargetReader::associateDatabase(database, store.string());
        Debug(Debug::INFO) << "RabbitAAStore built: " << stats.sequences << " sequences, "
                           << stats.packed_bytes << " packed bytes"
                           << (texts ? ", names/descriptions included; native_header_db=0" : "") << "\n";
    } catch (...) {
        std::error_code error;
        fs::remove_all(temporary, error);
        throw;
    }
}

} // namespace

class AAStoreCreatedbTextWriter::Impl {
public:
    struct Shard {
        std::filesystem::path path;
        std::unique_ptr<aastore::TextBuildWriter> text;
        uint64_t count = 0;
    };
    Impl(const std::string& database, size_t splits) : root(database + ".aastore.text.tmp." + std::to_string(::getpid())) {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Createdb text staging already exists");
        try {
            for (size_t i = 0; i < splits; ++i) {
                Shard shard; shard.path = root / std::to_string(i);
                std::filesystem::create_directory(shard.path);
                shard.text = std::make_unique<aastore::TextBuildWriter>(
                    shard.path / "names.bin", shard.path / "names.meta",
                    shard.path / "descriptions.bin", shard.path / "descriptions.meta", true, true, false);
                shards.push_back(std::move(shard));
            }
        } catch (...) { shards.clear(); std::filesystem::remove_all(root); throw; }
    }
    ~Impl() { shards.clear(); std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path root;
    std::vector<Shard> shards;
    bool finished = false;
};

AAStoreCreatedbTextWriter::AAStoreCreatedbTextWriter(const std::string& database, size_t splits)
    : impl_(std::make_unique<Impl>(database, splits)) {}
AAStoreCreatedbTextWriter::~AAStoreCreatedbTextWriter() = default;

void AAStoreCreatedbTextWriter::append(std::string_view header, size_t split) {
    if (impl_->finished || split >= impl_->shards.size()) throw std::runtime_error("Invalid createdb text append");
    auto& shard = impl_->shards[split];
    shard.text->append_header(">" + std::string(header));
    ++shard.count;
}

void AAStoreCreatedbTextWriter::finish() {
    if (impl_->finished) throw std::runtime_error("Createdb text writer already finished");
    for (auto& shard : impl_->shards) {
        shard.text->finish(shard.count);
        shard.text.reset();
    }
    impl_->finished = true;
}

void AAStoreCreatedbTextWriter::writeLookup(const std::string& database,
                                          const std::vector<unsigned int>* sources) {
    if (!impl_->finished) throw std::runtime_error("Createdb text columns are not finished");
    aastore::BufferedFileWriter output(database + ".lookup");
    DBReader<unsigned int>::LookupEntry entry;
    std::string buffer;
    uint64_t id = 0;
    for (size_t split = 0; split < impl_->shards.size(); ++split) {
        const auto& shard = impl_->shards[split];
        if (sources[split].size() != shard.count) throw std::runtime_error("Createdb source lookup count differs");
        visitText(shard.path, shard.count, false, [&](uint64_t row, std::string_view name, std::string_view) {
            entry.id = id++;
            entry.entryName = Util::parseFastaHeader(std::string(name).c_str());
            entry.fileNumber = sources[split][row];
            DBReader<unsigned int>::lookupEntryToBuffer(buffer, entry);
            output.append(buffer); buffer.clear();
        });
    }
    output.close();
}

void AAStoreCreatedbTextWriter::installColumns(const std::string& destination, uint64_t records) {
    if (!impl_->finished) throw std::runtime_error("Createdb text columns are not finished");
    uint64_t count = 0;
    for (const auto& shard : impl_->shards) count += shard.count;
    if (count != records) throw std::runtime_error("Createdb text and sequence counts differ");
    const auto path = std::filesystem::path(destination);
    if (impl_->shards.size() == 1) {
        for (const char* column : {"names", "descriptions"}) {
            for (const char* suffix : {".bin", ".meta"}) {
                std::filesystem::rename(impl_->shards[0].path / (std::string(column) + suffix), path / (std::string(column) + suffix));
            }
        }
        return;
    }
    aastore::TextBuildWriter output(path / "names.bin", path / "names.meta",
        path / "descriptions.bin", path / "descriptions.meta", true, true, false);
    std::string header;
    for (const auto& shard : impl_->shards) {
        visitText(shard.path, shard.count, true, [&](uint64_t, std::string_view name, std::string_view description) {
            header = ">";
            header.append(name);
            if (!description.empty()) { header += ' '; header.append(description); }
            output.append_header(header);
        });
        std::filesystem::remove_all(shard.path);
    }
    output.finish(records);
}

// 显式启用时校验数据库类型，复用关联库或完成自动构建。
void prepareAAStoreDatabase(const std::string& database, size_t threads,
                            bool rebuild, AAStoreCreatedbTextWriter* texts) {
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
                    Debug(Debug::INFO) << "Using existing RabbitAAStore: " << store << "\n";
                    return;
                }
            }
        }
        buildDatabase(database, threads, texts);
    } catch (const std::exception& error) {
        Debug(Debug::ERROR) << "RabbitAAStore preparation failed: " << error.what() << "\n";
        EXIT(EXIT_FAILURE);
    }
}
