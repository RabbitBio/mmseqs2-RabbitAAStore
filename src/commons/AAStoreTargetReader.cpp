#include "AAStoreTargetReader.h"

#include "AAStoreMemoryBudget.h"
#include "Debug.h"
#include "Parameters.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint64_t kMinimumPrefixBytes = UINT64_C(64) * 1024 * 1024;
constexpr char kSubsetMagic[8] = {'M', 'M', 'A', 'S', 'I', 'D', 'S', '1'};
constexpr const char* kFullAssociation = "MMSEQS_AASTORE_FULL_V1";
constexpr const char* kSubsetAssociation = "MMSEQS_AASTORE_SUBSET_V1";

std::string resolved_store_path(const std::string& store) {
    return std::filesystem::canonical(store).string();
}

uint64_t metadata_mtime_ns(const std::string& store) {
    struct stat metadata;
    if (::stat((std::filesystem::path(store) / "meta.bin").c_str(), &metadata) != 0) {
        throw std::runtime_error("cannot stat AAStore metadata: " + store);
    }
    return static_cast<uint64_t>(metadata.st_mtim.tv_sec) * UINT64_C(1000000000) +
        static_cast<uint64_t>(metadata.st_mtim.tv_nsec);
}

void write_association(const std::string& database, const std::string& store,
                       bool subset) {
    namespace fs = std::filesystem;
    const fs::path locator = database + ".aastore.path";
    const std::string resolvedStore = resolved_store_path(store);
    const fs::path localStore = database + ".aastore";
    if (!subset && fs::exists(localStore) &&
        fs::canonical(localStore).string() == resolvedStore) {
        fs::remove(locator);
        return;
    }
    const fs::path temporary = locator.string() + ".tmp." + std::to_string(::getpid());
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << resolvedStore << '\n'
               << (subset ? kSubsetAssociation : kFullAssociation) << '\n';
        output.close();
        if (!output) throw std::runtime_error("cannot write AAStore association: " + locator.string());
        fs::rename(temporary, locator);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::string association_layout(const std::string& database) {
    std::ifstream input(database + ".aastore.path", std::ios::binary);
    if (!input) return kFullAssociation;
    std::string path, layout;
    std::getline(input, path);
    std::getline(input, layout);
    if (!layout.empty() && layout.back() == '\r') layout.pop_back();
    if (!layout.empty() && layout != kFullAssociation && layout != kSubsetAssociation) {
        throw std::runtime_error("unsupported AAStore database association layout");
    }
    return layout;
}

struct SubsetHeader {
    uint64_t count = 0;
    uint64_t storeCount = 0;
    uint64_t totalResidues = 0;
    uint64_t maxResidues = 0;
    uint64_t metaBytes = 0;
    uint64_t metaMtimeNs = 0;
    uint64_t pathBytes = 0;
};
static_assert(sizeof(SubsetHeader) == 7 * sizeof(uint64_t), "AAStore subset header padding");

SubsetHeader read_subset_header(std::ifstream& input, std::string& storePath) {
    char magic[8] = {};
    SubsetHeader header;
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || std::memcmp(magic, kSubsetMagic, sizeof(magic)) != 0 ||
        header.count > UINT_MAX ||
        header.pathBytes == 0 || header.pathBytes > 16384) {
        throw std::runtime_error("invalid AAStore subset membership header");
    }
    storePath.resize(static_cast<size_t>(header.pathBytes));
    input.read(storePath.data(), static_cast<std::streamsize>(storePath.size()));
    if (!input || storePath.find('\0') != std::string::npos) {
        throw std::runtime_error("invalid AAStore subset store path");
    }
    return header;
}

uint64_t saturating_multiply(uint64_t left, uint64_t right) {
    if (left == 0 || right == 0) return 0;
    return right > std::numeric_limits<uint64_t>::max() / left
        ? std::numeric_limits<uint64_t>::max()
        : left * right;
}

bool read_number_file(const std::filesystem::path& path, uint64_t& value) {
    std::ifstream input(path);
    std::string text;
    if (!(input >> text) || text == "max") return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

uint64_t read_memory_stat(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string key;
    uint64_t value = 0;
    uint64_t inactiveFile = 0;
    while (input >> key >> value) {
        if (key == "total_inactive_file" || key == "inactive_file") {
            inactiveFile = std::max(inactiveFile, value);
        }
    }
    return inactiveFile;
}

uint64_t host_total_memory() {
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSize <= 0) return 0;
    return saturating_multiply(static_cast<uint64_t>(pages),
                               static_cast<uint64_t>(pageSize));
}

uint64_t host_available_memory(uint64_t fallback) {
    std::ifstream input("/proc/meminfo");
    std::string key;
    uint64_t kib = 0;
    std::string unit;
    while (input >> key >> kib >> unit) {
        if (key == "MemAvailable:") {
            return saturating_multiply(kib, UINT64_C(1024));
        }
    }
    return fallback;
}

struct MemoryAvailability {
    uint64_t limit = 0;
    uint64_t available = 0;
    bool cgroupLimited = false;
};

MemoryAvailability memory_availability() {
    MemoryAvailability result;
    result.limit = host_total_memory();
    result.available = host_available_memory(result.limit);

    std::ifstream cgroups("/proc/self/cgroup");
    std::string line;
    std::filesystem::path memoryDirectory;
    bool cgroupV2 = false;
    while (std::getline(cgroups, line)) {
        const size_t first = line.find(':');
        const size_t second = first == std::string::npos
            ? std::string::npos : line.find(':', first + 1);
        if (second == std::string::npos) continue;
        const std::string controllers =
            line.substr(first + 1, second - first - 1);
        const std::string cgroupPath = line.substr(second + 1);
        const bool memoryController =
            ("," + controllers + ",").find(",memory,") !=
            std::string::npos;
        if (!memoryController && !controllers.empty()) continue;
        const std::filesystem::path relative =
            std::filesystem::path(cgroupPath).relative_path();
        memoryDirectory = (controllers.empty()
            ? std::filesystem::path("/sys/fs/cgroup")
            : std::filesystem::path("/sys/fs/cgroup/memory")) / relative;
        cgroupV2 = controllers.empty();
        if (memoryController) break;
    }

    if (memoryDirectory.empty()) return result;
    uint64_t limit = 0;
    uint64_t usage = 0;
    const bool haveLimit = read_number_file(
        memoryDirectory / (cgroupV2 ? "memory.max"
                                    : "memory.limit_in_bytes"), limit);
    const bool haveUsage = read_number_file(
        memoryDirectory / (cgroupV2 ? "memory.current"
                                    : "memory.usage_in_bytes"), usage);
    if (!haveLimit || !haveUsage || limit == 0 ||
        (result.limit != 0 && limit >= result.limit)) {
        return result;
    }

    const uint64_t inactiveFile = read_memory_stat(
        memoryDirectory / "memory.stat");
    const uint64_t unreclaimableUsage =
        usage > inactiveFile ? usage - inactiveFile : 0;
    const uint64_t cgroupAvailable = limit > unreclaimableUsage
        ? limit - unreclaimableUsage : 0;
    result.limit = limit;
    result.available = std::min(result.available, cgroupAvailable);
    result.cgroupLimited = true;
    return result;
}

uint64_t file_bytes(const std::filesystem::path& path) {
    std::error_code error;
    const uintmax_t bytes = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<uint64_t>(bytes);
}

struct HeaderRecordsProof {
    bool present = false;
    bool denseIds = false;
    bool insertHeaderSpace = false;
    uint64_t records = 0;
};

HeaderRecordsProof header_records_proof(const std::string& database,
                                       const std::string& store) {
    HeaderRecordsProof proof;
    if (store.empty()) return proof;
    const std::filesystem::path path(store);
    std::ifstream marker(path / "mmseqs_headers.lossless");
    if (!marker) return proof;
    std::string line;
    if (!std::getline(marker, line) ||
        (line != "MMSEQS_AASTORE_HEADERS_V1" && line != "MMSEQS_AASTORE_HEADERS_V2")) {
        throw std::runtime_error("invalid AAStore native-header certificate");
    }
    const bool legacy = line == "MMSEQS_AASTORE_HEADERS_V1";
    const std::string layout = association_layout(database);
    if (std::filesystem::exists(database + ".aastore.ids") ||
        layout == kSubsetAssociation || (!legacy && layout != kFullAssociation)) {
        throw std::runtime_error("AAStore record export requires an explicit full database view");
    }
    std::unordered_map<std::string, std::string> fields;
    while (std::getline(marker, line)) {
        const size_t equal = line.find('=');
        if (equal == std::string::npos ||
            !fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second) {
            throw std::runtime_error("invalid AAStore native-header certificate field");
        }
    }
    const auto number = [&](const char* key) {
        const auto field = fields.find(key);
        if (field == fields.end() || field->second.empty() ||
            field->second.find_first_not_of("0123456789") != std::string::npos) {
            throw std::runtime_error(std::string("missing native-header certificate field: ") + key);
        }
        return static_cast<uint64_t>(std::stoull(field->second));
    };
    const std::string& reconstruction = fields["reconstruct"];
    if (fields["header_newline"] != "excluded" ||
        (reconstruction != "name+description+newline" &&
         reconstruction != "name+space+description+newline")) {
        throw std::runtime_error("unsupported AAStore record-header reconstruction");
    }
    proof.insertHeaderSpace = reconstruction == "name+space+description+newline";
    const uint64_t dense = number("dense_keys");
    if (dense > 1) throw std::runtime_error("invalid native-header key layout");
    if (legacy) {
        // Verify the old source binding once before publishing an AAStore-only
        // certificate; an old one-line subset must not become a full view.
        struct stat indexStat;
        if (::stat((database + ".index").c_str(), &indexStat) != 0 ||
            !S_ISREG(indexStat.st_mode) ||
            number("source_index_size") != static_cast<uint64_t>(indexStat.st_size) ||
            number("source_index_inode") != static_cast<uint64_t>(indexStat.st_ino) ||
            number("source_index_mtime_ns") !=
                static_cast<uint64_t>(indexStat.st_mtim.tv_sec) * UINT64_C(1000000000) +
                static_cast<uint64_t>(indexStat.st_mtim.tv_nsec)) {
            throw std::runtime_error("legacy AAStore records need the original index for one-time certification upgrade");
        }
    }
    if (number("packed_size") != file_bytes(path / "packed.bin") ||
        number("sequence_meta_size") != file_bytes(path / "meta.bin")) {
        throw std::runtime_error("AAStore record certificate no longer matches AAStore files");
    }
    std::ifstream metadata(path / "meta.bin", std::ios::binary);
    char magic[8] = {};
    uint64_t version = 0, records = 0;
    metadata.read(magic, sizeof(magic));
    metadata.read(reinterpret_cast<char*>(&version), sizeof(version));
    metadata.read(reinterpret_cast<char*>(&records), sizeof(records));
    if (!metadata || std::memcmp(magic, "AAPCMP3\0", 8) != 0 || version != 3 ||
        records != number("records")) {
        throw std::runtime_error("AAStore native-header certificate sequence count differs");
    }
    for (const auto pair : {std::pair<const char*, const char*>{"packed", "packed.bin"},
             {"sequence_meta", "meta.bin"}, {"names_meta", "names.meta"},
             {"descriptions_meta", "descriptions.meta"}, {"names", "names.bin"},
             {"descriptions", "descriptions.bin"}}) {
        struct stat file;
        if (::stat((path / pair.second).c_str(), &file) != 0 || !S_ISREG(file.st_mode)) {
            throw std::runtime_error("incomplete certified AAStore record files");
        }
        const std::string prefix = std::string(pair.first) + '_';
        const uint64_t mtime = static_cast<uint64_t>(file.st_mtim.tv_sec) * UINT64_C(1000000000) + file.st_mtim.tv_nsec;
        if (legacy) {
            fields[prefix + "size"] = std::to_string(file.st_size);
            fields[prefix + "inode"] = std::to_string(file.st_ino);
            fields[prefix + "mtime_ns"] = std::to_string(mtime);
        } else if (number((prefix + "size").c_str()) != static_cast<uint64_t>(file.st_size) ||
                   number((prefix + "inode").c_str()) != static_cast<uint64_t>(file.st_ino) ||
                   number((prefix + "mtime_ns").c_str()) != mtime) {
            throw std::runtime_error("AAStore record certificate no longer matches AAStore files");
        }
    }
    if (legacy && dense == 1) {
        // Publish the full-view association first, so an interrupted upgrade
        // cannot leave a V2 certificate with an ambiguous legacy association.
        AAStoreTargetReader::associateDatabase(database, store);
        fields.erase("source_index_size");
        fields.erase("source_index_inode");
        fields.erase("source_index_mtime_ns");
        marker.close();
        const auto certificate = path / "mmseqs_headers.lossless";
        const auto temporary = certificate.string() + ".tmp." + std::to_string(::getpid());
        try {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << "MMSEQS_AASTORE_HEADERS_V2\n";
            for (const auto& field : fields) output << field.first << '=' << field.second << '\n';
            output.close();
            if (!output) throw std::runtime_error("cannot upgrade AAStore record certificate");
            std::filesystem::rename(temporary, certificate);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
        Debug(Debug::INFO) << "Upgraded AAStore record certificate to AAStore index binding: " << database << "\n";
    }
    proof.present = true;
    proof.denseIds = dense == 1;
    proof.records = records;
    return proof;
}

aastore_memory::Budget automatic_storage_budget(
        const std::string& database, size_t computeThreads,
        uint64_t stageMemoryReserve, const std::string& recordStore = {},
        bool denseRecordIds = false, uint64_t recordSafetyReserve = 0,
        const std::string& scoringStore = {}, bool includeDescriptions = true) {
    const MemoryAvailability memory = memory_availability();
    const uint64_t indexReserve = database.empty() || denseRecordIds || !scoringStore.empty()
        ? 0 : file_bytes(database + ".index");
    const uint64_t threadReserve = saturating_multiply(
        std::max<size_t>(1, computeThreads),
        AAStoreTargetReader::kThreadBatchBytes);
    aastore_memory::Budget budget;
    if (!scoringStore.empty()) {
        const std::filesystem::path path(scoringStore);
        const uint64_t metadataBytes = aastore_memory::add_bytes(
            file_bytes(path / "meta.bin"), aastore_memory::add_bytes(
                file_bytes(path / "names.meta"), aastore_memory::add_bytes(
                    file_bytes(path / "descriptions.meta"),
                    file_bytes(database + ".aastore.ids"))));
        budget = aastore_memory::plan(memory.limit, memory.available,
            0, threadReserve, aastore_memory::add_bytes(stageMemoryReserve, metadataBytes));
        budget.stageReserve = stageMemoryReserve;
        budget.metadataReserve = metadataBytes;
    } else if (recordStore.empty()) {
        budget = aastore_memory::plan(memory.limit, memory.available,
            indexReserve, threadReserve, stageMemoryReserve);
    } else {
        const std::filesystem::path path(recordStore);
        const uint64_t textBytes = aastore_memory::add_bytes(
            file_bytes(path / "names.bin"),
            includeDescriptions ? file_bytes(path / "descriptions.bin") : 0);
        const uint64_t metadataBytes = aastore_memory::add_bytes(
            file_bytes(path / "meta.bin"), aastore_memory::add_bytes(
                file_bytes(path / "names.meta"), file_bytes(path / "descriptions.meta")));
        budget = aastore_memory::plan_records(memory.limit, memory.available,
            indexReserve, threadReserve, stageMemoryReserve, textBytes, metadataBytes,
            recordSafetyReserve);
    }
    budget.cgroupLimited = memory.cgroupLimited;
    return budget;
}

const char* storage_mode_name(aastore::StorageMode mode) {
    switch (mode) {
        case aastore::StorageMode::FullMemory: return "full_memory";
        case aastore::StorageMode::PrefixMemory: return "prefix_memory";
        case aastore::StorageMode::ByteRange: return "byte_range";
    }
    return "unknown";
}

}  // namespace

AAStoreTargetReader::AAStoreTargetReader() = default;

bool AAStoreTargetReader::IdPositionMap::insert_if_absent(
        size_t key, size_t value) noexcept {
    if (keys_.empty() || key == kEmpty) return false;
    size_t position = hash(key) & mask_;
    while (true) {
        size_t& stored = keys_[position];
        if (stored == key) return false;
        if (stored == kEmpty) {
            stored = key;
            values_[position] = value;
            return true;
        }
        position = (position + 1) & mask_;
    }
}

bool AAStoreTargetReader::IdPositionMap::find(
        size_t key, size_t& value) const noexcept {
    if (keys_.empty() || key == kEmpty) return false;
    size_t position = hash(key) & mask_;
    while (true) {
        const size_t stored = keys_[position];
        if (stored == key) {
            value = values_[position];
            return true;
        }
        if (stored == kEmpty) return false;
        position = (position + 1) & mask_;
    }
}

bool AAStoreTargetReader::Batch::view(
        size_t mmseqsId, std::string_view& sequence) const {
    size_t position = 0;
    if (!positions.find(mmseqsId, position)) return false;
    sequence = fetched.view(position);
    return true;
}

AAStoreTargetReader::~AAStoreTargetReader() {
    if (trackedResidentBytes_ != 0) decrementMemory(trackedResidentBytes_);
}

void AAStoreTargetReader::installStore(
        std::unique_ptr<aastore::Store> store) {
    const uint64_t residentBytes = store ? aastore_memory::add_bytes(
        store->resident_bytes(), store->resident_text_bytes()) : 0;
    const size_t tracked = static_cast<size_t>(std::min<uint64_t>(
        residentBytes, std::numeric_limits<size_t>::max()));
    if (trackedResidentBytes_ != 0) decrementMemory(trackedResidentBytes_);
    store_ = std::move(store);
    trackedResidentBytes_ = tracked;
    if (trackedResidentBytes_ != 0) incrementMemory(trackedResidentBytes_);
}

std::string AAStoreTargetReader::associatedStorePath(
        const std::string& database) {
    namespace fs = std::filesystem;
    const fs::path locator = database + ".aastore.path";
    std::ifstream input(locator, std::ios::binary);
    if (input) {
        std::string path;
        std::getline(input, path);
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (path.empty() ||
            !fs::is_regular_file(fs::path(path) / "packed.bin") ||
            !fs::is_regular_file(fs::path(path) / "meta.bin")) {
            throw std::runtime_error("invalid AAStore database association: " + locator.string());
        }
        return path;
    }
    const fs::path localStore = database + ".aastore";
    if (fs::is_regular_file(localStore / "packed.bin") &&
        fs::is_regular_file(localStore / "meta.bin")) return localStore.string();
    return std::string();
}

void AAStoreTargetReader::associateDatabase(const std::string& database,
                                            const std::string& store) {
    write_association(database, store, false);
    // A fresh full database must never inherit an earlier subset's members.
    std::filesystem::remove(database + ".aastore.ids");
}

aastore::IndexLoadMode AAStoreTargetReader::configuredIndexLoadMode() {
    return Parameters::getInstance().aastoreIndexMode == "read"
        ? aastore::IndexLoadMode::Read : aastore::IndexLoadMode::Mmap;
}

void AAStoreTargetReader::associateSubsetDatabase(
        const std::string& database, const std::string& store,
        std::vector<uint32_t> keys) {
    namespace fs = std::filesystem;
    std::sort(keys.begin(), keys.end());
    aastore::StoreOptions options;
    options.index_load_mode = configuredIndexLoadMode();
    options.storage_mode = aastore::StorageMode::ByteRange;
    const std::string path = resolved_store_path(store);
    aastore::Store metadataStore(path, options);
    SubsetHeader header;
    header.count = keys.size();
    header.storeCount = metadataStore.size();
    header.metaBytes = file_bytes(fs::path(path) / "meta.bin");
    header.metaMtimeNs = metadata_mtime_ns(path);
    header.pathBytes = path.size();
    for (const uint32_t key : keys) {
        if (key >= metadataStore.size()) throw std::out_of_range("AAStore subset member exceeds physical ID range");
        const uint64_t length = metadataStore.sequence_length(key);
        header.totalResidues = aastore_memory::add_bytes(header.totalResidues, length);
        header.maxResidues = std::max(header.maxResidues, length);
    }
    const fs::path members = database + ".aastore.ids";
    const fs::path temporary = members.string() + ".tmp." + std::to_string(::getpid());
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(kSubsetMagic, sizeof(kSubsetMagic));
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(path.data(), static_cast<std::streamsize>(path.size()));
        output.write(reinterpret_cast<const char*>(keys.data()),
                     static_cast<std::streamsize>(keys.size() * sizeof(uint32_t)));
        output.close();
        if (!output) throw std::runtime_error("cannot write AAStore subset membership: " + members.string());
        fs::rename(temporary, members);
        write_association(database, store, true);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

void AAStoreTargetReader::resetIdMap() {
    mmseqsToAAStoreId_.clear();
    keyOrder_.clear();
    hasIdMap_ = false;
    viewStatisticsKnown_ = false;
    viewTotalLength_ = 0;
    viewMaxLength_ = 0;
}

void AAStoreTargetReader::open(
        const std::string& path,
        const aastore::StoreOptions& storeOptions,
        const aastore::ReadOptions& readOptions) {
    installStore(std::make_unique<aastore::Store>(path, storeOptions));
    resetIdMap();
    recordsEnabled_ = false;
    recordsDenseIds_ = false;
    recordHeaderNeedsSpace_ = false;
    headerDescriptionsEnabled_ = false;
    readOptions_ = readOptions;
}

bool AAStoreTargetReader::openForDatabase(
        const std::string& database, size_t computeThreads,
        uint64_t stageMemoryReserve, bool enabled, bool preferRecordFields,
        uint64_t recordSafetyReserve) {
    return enabled && openAssociated(database, computeThreads, stageMemoryReserve,
                                     preferRecordFields, recordSafetyReserve, false);
}

bool AAStoreTargetReader::openForScoring(
        const std::string& database, size_t computeThreads,
        uint64_t stageMemoryReserve, bool enabled) {
    return enabled && openAssociated(database, computeThreads, stageMemoryReserve,
                                     false, 0, true);
}

bool AAStoreTargetReader::openForHeaders(
        const std::string& database, size_t computeThreads,
        uint64_t stageMemoryReserve, bool fullHeader) {
    return openAssociated(database, computeThreads, stageMemoryReserve,
                          true, UINT64_C(8) * 1024 * 1024 * 1024, false,
                          true, fullHeader);
}

bool AAStoreTargetReader::openAssociated(
        const std::string& database, size_t computeThreads,
        uint64_t stageMemoryReserve, bool preferRecordFields,
        uint64_t recordSafetyReserve, bool scoring,
        bool headersOnly, bool fullHeader) {
    const std::string path = associatedStorePath(database);
    if (path.empty()) return false;

    SubsetHeader subsetHeader;
    std::vector<uint32_t> subsetKeys;
    bool subsetView = false;
    if (scoring) {
        namespace fs = std::filesystem;
        const fs::path members = database + ".aastore.ids";
        const std::string layout = association_layout(database);
        if (fs::exists(members)) {
            std::ifstream input(members, std::ios::binary);
            std::string boundStore;
            subsetHeader = read_subset_header(input, boundStore);
            const uint64_t expectedBytes = sizeof(kSubsetMagic) + sizeof(SubsetHeader) +
                subsetHeader.pathBytes + subsetHeader.count * sizeof(uint32_t);
            if (file_bytes(members) != expectedBytes ||
                boundStore != resolved_store_path(path) ||
                subsetHeader.metaBytes != file_bytes(fs::path(path) / "meta.bin") ||
                subsetHeader.metaMtimeNs != metadata_mtime_ns(path)) {
                throw std::runtime_error("AAStore subset membership differs from associated store: " + members.string());
            }
            subsetView = true;
        } else if (layout == kSubsetAssociation) {
            throw std::runtime_error("missing AAStore subset membership: " + members.string());
        } else if (layout.empty()) {
            // Compatibility for old one-line associations only. Convert keys
            // once without constructing/parsing a native DBReader index.
            std::ifstream index(database + ".index");
            if (!index) throw std::runtime_error("legacy AAStore association needs its index for one-time membership conversion");
            std::string line;
            uint64_t count = 0;
            bool dense = true;
            while (std::getline(index, line)) {
                std::istringstream fields(line);
                uint64_t key = 0, offset = 0, length = 0;
                std::string extra;
                if (!(fields >> key >> offset >> length) || fields >> extra || key >= UINT_MAX) {
                    throw std::runtime_error("invalid legacy AAStore sequence index entry");
                }
                if (dense && key != count) {
                    dense = false;
                    subsetKeys.resize(static_cast<size_t>(count));
                    std::iota(subsetKeys.begin(), subsetKeys.end(), uint32_t{0});
                }
                if (!dense) subsetKeys.push_back(static_cast<uint32_t>(key));
                if (++count > UINT_MAX) throw std::runtime_error("AAStore view exceeds MMseqs ID range");
            }
            if (!index.eof()) throw std::runtime_error("cannot read legacy AAStore sequence index");
            aastore::StoreOptions metadataOptions;
            metadataOptions.index_load_mode = configuredIndexLoadMode();
            metadataOptions.storage_mode = aastore::StorageMode::ByteRange;
            uint64_t storeCount;
            {
                aastore::Store metadataStore(path, metadataOptions);
                storeCount = metadataStore.size();
            }
            if (dense && count == storeCount) associateDatabase(database, path);
            else {
                if (dense) {
                    subsetKeys.resize(static_cast<size_t>(count));
                    std::iota(subsetKeys.begin(), subsetKeys.end(), uint32_t{0});
                }
                associateSubsetDatabase(database, path, std::move(subsetKeys));
            }
            Debug(Debug::INFO) << "Upgraded legacy AAStore association to explicit membership: " << database << "\n";
            return openAssociated(database, computeThreads, stageMemoryReserve,
                                  preferRecordFields, recordSafetyReserve, scoring,
                                  headersOnly, fullHeader);
        }
    }

    const HeaderRecordsProof proof = preferRecordFields
        ? header_records_proof(database, path) : HeaderRecordsProof{};
    // Old raw separators can make the native accession span both fields.
    const bool includeDescriptions = !headersOnly || fullHeader || !proof.insertHeaderSpace;
    if (headersOnly && (!proof.present || !proof.denseIds)) {
        throw std::runtime_error("AAStore TSV export needs certified dense record IDs");
    }
    aastore_memory::Budget automaticBudget = automatic_storage_budget(
        database, computeThreads, stageMemoryReserve,
        proof.present ? path : std::string(), proof.denseIds, recordSafetyReserve,
        scoring ? path : std::string(), includeDescriptions);
    if (headersOnly) {
        automaticBudget.cacheBytes = 0;
        automaticBudget.pageCacheReserve = automaticBudget.remainingPool;
    }
    if (subsetView) {
        // Probe available memory before allocating members so their bytes are
        // reserved once in the budget, rather than also reducing availability.
        std::ifstream input(database + ".aastore.ids", std::ios::binary);
        input.seekg(static_cast<std::streamoff>(sizeof(kSubsetMagic) +
                    sizeof(SubsetHeader) + subsetHeader.pathBytes));
        subsetKeys.resize(static_cast<size_t>(subsetHeader.count));
        input.read(reinterpret_cast<char*>(subsetKeys.data()),
                   static_cast<std::streamsize>(subsetKeys.size() * sizeof(uint32_t)));
        if (!input) throw std::runtime_error("truncated AAStore subset membership: " + database + ".aastore.ids");
        for (size_t id = 0; id < subsetKeys.size(); ++id) {
            if (subsetKeys[id] >= subsetHeader.storeCount ||
                (id != 0 && subsetKeys[id - 1] > subsetKeys[id])) {
                throw std::runtime_error("invalid AAStore subset member order or ID");
            }
        }
    }
    const uint64_t packedBytes = file_bytes(
        std::filesystem::path(path) / "packed.bin");
    const uint64_t selectedBytes =
        std::min(automaticBudget.cacheBytes, packedBytes);
    aastore::StoreOptions options;
    options.index_load_mode = configuredIndexLoadMode();
    size_t cacheBytes = 0;
    if (packedBytes != 0 && selectedBytes >= packedBytes) {
        options.storage_mode = aastore::StorageMode::FullMemory;
    } else if (selectedBytes >= kMinimumPrefixBytes) {
        options.storage_mode = aastore::StorageMode::PrefixMemory;
        cacheBytes = static_cast<size_t>(std::min<uint64_t>(
            selectedBytes, std::numeric_limits<size_t>::max()));
    } else {
        options.storage_mode = aastore::StorageMode::ByteRange;
    }

    options.threads = 8;
    options.cache_bytes = cacheBytes;
    options.resident_names = proof.present && automaticBudget.residentText;
    options.resident_descriptions = options.resident_names && includeDescriptions;
    if (proof.present) {
        Debug(Debug::INFO) << "AAStore record allocation planned: resident_text_columns="
            << (options.resident_names ? 1 : 0)
            << " memory_text_reserve=" << automaticBudget.textReserve
            << " memory_metadata_reserve=" << automaticBudget.metadataReserve
            << " memory_safety_reserve=" << automaticBudget.safetyReserve
            << " memory_available=" << automaticBudget.memoryAvailable
            << " memory_packed_budget=" << automaticBudget.cacheBytes << "\n";
        std::cout.flush();
    }
    installStore(std::make_unique<aastore::Store>(
        std::filesystem::path(path), options));
    resetIdMap();
    if (scoring) {
        if (store_->size() > UINT_MAX) throw std::runtime_error("AAStore database exceeds MMseqs key range");
        if (subsetView) {
            if (subsetHeader.storeCount != store_->size()) {
                throw std::runtime_error("AAStore subset membership sequence count differs from store");
            }
            mmseqsToAAStoreId_ = std::move(subsetKeys);
            hasIdMap_ = true;
        }
        updateViewStatistics();
        if (subsetView && (viewTotalLength_ != subsetHeader.totalResidues ||
                           viewMaxLength_ != subsetHeader.maxResidues)) {
            throw std::runtime_error("AAStore subset membership statistics differ from metadata");
        }
    }
    recordsEnabled_ = proof.present;
    recordsDenseIds_ = proof.denseIds;
    recordHeaderNeedsSpace_ = proof.insertHeaderSpace;
    headerDescriptionsEnabled_ = includeDescriptions;
    if (recordsEnabled_) {
        if (store_->size() != proof.records || !store_->has_names() || !store_->has_descriptions()) {
            throw std::runtime_error("certified AAStore record columns differ from sequence metadata");
        }
    }

    readOptions_ = aastore::ReadOptions{};
    readOptions_.threads = 1;
    readOptions_.io_backend = aastore::IoBackend::IoUring;

    Debug(Debug::INFO)
        << "AAStore target enabled: " << path
        << " io_backend=" << (headersOnly ? "pread" : "io_uring")
        << " threads=1"
        << " merge_gap=" << readOptions_.merge_gap_bytes
        << " max_range=" << readOptions_.max_merged_range_bytes
        << " cache_bytes=" << cacheBytes
        << " storage_mode=" << storage_mode_name(options.storage_mode)
        << " index_load_mode=" << (store_->index_load_mode() == aastore::IndexLoadMode::Read ? "read" : "mmap")
        << " memory_limit=" << automaticBudget.memoryLimit
        << " memory_available=" << automaticBudget.memoryAvailable
        << " memory_reserve=" << automaticBudget.memoryReserve
        << " memory_index_reserve=" << automaticBudget.indexReserve
        << " memory_buffer_reserve=" << automaticBudget.bufferReserve
        << " memory_safety_reserve=" << automaticBudget.safetyReserve
        << " memory_stage_reserve=" << automaticBudget.stageReserve
        << " memory_metadata_reserve=" << automaticBudget.metadataReserve
        << " memory_text_reserve=" << automaticBudget.textReserve
        << " resident_text_columns=" << (options.resident_names ? 1 : 0)
        << " resident_text_bytes=" << store_->resident_text_bytes()
        << " records_dense_ids=" << (recordsDenseIds_ ? 1 : 0)
        << " sequence_index=" << (scoring || recordsDenseIds_ ? "aastore" : "native_mapping")
        << " subset_members=" << (scoring && hasIdMap_ ? size() : 0)
        << " memory_remaining_pool=" << automaticBudget.remainingPool
        << " memory_packed_budget=" << automaticBudget.cacheBytes
        << " memory_page_cache_reserve="
        << automaticBudget.pageCacheReserve
        << " cgroup_limited="
        << (automaticBudget.cgroupLimited ? 1 : 0)
        << " packed_resident_bytes=" << store_->resident_bytes()
        << " packed_preload_seconds="
        << static_cast<double>(store_->preload_ns()) / 1e9
        << "\n";
    return true;
}

bool AAStoreTargetReader::hasNativeRecordHeaders(const std::string& database) {
    return header_records_proof(database, associatedStorePath(database)).present;
}

bool AAStoreTargetReader::recordsEnabled() const noexcept { return recordsEnabled_; }

bool AAStoreTargetReader::recordsDenseIds() const noexcept { return recordsDenseIds_; }

bool AAStoreTargetReader::headerDescriptionsEnabled() const noexcept {
    return headerDescriptionsEnabled_;
}

void AAStoreTargetReader::getHeaderFields(
        size_t key, std::string& name, std::string& description) const {
    if (!recordsEnabled_ || !recordsDenseIds_) {
        throw std::logic_error("AAStore TSV headers require certified dense record IDs");
    }
    aastore::ReadOptions options;
    options.threads = 1;
    options.io_backend = aastore::IoBackend::Pread;
    const auto fields = headerDescriptionsEnabled_
        ? aastore::RecordFields::Name | aastore::RecordFields::Description
        : aastore::RecordFields::Name;
    const auto records = store_->batch_get_records({key}, fields, options);
    const std::string_view recordName = records.name_view(0);
    if (recordName.empty()) name.clear();
    else name.assign(recordName.data(), recordName.size());
    const std::string_view recordDescription = headerDescriptionsEnabled_
        ? records.description_view(0) : std::string_view();
    if (recordDescription.empty()) description.clear();
    else description.assign(recordDescription.data(), recordDescription.size());
}

std::string_view AAStoreTargetReader::headerSeparator(std::string_view description) const noexcept {
    return recordHeaderNeedsSpace_ && !description.empty() ? std::string_view(" ") : std::string_view();
}

uint64_t AAStoreTargetReader::recordLength(size_t mmseqsId) const {
    if (!recordsEnabled_) throw std::logic_error("AAStore native record fields are not enabled");
    const size_t id = store_id_for_mmseqs_id(mmseqsId);
    return aastore_memory::add_bytes(store_->sequence_length(id),
        aastore_memory::add_bytes(store_->name_length(id), store_->description_length(id)));
}

void AAStoreTargetReader::fetch_records(const std::vector<size_t>& ids,
                                      RecordThreadBatch& batch,
                                      size_t computeThreads, size_t readThreads) const {
    if (!recordsEnabled_) throw std::logic_error("AAStore native record fields are not enabled");
    batch.physicalIds.clear();
    batch.physicalIds.reserve(ids.size());
    batch.positions.prepare(ids.size());
    for (const size_t id : ids) {
        batch.positions.insert_if_absent(id, batch.physicalIds.size());
        batch.physicalIds.push_back(store_id_for_mmseqs_id(id));
    }
    aastore::ReadOptions options = readOptions_;
    options.threads = readThreads;
    options.io_queue_depth = threadBatchQueueDepth(computeThreads);
    // batch_get_records does not reuse its arenas. Release the previous batch
    // first so a second 64 MiB batch cannot double the resident arena budget.
    batch.records = aastore::RecordBatch{};
    batch.records = store_->batch_get_records(batch.physicalIds, aastore::RecordFields::All, options);
}

void AAStoreTargetReader::fetch_headers(const std::vector<size_t>& ids,
                                      RecordThreadBatch& batch, size_t readThreads) const {
    if (!recordsEnabled_) throw std::logic_error("AAStore native record fields are not enabled");
    batch.physicalIds.clear();
    batch.physicalIds.reserve(ids.size());
    batch.positions.prepare(ids.size());
    for (const size_t id : ids) {
        batch.positions.insert_if_absent(id, batch.physicalIds.size());
        batch.physicalIds.push_back(store_id_for_mmseqs_id(id));
    }
    aastore::ReadOptions options = readOptions_;
    options.threads = readThreads;
    options.io_queue_depth = threadBatchQueueDepth(readThreads);
    batch.records = aastore::RecordBatch{};
    batch.records = store_->batch_get_records(batch.physicalIds,
        aastore::RecordFields::Name | aastore::RecordFields::Description, options);
}

bool AAStoreTargetReader::enabled() const noexcept {
    return store_ != nullptr;
}

bool AAStoreTargetReader::fullMemory() const noexcept {
    return store_ &&
        store_->storage_mode() == aastore::StorageMode::FullMemory;
}

size_t AAStoreTargetReader::threadBatchQueueDepth(size_t computeThreads) {
    if (computeThreads == 0 || computeThreads > kThreadBatchIoBudget) {
        throw std::invalid_argument(
            "AAStore thread batches require 1 to 512 compute threads");
    }
    return kThreadBatchIoBudget / computeThreads;
}

void AAStoreTargetReader::prepare_into(ThreadBatch& batch) const {
    if (!store_) throw std::logic_error("AAStore reader is not open");
    Batch& result = batch.sequences;
    result.positions.prepare(batch.ids.size());
    result.fetched.offsets.clear();
    result.fetched.lengths.clear();
    batch.physicalIds.clear();
    batch.outputs.clear();

    size_t bytes = 0;
    for (const size_t id : batch.ids) {
        if (id >= size()) {
            throw std::out_of_range("AAStore MMseqs ID out of range");
        }
        if (!result.positions.insert_if_absent(
                id, batch.physicalIds.size())) {
            continue;
        }
        const size_t physicalId = store_id_for_mmseqs_id(id);
        const uint64_t length = store_->sequence_length(physicalId);
        if (length >= std::numeric_limits<size_t>::max() - bytes) {
            throw std::length_error("AAStore thread batch exceeds address space");
        }
        batch.physicalIds.push_back(
            static_cast<aastore::SequenceId>(physicalId));
        result.fetched.offsets.push_back(bytes);
        result.fetched.lengths.push_back(length);
        bytes += static_cast<size_t>(length) + 1;
    }

    result.fetched.storage.resize_uninitialized(bytes);
    for (size_t i = 0; i < batch.physicalIds.size(); ++i) {
        char* output = result.fetched.storage.data() + result.fetched.offsets[i];
        output[result.fetched.lengths[i]] = '\0';
        batch.outputs.push_back(output);
    }
}

void AAStoreTargetReader::fetch_into(
        ThreadBatch& batch, size_t computeThreads) const {
    prepare_into(batch);
    aastore::ReadOptions options = readOptions_;
    options.threads = 1;
    options.io_queue_depth = threadBatchQueueDepth(computeThreads);
    batch.sequences.fetched.stats =
        store_->get_into(batch.physicalIds, batch.outputs, options);
}

// 汇总接入层使用的基本读取计数。
void AAStoreTargetReader::accumulateIoStats(
        aastore::BatchStats& total, const aastore::BatchStats& batch) {
    total.ranges += batch.ranges;
    total.read_bytes += batch.read_bytes;
    total.io_submissions += batch.io_submissions;
}

// 输出各计算线程的基本读取计数。
void AAStoreTargetReader::logIoStats(
        const std::vector<aastore::BatchStats>& workers) {
    aastore::BatchStats total;
    for (const auto& worker : workers) accumulateIoStats(total, worker);
    Debug(Debug::INFO) << "AAStore thread I/O: ranges=" << total.ranges
        << " read_bytes=" << total.read_bytes
        << " requests=" << total.io_submissions
        << "\n";
}

size_t AAStoreTargetReader::store_id_for_mmseqs_id(
        size_t mmseqsId) const {
    if (!store_) throw std::logic_error("AAStore reader is not open");
    if (hasIdMap_) {
        return static_cast<size_t>(mmseqsToAAStoreId_.at(mmseqsId));
    }
    if (mmseqsId >= store_->size()) {
        throw std::out_of_range("AAStore MMseqs ID out of range");
    }
    return mmseqsId;
}

uint64_t AAStoreTargetReader::size() const {
    if (!store_) return 0;
    return hasIdMap_ ? mmseqsToAAStoreId_.size() : store_->size();
}

uint64_t AAStoreTargetReader::totalLength() const {
    if (!store_) return 0;
    if (!hasIdMap_) return store_->total_residues();
    updateViewStatistics();
    return viewTotalLength_;
}

uint64_t AAStoreTargetReader::sequenceLength(size_t mmseqsId) const {
    return store_->sequence_length(store_id_for_mmseqs_id(mmseqsId));
}

void AAStoreTargetReader::updateViewStatistics() const {
    if (viewStatisticsKnown_) return;
    viewTotalLength_ = 0;
    viewMaxLength_ = 0;
    if (store_) {
        for (size_t id = 0; id < size(); ++id) {
            const uint64_t length = sequenceLength(id);
            viewTotalLength_ = aastore_memory::add_bytes(viewTotalLength_, length);
            viewMaxLength_ = std::max(viewMaxLength_, length);
        }
    }
    viewStatisticsKnown_ = true;
}

uint64_t AAStoreTargetReader::maxSequenceLength() const {
    updateViewStatistics();
    return viewMaxLength_;
}

size_t AAStoreTargetReader::getId(unsigned int key) const {
    if (!store_) return UINT_MAX;
    if (!hasIdMap_) return key < store_->size() ? static_cast<size_t>(key) : UINT_MAX;
    if (keyOrder_.empty()) {
        const auto found = std::lower_bound(mmseqsToAAStoreId_.begin(), mmseqsToAAStoreId_.end(), key);
        return found != mmseqsToAAStoreId_.end() && *found == key
            ? static_cast<size_t>(found - mmseqsToAAStoreId_.begin()) : UINT_MAX;
    }
    const auto found = std::lower_bound(keyOrder_.begin(), keyOrder_.end(), key,
        [&](uint32_t local, uint32_t sought) { return mmseqsToAAStoreId_[local] < sought; });
    return found != keyOrder_.end() && mmseqsToAAStoreId_[*found] == key ? *found : UINT_MAX;
}

unsigned int AAStoreTargetReader::getDbKey(size_t mmseqsId) const {
    const size_t physical = store_id_for_mmseqs_id(mmseqsId);
    if (physical > UINT_MAX) throw std::out_of_range("AAStore key exceeds MMseqs key range");
    return static_cast<unsigned int>(physical);
}

void AAStoreTargetReader::mapFromMMseqsKeys(
        const std::vector<uint32_t>& keys) {
    if (!enabled() || hasIdMap_) return;
    bool identity = keys.size() == store_->size();
    for (size_t id = 0; id < keys.size(); ++id) {
        const size_t storeId = static_cast<size_t>(keys[id]);
        if (storeId >= store_->size()) {
            throw std::out_of_range(
                "MMseqs key exceeds AAStore physical ID range");
        }
        identity = identity && storeId == id;
    }
    if (identity) return;
    mmseqsToAAStoreId_.assign(keys.begin(), keys.end());
    hasIdMap_ = true;
    viewStatisticsKnown_ = false;
    if (!std::is_sorted(keys.begin(), keys.end())) {
        if (keys.size() > UINT_MAX) throw std::out_of_range("AAStore view exceeds MMseqs ID range");
        keyOrder_.resize(keys.size());
        std::iota(keyOrder_.begin(), keyOrder_.end(), uint32_t{0});
        std::stable_sort(keyOrder_.begin(), keyOrder_.end(),
            [&](uint32_t left, uint32_t right) { return keys[left] < keys[right]; });
    }
}

void AAStoreTargetReader::validateSize(size_t mmseqsSize) const {
    if (!enabled()) return;
    if (size() != mmseqsSize) {
        throw std::runtime_error(
            "AAStore and MMseqs target database sizes differ: aastore=" +
            std::to_string(size()) + " mmseqs=" +
            std::to_string(mmseqsSize));
    }
}

void AAStoreTargetReader::get(
        size_t mmseqsId, std::string& output) const {
    if (!fullMemory()) {
        throw std::logic_error(
            "direct AAStore reads require FullMemory");
    }
    store_->get_into(
        static_cast<aastore::SequenceId>(
            store_id_for_mmseqs_id(mmseqsId)),
        output);
}
