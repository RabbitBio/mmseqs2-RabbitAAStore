// 文件职责：独立于宿主内核验证 READ、READV 和能力缺失的兼容路径。
// 替身在完成阶段才读取 iovec，覆盖旧内核的生命周期和短读要求。
#include "../src/io_uring_backend.hpp"
#include "aastore/builder.hpp"
#include "aastore/store.hpp"
#include <liburing.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

struct AastoreTestRing {
    unsigned capacity = 0;
    std::deque<io_uring_sqe> prepared;
    std::deque<io_uring_sqe> pending;
    std::deque<io_uring_cqe> completed;
};

namespace {
struct Configuration {
    bool probe_available = true;
    bool read = true;
    bool readv = true;
    int init_error = 0;
    int read_error = 0;
    size_t maximum_read = 3;
};
struct Counters {
    size_t initialized = 0;
    size_t probes = 0;
    size_t reads = 0;
    size_t readvs = 0;
    size_t completed = 0;
    size_t closed = 0;
    bool aliased_vectors = false;
};
Configuration configuration;
Counters counters;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 每次场景使用新线程，避免不同模拟内核共享线程本地 ring。
template <typename Function>
void run_case(const char* name, Configuration options, Function function) {
    configuration = options;
    counters = {};
    std::exception_ptr error;
    std::thread worker([&] {
        try { function(); }
        catch (...) { error = std::current_exception(); }
    });
    worker.join();
    if (error) std::rethrow_exception(error);
    check(counters.initialized == counters.closed,
          "a simulated ring was not closed");
    std::cout << name << " passed\n";
}

struct File {
    int descriptor = -1;
    std::vector<uint8_t> bytes;
    File() : bytes(64) {
        char path[] = "/tmp/aastore_uring_compat_XXXXXX";
        descriptor = ::mkstemp(path);
        check(descriptor >= 0, "mkstemp failed");
        ::unlink(path);
        for (size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<uint8_t>(index * 17 + 5);
        check(::write(descriptor, bytes.data(), bytes.size()) ==
                  static_cast<ssize_t>(bytes.size()), "fixture write failed");
    }
    ~File() { if (descriptor >= 0) ::close(descriptor); }
};

struct CompletionContext {
    const File* file = nullptr;
    const std::vector<aastore::ByteRange>* ranges = nullptr;
    size_t callbacks = 0;
    bool throw_callback = false;
};

void complete_range(void* opaque, size_t worker, size_t index,
                    const uint8_t* data, uint64_t size) {
    auto& context = *static_cast<CompletionContext*>(opaque);
    check(worker == 0, "unexpected completion worker");
    const auto& range = (*context.ranges)[index];
    check(size == range.end - range.begin, "wrong completed range length");
    check(std::memcmp(data, context.file->bytes.data() + range.begin,
                      static_cast<size_t>(size)) == 0,
          "wrong completed range bytes");
    ++context.callbacks;
    if (context.throw_callback) throw std::logic_error("callback sentinel");
}

// 提交与完成之间使用栈空间，防止栈上临时 iovec 偶然保持旧值。
bool clobber_stack(void*, size_t) {
    volatile unsigned char scratch[4096];
    for (size_t index = 0; index < sizeof(scratch); ++index)
        scratch[index] = static_cast<unsigned char>(index);
    return false;
}

bool read_fixture(const File& file, CompletionContext& context,
                  aastore::IoUringStats& stats, std::string& reason) {
    return aastore::read_ranges_io_uring(
        file.descriptor, *context.ranges, 1, 3, clobber_stack, nullptr,
        complete_range, &context, stats, reason, true);
}

template <typename Function>
void expect_runtime_error(Function function, const char* text) {
    try { function(); }
    catch (const std::runtime_error& error) {
        check(std::string(error.what()).find(text) != std::string::npos,
              "wrong runtime error");
        return;
    }
    throw std::runtime_error("expected a runtime error");
}

void check_success(bool expected_readv) {
    File file;
    const std::vector<aastore::ByteRange> ranges{
        {0, 13, 0, 0}, {17, 30, 0, 0}, {31, 64, 0, 0}};
    CompletionContext context{&file, &ranges};
    aastore::IoUringStats stats;
    std::string reason;
    check(read_fixture(file, context, stats, reason), "backend unavailable");
    check(context.callbacks == ranges.size(), "missing completion callbacks");
    check(stats.submissions > ranges.size(), "short reads were not continued");
    check(stats.submissions == stats.completions, "pending reads remain");
    check(!counters.aliased_vectors, "concurrent READV requests share iovec");
    check(expected_readv ? counters.reads == 0 && counters.readvs > 0
                         : counters.reads > 0 && counters.readvs == 0,
          "wrong read opcode selected");
    const size_t initializations = counters.initialized;
    context.callbacks = 0;
    stats = {};
    check(read_fixture(file, context, stats, reason), "ring reuse failed");
    check(counters.initialized == initializations,
          "successful reads did not reuse the ring");
    check(context.callbacks == ranges.size(), "ring reuse lost callbacks");
}

struct StoreFixture {
    std::filesystem::path root;
    std::filesystem::path store;
    StoreFixture() {
        char path[] = "/tmp/aastore_uring_store_XXXXXX";
        char* created = ::mkdtemp(path);
        check(created != nullptr, "mkdtemp failed");
        root = created;
        store = root / "store";
        const auto fasta = root / "input.fa";
        std::ofstream output(fasta);
        output << ">first first-description\nACDEFGHIKLMNPQRSTVWY\n"
                  ">second second-description\nWYACDEFG\n";
        output.close();
        aastore::build_fasta(fasta, store);
    }
    ~StoreFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

void check_public_store(bool available) {
    StoreFixture fixture;
    aastore::Store store(fixture.store);
    aastore::ReadOptions options;
    options.threads = 1;
    const std::vector<aastore::SequenceId> ids{1, 0};
    const auto sequences = store.get(ids, options);
    check(sequences[0] == "WYACDEFG" &&
          sequences[1] == "ACDEFGHIKLMNPQRSTVWY", "Auto returned wrong sequences");
    check(sequences.stats.io_uring == available, "Auto used the wrong backend");
    const auto single = store.get(1, options);
    check(single.size() == 1 && single[0] == "WYACDEFG" &&
          single.stats.io_uring == available, "single Auto get used the wrong backend");
    const auto records = store.batch_get_records(
        ids, aastore::RecordFields::All, options);
    check(records.name(0) == "second" && records.name(1) == "first",
          "Auto returned wrong names");
    check(records.description(0) == "second-description" &&
          records.description(1) == "first-description",
          "Auto returned wrong descriptions");
    check(records.sequence(0) == "WYACDEFG", "Auto returned wrong record sequence");
    check(records.names.stats.io_uring == available &&
          records.descriptions.stats.io_uring == available,
          "text columns used the wrong backend");
    options.io_backend = aastore::IoBackend::IoUring;
    if (available) {
        check(store.get(ids, options)[0] == "WYACDEFG",
              "explicit io_uring returned wrong data");
        check(store.get(1, options)[0] == "WYACDEFG",
              "single explicit io_uring returned wrong data");
    } else {
        expect_runtime_error([&] { store.get(ids, options); }, "io_uring unavailable");
        expect_runtime_error([&] { store.get(1, options); }, "io_uring unavailable");
        expect_runtime_error([&] {
            store.batch_get_records(ids, aastore::RecordFields::Name, options);
        }, "io_uring unavailable for text column");
    }
}

// 完成阶段才访问请求地址，模拟没有 SUBMIT_STABLE 的旧内核。
int produce_completion(io_uring* ring, io_uring_cqe** completion) {
    auto& state = *ring->state;
    if (state.completed.empty()) {
        if (state.pending.empty()) return -EAGAIN;
        const io_uring_sqe request = state.pending.front();
        state.pending.pop_front();
        void* destination = nullptr;
        size_t length = 0;
        int result = 0;
        if (request.opcode == IORING_OP_READ) {
            if (!configuration.read) result = -EINVAL;
            destination = const_cast<void*>(request.address);
            length = request.length;
        } else {
            check(request.opcode == IORING_OP_READV, "unexpected opcode");
            check(request.length == 1, "READV must submit one vector");
            if (!configuration.readv) result = -EINVAL;
            const auto* vector = static_cast<const iovec*>(request.address);
            destination = vector->iov_base;
            length = vector->iov_len;
        }
        if (result == 0 && configuration.read_error != 0)
            result = -configuration.read_error;
        if (result == 0) {
            const ssize_t read = ::pread(request.fd, destination,
                std::min(length, configuration.maximum_read), request.offset);
            result = read < 0 ? -errno : static_cast<int>(read);
        }
        state.completed.push_back({request.user_data, result});
        ++counters.completed;
    }
    *completion = &state.completed.front();
    return 0;
}
}  // namespace

extern "C" {
int io_uring_queue_init_params(unsigned capacity, io_uring* ring, io_uring_params*) {
    if (configuration.init_error != 0) return -configuration.init_error;
    ring->state = new AastoreTestRing;
    ring->state->capacity = capacity;
    ++counters.initialized;
    return 0;
}
void io_uring_queue_exit(io_uring* ring) {
    check(ring->state != nullptr, "double ring close");
    delete ring->state;
    ring->state = nullptr;
    ++counters.closed;
}
io_uring_probe* io_uring_get_probe_ring(io_uring*) {
    ++counters.probes;
    if (!configuration.probe_available) { errno = EINVAL; return nullptr; }
    auto* probe = static_cast<io_uring_probe*>(std::malloc(sizeof(io_uring_probe)));
    check(probe != nullptr, "probe allocation failed");
    *probe = {configuration.read, configuration.readv};
    return probe;
}
int io_uring_opcode_supported(io_uring_probe* probe, int opcode) {
    return opcode == IORING_OP_READ ? probe->read : probe->readv;
}
io_uring_sqe* io_uring_get_sqe(io_uring* ring) {
    auto& state = *ring->state;
    if (state.prepared.size() >= state.capacity) return nullptr;
    state.prepared.emplace_back();
    return &state.prepared.back();
}
void io_uring_prep_read(io_uring_sqe* sqe, int fd, void* data,
                        unsigned length, off_t offset) {
    *sqe = {IORING_OP_READ, fd, data, length, offset, 0};
    ++counters.reads;
}
void io_uring_prep_readv(io_uring_sqe* sqe, int fd, const iovec* vector,
                         unsigned count, off_t offset) {
    *sqe = {IORING_OP_READV, fd, vector, count, offset, 0};
    ++counters.readvs;
}
int io_uring_submit(io_uring* ring) {
    auto& state = *ring->state;
    const size_t count = state.prepared.size();
    for (const auto& request : state.prepared) {
        if (request.opcode == IORING_OP_READV) {
            for (const auto& pending : state.pending) {
                if (pending.opcode == IORING_OP_READV &&
                    pending.address == request.address &&
                    pending.user_data != request.user_data)
                    counters.aliased_vectors = true;
            }
        }
        state.pending.push_back(request);
    }
    state.prepared.clear();
    return static_cast<int>(count);
}
int io_uring_wait_cqe(io_uring* ring, io_uring_cqe** completion) {
    return produce_completion(ring, completion);
}
int io_uring_peek_cqe(io_uring* ring, io_uring_cqe** completion) {
    return produce_completion(ring, completion);
}
unsigned io_uring_cq_ready(io_uring* ring) {
    return static_cast<unsigned>(ring->state->completed.size());
}
void io_uring_cqe_seen(io_uring* ring, io_uring_cqe* completion) {
    check(!ring->state->completed.empty() &&
          completion == &ring->state->completed.front(), "invalid completion");
    ring->state->completed.pop_front();
}
}

int main() {
    try {
#if defined(AASTORE_URING_HAS_READ) && defined(AASTORE_URING_HAS_PROBE)
        run_case("READ preferred", {}, [] { check_success(false); });
#else
        run_case("baseline READV without optional APIs", {}, [] {
            check_success(true);
            check_public_store(true);
            check(counters.probes == 0, "baseline called an optional probe API");
        });
#endif
        Configuration legacy;
        legacy.read = false;
#if defined(AASTORE_URING_HAS_PROBE)
        run_case("READV only", legacy, [] { check_success(true); check_public_store(true); });
#endif
        legacy.probe_available = false;
#if defined(AASTORE_URING_HAS_PROBE)
        run_case("no PROBE", legacy, [] { check_success(true); check_public_store(true); });
        Configuration unsupported;
        unsupported.read = false;
        unsupported.readv = false;
        run_case("neither opcode", unsupported, [] {
            File file;
            const std::vector<aastore::ByteRange> ranges{{0, 13, 0, 0}};
            CompletionContext context{&file, &ranges};
            aastore::IoUringStats stats;
            std::string reason;
            check(!read_fixture(file, context, stats, reason),
                  "unsupported opcodes were accepted");
            check(!reason.empty() && context.callbacks == 0 &&
                  counters.reads == 0 && counters.readvs == 0,
                  "unsupported kernel submitted reads");
            check_public_store(false);
        });
#endif
        Configuration denied;
        denied.init_error = EPERM;
        run_case("initialization denied", denied, [] { check_public_store(false); });
        Configuration failed;
        failed.read = false;
        failed.read_error = EIO;
        run_case("read error and recovery", failed, [] {
            File file;
            const std::vector<aastore::ByteRange> ranges{
                {0, 13, 0, 0}, {17, 30, 0, 0}, {31, 64, 0, 0}};
            CompletionContext context{&file, &ranges};
            aastore::IoUringStats stats;
            std::string reason;
            expect_runtime_error([&] { read_fixture(file, context, stats, reason); },
                                 std::strerror(EIO));
            check(context.callbacks == 0, "failed read reached a callback");
            StoreFixture fixture;
            aastore::Store store(fixture.store);
            aastore::ReadOptions options;
            options.threads = 1;
            expect_runtime_error([&] { store.get({0}, options); }, std::strerror(EIO));
            expect_runtime_error([&] {
                store.batch_get_records({0}, aastore::RecordFields::Name, options);
            }, std::strerror(EIO));
            configuration.read_error = 0;
            check_success(true);
        });
        run_case("callback error and recovery", legacy, [] {
            File file;
            const std::vector<aastore::ByteRange> ranges{
                {0, 13, 0, 0}, {17, 30, 0, 0}, {31, 64, 0, 0}};
            CompletionContext context{&file, &ranges, 0, true};
            aastore::IoUringStats stats;
            std::string reason;
            bool thrown = false;
            try { read_fixture(file, context, stats, reason); }
            catch (const std::logic_error& error) {
                thrown = std::string(error.what()) == "callback sentinel";
            }
            check(thrown, "callback exception was swallowed or replaced");
            check(counters.initialized == counters.closed,
                  "callback failure did not destroy the ring");
            check_success(true);
        });
        std::cout << "io_uring kernel compatibility and public fallback passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
