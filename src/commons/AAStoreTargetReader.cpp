#include "AAStoreTargetReader.h"

#include "AAStoreMemoryBudget.h"
#include "Debug.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {

constexpr uint64_t kMinimumPrefixBytes = UINT64_C(64) * 1024 * 1024;

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

uint64_t environment_bytes(const char* name, uint64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') return fallback;
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    return errno == 0 && end != text && *end == '\0'
        ? static_cast<uint64_t>(value) : fallback;
}

aastore_memory::Budget automatic_storage_budget(
        const std::string& database, size_t computeThreads,
        uint64_t stageMemoryReserve) {
    const MemoryAvailability memory = memory_availability();
    const uint64_t indexReserve = database.empty()
        ? 0 : file_bytes(database + ".index");
    const uint64_t perThreadBytes = environment_bytes(
        "MMSEQS_AASTORE_THREAD_BATCH_BYTES", UINT64_C(64) * 1024 * 1024);
    const uint64_t threadReserve = saturating_multiply(
        std::max<size_t>(1, computeThreads), perThreadBytes);
    aastore_memory::Budget budget = aastore_memory::plan(
        memory.limit, memory.available, indexReserve, threadReserve,
        stageMemoryReserve);
    budget.cgroupLimited = memory.cgroupLimited;
    return budget;
}

size_t positive_environment_size(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    size_t parsed = 0;
    for (const char* digit = value; *digit; ++digit) {
        if (*digit < '0' || *digit > '9' ||
            parsed > (std::numeric_limits<size_t>::max() -
                      static_cast<size_t>(*digit - '0')) / 10) {
            throw std::invalid_argument(std::string("invalid ") + name);
        }
        parsed = parsed * 10 + static_cast<size_t>(*digit - '0');
    }
    if (parsed == 0) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return parsed;
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
    const uint64_t residentBytes = store ? store->resident_bytes() : 0;
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
    const fs::path localStore = database + ".aastore";
    if (fs::is_regular_file(localStore / "packed.bin") &&
        fs::is_regular_file(localStore / "meta.bin")) {
        return localStore.string();
    }

    const fs::path locator = database + ".aastore.path";
    std::ifstream input(locator, std::ios::binary);
    if (!input) return std::string();

    std::string path;
    std::getline(input, path);
    if (!path.empty() && path.back() == '\r') path.pop_back();
    if (path.empty() ||
        !fs::is_regular_file(fs::path(path) / "packed.bin") ||
        !fs::is_regular_file(fs::path(path) / "meta.bin")) {
        throw std::runtime_error(
            "invalid AAStore database association: " + locator.string());
    }
    return path;
}

void AAStoreTargetReader::associateDatabase(const std::string& database,
                                            const std::string& store) {
    namespace fs = std::filesystem;
    const fs::path localStore = database + ".aastore";
    const fs::path resolvedStore =
        fs::absolute(fs::path(store)).lexically_normal();
    const fs::path resolvedLocal =
        fs::absolute(localStore).lexically_normal();
    const fs::path locator = database + ".aastore.path";
    if (resolvedStore == resolvedLocal) {
        std::error_code error;
        fs::remove(locator, error);
        return;
    }

    const fs::path temporary =
        locator.string() + ".tmp." + std::to_string(::getpid());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "cannot create AAStore association: " + locator.string());
    }
    output << resolvedStore.string() << '\n';
    output.close();
    if (!output) {
        std::error_code error;
        fs::remove(temporary, error);
        throw std::runtime_error(
            "failed to write AAStore association: " + locator.string());
    }
    std::error_code error;
    fs::rename(temporary, locator, error);
    if (error) {
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);
        throw fs::filesystem_error(
            "cannot install AAStore association", locator, error);
    }
}

void AAStoreTargetReader::open(
        const std::string& path,
        const aastore::StoreOptions& storeOptions,
        const aastore::ReadOptions& readOptions) {
    installStore(std::make_unique<aastore::Store>(path, storeOptions));
    mmseqsToAAStoreId_.clear();
    hasIdMap_ = false;
    readOptions_ = readOptions;
}

bool AAStoreTargetReader::openForDatabase(
        const std::string& database, size_t computeThreads,
        uint64_t stageMemoryReserve) {
    const char* environmentPath = std::getenv("MMSEQS_AASTORE");
    const std::string path = environmentPath != nullptr && environmentPath[0] != '\0'
        ? environmentPath : associatedStorePath(database);
    if (path.empty()) return false;

    aastore::StoreOptions options;
    size_t cacheBytes = 0;
    const char* cacheValue = std::getenv("MMSEQS_AASTORE_CACHE_BYTES");
    const bool cacheExplicit = cacheValue != nullptr && cacheValue[0] != '\0';
    if (cacheExplicit) {
        cacheBytes = static_cast<size_t>(
            std::strtoull(cacheValue, nullptr, 10));
    }
    options.storage_mode = cacheBytes == 0
        ? aastore::StorageMode::ByteRange
        : aastore::StorageMode::PrefixMemory;

    const char* fullMemoryValue =
        std::getenv("MMSEQS_AASTORE_FULL_MEMORY");
    const bool fullMemoryExplicit =
        fullMemoryValue != nullptr && fullMemoryValue[0] != '\0';
    if (fullMemoryValue != nullptr &&
        std::string_view(fullMemoryValue) == "1") {
        options.storage_mode = aastore::StorageMode::FullMemory;
    }

    aastore_memory::Budget automaticBudget;
    const bool automaticMode = !cacheExplicit && !fullMemoryExplicit;
    if (automaticMode) {
        automaticBudget = automatic_storage_budget(
            database, computeThreads, stageMemoryReserve);
        const uint64_t packedBytes = file_bytes(
            std::filesystem::path(path) / "packed.bin");
        const uint64_t selectedBytes =
            std::min(automaticBudget.cacheBytes, packedBytes);
        if (packedBytes != 0 && selectedBytes >= packedBytes) {
            options.storage_mode = aastore::StorageMode::FullMemory;
            cacheBytes = 0;
        } else if (selectedBytes >= kMinimumPrefixBytes) {
            options.storage_mode = aastore::StorageMode::PrefixMemory;
            cacheBytes = static_cast<size_t>(std::min<uint64_t>(
                selectedBytes, std::numeric_limits<size_t>::max()));
        } else {
            options.storage_mode = aastore::StorageMode::ByteRange;
            cacheBytes = 0;
        }
    }

    options.threads = positive_environment_size(
        "MMSEQS_AASTORE_PRELOAD_THREADS", 8);
    options.cache_bytes = cacheBytes;
    options.resident_names = false;
    options.resident_descriptions = false;
    installStore(std::make_unique<aastore::Store>(
        std::filesystem::path(path), options));
    mmseqsToAAStoreId_.clear();
    hasIdMap_ = false;

    readOptions_.threads = 1;
    readOptions_.merge_gap_bytes = positive_environment_size(
        "MMSEQS_AASTORE_MERGE_GAP_BYTES", 4096);
    readOptions_.max_merged_range_bytes = positive_environment_size(
        "MMSEQS_AASTORE_MAX_RANGE_BYTES", 16 * 1024);
    readOptions_.max_exact_merged_range_bytes = positive_environment_size(
        "MMSEQS_AASTORE_MAX_EXACT_RANGE_BYTES", 1024 * 1024);
    readOptions_.io_backend = aastore::IoBackend::IoUring;
    const char* ioBackend = std::getenv("MMSEQS_AASTORE_IO_BACKEND");
    if (ioBackend != nullptr && ioBackend[0] != '\0') {
        if (std::string_view(ioBackend) == "pread") {
            readOptions_.io_backend = aastore::IoBackend::Pread;
        } else if (std::string_view(ioBackend) != "io_uring") {
            throw std::runtime_error(
                "MMSEQS_AASTORE_IO_BACKEND must be io_uring or pread");
        }
    }

    Debug(Debug::INFO)
        << "AAStore target enabled: " << path
        << " io_backend="
        << (readOptions_.io_backend == aastore::IoBackend::Pread
                ? "pread" : "io_uring")
        << " threads=1"
        << " merge_gap=" << readOptions_.merge_gap_bytes
        << " max_range=" << readOptions_.max_merged_range_bytes
        << " cache_bytes=" << cacheBytes
        << " storage_mode=" << storage_mode_name(options.storage_mode)
        << " mode_source="
        << (automaticMode ? "automatic" : "environment")
        << " memory_limit=" << automaticBudget.memoryLimit
        << " memory_available=" << automaticBudget.memoryAvailable
        << " memory_reserve=" << automaticBudget.memoryReserve
        << " memory_index_reserve=" << automaticBudget.indexReserve
        << " memory_buffer_reserve=" << automaticBudget.bufferReserve
        << " memory_safety_reserve=" << automaticBudget.safetyReserve
        << " memory_stage_reserve=" << automaticBudget.stageReserve
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

bool AAStoreTargetReader::enabled() const noexcept {
    return store_ != nullptr;
}

bool AAStoreTargetReader::fullMemory() const noexcept {
    return store_ &&
        store_->storage_mode() == aastore::StorageMode::FullMemory;
}

size_t AAStoreTargetReader::threadBatchBytes() const {
    return positive_environment_size(
        "MMSEQS_AASTORE_THREAD_BATCH_BYTES", 64 * 1024 * 1024);
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
        store_->batch_get_into(batch.physicalIds, batch.outputs, options);
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
    uint64_t total = 0;
    for (const aastore::SequenceId id : mmseqsToAAStoreId_) {
        total += store_->sequence_length(id);
    }
    return total;
}

uint64_t AAStoreTargetReader::sequenceLength(size_t mmseqsId) const {
    return store_->sequence_length(store_id_for_mmseqs_id(mmseqsId));
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
