// 文件职责：顺序读取 FASTA 行，处理跨缓冲长行并检查、规范化序列字符。
// 运行时选择标量、AVX2 或 AVX-512 扫描器，同时定位换行并判断是否需要转换。
#include "fasta_reader.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define AASTORE_FASTA_READER_X86_SIMD 1
#else
#define AASTORE_FASTA_READER_X86_SIMD 0
#endif

namespace aastore {
namespace {

constexpr size_t kReadBufferBytes = 4 * 1024 * 1024;

// 创建字符转换表：保留大写与特殊符号，小写转大写，空白删除，其他字符变为 X。
constexpr std::array<char, 256> make_normalize_table() {
    std::array<char, 256> table{};
    for (char& value : table) value = 'X';
    for (unsigned value = 'A'; value <= 'Z'; ++value) {
        table[value] = static_cast<char>(value);
    }
    for (unsigned value = 'a'; value <= 'z'; ++value) {
        table[value] = static_cast<char>(value - 'a' + 'A');
    }
    for (unsigned value = '\t'; value <= '\r'; ++value) table[value] = 0;
    table[static_cast<unsigned>(' ')] = 0;
    table[static_cast<unsigned>('*')] = '*';
    table[static_cast<unsigned>('-')] = '-';
    return table;
}

constexpr std::array<char, 256> kNormalizeTable = make_normalize_table();

// 判断字符是否已经属于无需转换的大写字母、星号或连字符。
bool is_normalized_residue(unsigned char value) {
    return (value >= 'A' && value <= 'Z') || value == '*' || value == '-';
}

// 扫描一段输入得到的换行位置及规范化状态，不拥有输入数据。
struct SequenceChunkScan {
    // 输入段中首个换行的借用指针；nullptr 表示本段未找到换行。
    const char* newline = nullptr;
    // 换行前的字符（无换行时为整段）是否无需大小写或非法字符转换。
    bool normalized = true;
};

// 逐字节寻找本块首个换行，并判断换行前字符是否都已规范化。
SequenceChunkScan scan_sequence_chunk_scalar(const char* input,
                                             size_t length) {
    bool normalized = true;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char value = static_cast<unsigned char>(input[i]);
        if (value == '\n') return {input + i, normalized};
        normalized = normalized && is_normalized_residue(value);
    }
    return {nullptr, normalized};
}

#if AASTORE_FASTA_READER_X86_SIMD && \
    (defined(__GNUC__) || defined(__clang__))
// 使用 AVX2 同时检测 32 个字符的换行和合法性，尾部交给标量扫描器。
__attribute__((target("avx2")))
SequenceChunkScan scan_sequence_chunk_avx2(const char* input,
                                           size_t length) {
    const __m256i before_a = _mm256_set1_epi8('A' - 1);
    const __m256i after_z = _mm256_set1_epi8('Z' + 1);
    const __m256i stop = _mm256_set1_epi8('*');
    const __m256i gap = _mm256_set1_epi8('-');
    const __m256i newline = _mm256_set1_epi8('\n');
    bool normalized = true;
    size_t offset = 0;
    for (; offset + 32 <= length; offset += 32) {
        const __m256i characters = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(input + offset));
        const __m256i uppercase = _mm256_and_si256(
            _mm256_cmpgt_epi8(characters, before_a),
            _mm256_cmpgt_epi8(after_z, characters));
        const __m256i special = _mm256_or_si256(
            _mm256_cmpeq_epi8(characters, stop),
            _mm256_cmpeq_epi8(characters, gap));
        const uint32_t valid_mask = static_cast<uint32_t>(
            _mm256_movemask_epi8(_mm256_or_si256(uppercase, special)));
        const uint32_t newline_mask = static_cast<uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(characters, newline)));
        if (newline_mask != 0) {
            const unsigned newline_offset =
                static_cast<unsigned>(__builtin_ctz(newline_mask));
            const uint32_t before_newline = newline_offset == 0
                ? 0
                : (uint32_t{1} << newline_offset) - 1;
            normalized = normalized &&
                         ((~valid_mask & before_newline) == 0);
            return {input + offset + newline_offset, normalized};
        }
        normalized = normalized && valid_mask == UINT32_MAX;
    }
    SequenceChunkScan tail =
        scan_sequence_chunk_scalar(input + offset, length - offset);
    tail.normalized = normalized && tail.normalized;
    return tail;
}

// 使用 AVX-512 同时检测 64 个字符，剩余部分交给 AVX2 扫描器。
__attribute__((target("avx512f,avx512bw")))
SequenceChunkScan scan_sequence_chunk_avx512(const char* input,
                                             size_t length) {
    const __m512i before_a = _mm512_set1_epi8('A' - 1);
    const __m512i after_z = _mm512_set1_epi8('Z' + 1);
    const __m512i stop = _mm512_set1_epi8('*');
    const __m512i gap = _mm512_set1_epi8('-');
    const __m512i newline = _mm512_set1_epi8('\n');
    bool normalized = true;
    size_t offset = 0;
    for (; offset + 64 <= length; offset += 64) {
        const __m512i characters = _mm512_loadu_si512(input + offset);
        const uint64_t uppercase = static_cast<uint64_t>(
            _mm512_cmpgt_epi8_mask(characters, before_a) &
            _mm512_cmpgt_epi8_mask(after_z, characters));
        const uint64_t special = static_cast<uint64_t>(
            _mm512_cmpeq_epi8_mask(characters, stop) |
            _mm512_cmpeq_epi8_mask(characters, gap));
        const uint64_t valid_mask = uppercase | special;
        const uint64_t newline_mask = static_cast<uint64_t>(
            _mm512_cmpeq_epi8_mask(characters, newline));
        if (newline_mask != 0) {
            const unsigned newline_offset = static_cast<unsigned>(
                __builtin_ctzll(newline_mask));
            const uint64_t before_newline = newline_offset == 0
                ? 0
                : (uint64_t{1} << newline_offset) - 1;
            normalized = normalized &&
                         ((~valid_mask & before_newline) == 0);
            return {input + offset + newline_offset, normalized};
        }
        normalized = normalized && valid_mask == UINT64_MAX;
    }
    SequenceChunkScan tail =
        scan_sequence_chunk_avx2(input + offset, length - offset);
    tail.normalized = normalized && tail.normalized;
    return tail;
}
#endif

using SequenceChunkScanner = SequenceChunkScan (*)(const char*, size_t);

// 探测本机指令集，选择当前 CPU 可安全执行的最快可用扫描实现。
SequenceChunkScanner detect_sequence_chunk_scanner() {
#if AASTORE_FASTA_READER_X86_SIMD && \
    (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx2")) {
        return scan_sequence_chunk_avx512;
    }
    if (__builtin_cpu_supports("avx2")) return scan_sequence_chunk_avx2;
#endif
    return scan_sequence_chunk_scalar;
}

}  // namespace

// 有缓冲的 FASTA 行读取状态；普通行借用读缓冲，跨块长行存入拼接区。
class FastaLineReader::Impl {
public:
    // 打开顺序输入文件，分配读取缓冲并向内核提供顺序访问提示。
    explicit Impl(const std::filesystem::path& path)
        : buffer_(kReadBufferBytes) {
        descriptor_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor_ < 0) {
            throw std::runtime_error("cannot open FASTA: " + path.string());
        }
#ifdef POSIX_FADV_SEQUENTIAL
        (void)::posix_fadvise(descriptor_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    }

    // 关闭内部持有的 FASTA 文件描述符。
    ~Impl() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    // 返回下一行的借用视图，必要时拼接跨块内容，并累计序列字符的规范化状态。
    bool next(std::string_view& line, bool& normalized_sequence) {
        spill_.clear();
        bool line_kind_known = false;
        bool sequence_line = false;
        normalized_sequence = true;
        while (true) {
            if (begin_ < end_) {
                const char* const first = buffer_.data() + begin_;
                const size_t available = end_ - begin_;
                if (!line_kind_known) {
                    sequence_line = *first != '>';
                    line_kind_known = true;
                }
                const char* newline = nullptr;
                if (sequence_line) {
                    const SequenceChunkScan scan =
                        sequence_scanner_(first, available);
                    newline = scan.newline;
                    normalized_sequence =
                        normalized_sequence && scan.normalized;
                } else {
                    newline = static_cast<const char*>(
                        std::memchr(first, '\n', available));
                }
                if (newline != nullptr) {
                    const size_t length =
                        static_cast<size_t>(newline - first);
                    begin_ += length + 1;
                    if (spill_.empty()) {
                        line = std::string_view(first, length);
                    } else {
                        spill_.append(first, length);
                        line = spill_;
                    }
                    return true;
                }
                spill_.append(first, available);
                begin_ = end_;
            }

            if (eof_) {
                if (spill_.empty()) return false;
                line = spill_;
                return true;
            }

            ssize_t bytes = 0;
            do {
                bytes = ::read(descriptor_, buffer_.data(), buffer_.size());
            } while (bytes < 0 && errno == EINTR);
            if (bytes < 0) {
                throw std::runtime_error("failed while reading FASTA: " +
                                         std::string(std::strerror(errno)));
            }
            begin_ = 0;
            end_ = static_cast<size_t>(bytes);
            eof_ = bytes == 0;
        }
    }

private:
    // 本对象持有的 FASTA 输入文件描述符，析构时关闭。
    int descriptor_ = -1;
    // read 系统调用填充的固定大小字节缓冲。
    std::vector<char> buffer_;
    // 跨越多个读缓冲的行片段；下一次 next 调用时清空。
    std::string spill_;
    // buffer_ 中下一未消费字节的下标。
    size_t begin_ = 0;
    // buffer_ 有效数据末尾的下一下标，未消费区间为 [begin_, end_)。
    size_t end_ = 0;
    // read 是否已经返回零；此时仍可能需要返回 spill_ 中的最后一行。
    bool eof_ = false;
    // 根据 CPU 能力选定的扫描函数，同时定位换行和检查规范化状态。
    SequenceChunkScanner sequence_scanner_ =
        detect_sequence_chunk_scanner();
};

// 创建持有文件描述符和行缓冲的内部读取实现。
FastaLineReader::FastaLineReader(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {}

// 释放内部读取器及其输入文件和缓冲。
FastaLineReader::~FastaLineReader() = default;

// 获取下一行及其规范化标志；视图在下次读取前有效。
bool FastaLineReader::next(std::string_view& line,
                           bool& normalized_sequence) {
    return impl_->next(line, normalized_sequence);
}

// 按转换表删除空白、统一大小写并替换无效字符，结果写入可复用字符串。
void normalize_sequence_line(std::string_view line,
                             std::string& normalized_line) {
    normalized_line.resize(line.size());
    size_t output = 0;
    for (const unsigned char value : line) {
        const char normalized = kNormalizeTable[value];
        if (normalized != 0) normalized_line[output++] = normalized;
    }
    normalized_line.resize(output);
}

}  // namespace aastore
