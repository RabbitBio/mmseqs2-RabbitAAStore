#include "codec.hpp"

#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define AASTORE_X86_SIMD 1
#else
#define AASTORE_X86_SIMD 0
#endif

namespace aastore {
namespace {

enum class DecodeKernel : uint8_t {
    Scalar32,
    Avx2Bmi2,
    Avx512Vbmi2
};

uint8_t encode_stop_aware(char residue) {
    const uint8_t value = static_cast<uint8_t>(residue);
    const uint8_t bit = static_cast<uint8_t>((value >> 5) & 1);
    return static_cast<uint8_t>((value & 0x1F) ^ ((bit << 4) | bit));
}

void set_code(uint8_t* packed, uint64_t offset, uint8_t code) {
    const uint64_t bit_position = offset * 5;
    const size_t byte_position = static_cast<size_t>(bit_position >> 3);
    const unsigned shift = static_cast<unsigned>(bit_position & 7);
    const uint16_t value = static_cast<uint16_t>(code & 0x1F);
    packed[byte_position] |= static_cast<uint8_t>(value << shift);
    if (shift > 3) {
        packed[byte_position + 1] |=
            static_cast<uint8_t>(value >> (8 - shift));
    }
}

uint64_t load_5_bytes(const uint8_t* source) {
    return static_cast<uint64_t>(source[0]) |
           (static_cast<uint64_t>(source[1]) << 8) |
           (static_cast<uint64_t>(source[2]) << 16) |
           (static_cast<uint64_t>(source[3]) << 24) |
           (static_cast<uint64_t>(source[4]) << 32);
}

void store_5_bytes(uint8_t* output, uint64_t value) {
    std::memcpy(output, &value, 5);
}

void pack_8_scalar(const char* input, uint8_t* output) {
    const uint64_t packed =
        static_cast<uint64_t>(encode_stop_aware(input[0])) |
        (static_cast<uint64_t>(encode_stop_aware(input[1])) << 5) |
        (static_cast<uint64_t>(encode_stop_aware(input[2])) << 10) |
        (static_cast<uint64_t>(encode_stop_aware(input[3])) << 15) |
        (static_cast<uint64_t>(encode_stop_aware(input[4])) << 20) |
        (static_cast<uint64_t>(encode_stop_aware(input[5])) << 25) |
        (static_cast<uint64_t>(encode_stop_aware(input[6])) << 30) |
        (static_cast<uint64_t>(encode_stop_aware(input[7])) << 35);
    store_5_bytes(output, packed);
}

void pack_8_blocks_scalar(const char* input,
                          uint8_t* output,
                          size_t block_count) {
    for (size_t block = 0; block < block_count; ++block) {
        pack_8_scalar(input, output);
        input += 8;
        output += 5;
    }
}

#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("bmi2")))
void pack_8_blocks_bmi2(const char* input,
                        uint8_t* output,
                        size_t block_count) {
    constexpr uint64_t kLowFiveBits = 0x1F1F1F1F1F1F1F1FULL;
    constexpr uint64_t kByteLowBits = 0x0101010101010101ULL;
    for (size_t block = 0; block < block_count; ++block) {
        uint64_t characters = 0;
        std::memcpy(&characters, input, sizeof(characters));
        const uint64_t special = (characters >> 5) & kByteLowBits;
        characters ^= special | (special << 4);
        store_5_bytes(output, _pext_u64(characters, kLowFiveBits));
        input += 8;
        output += 5;
    }
}

__attribute__((target("avx512f,avx512bw,avx512vbmi2,bmi2")))
void pack_8_blocks_avx512_vbmi2(const char* input,
                                uint8_t* output,
                                size_t block_count) {
    constexpr __mmask64 kPackedBytes = 0x1F1F1F1F1F1F1F1FULL;
    const __m512i low_byte_bits = _mm512_set1_epi8(1);
    const __m512i low_five_bits = _mm512_set1_epi8(0x1F);
    const __m512i pair_weights = _mm512_set1_epi16(0x2001);
    const __m512i quad_weights = _mm512_set1_epi32(0x04000001);
    const __m512i low_twenty_bits = _mm512_set1_epi64(0xFFFFF);
    while (block_count >= 8) {
        __m512i characters = _mm512_loadu_si512(input);
        const __m512i special = _mm512_and_si512(
            _mm512_srli_epi16(characters, 5), low_byte_bits);
        characters = _mm512_xor_si512(
            characters,
            _mm512_or_si512(special, _mm512_slli_epi16(special, 4)));
        const __m512i codes = _mm512_and_si512(characters, low_five_bits);
        const __m512i packed_ten =
            _mm512_maddubs_epi16(codes, pair_weights);
        const __m512i packed_twenty =
            _mm512_madd_epi16(packed_ten, quad_weights);
        const __m512i packed_forty = _mm512_or_si512(
            _mm512_and_si512(packed_twenty, low_twenty_bits),
            _mm512_slli_epi64(_mm512_srli_epi64(packed_twenty, 32), 20));
        _mm512_mask_compressstoreu_epi8(output, kPackedBytes, packed_forty);
        input += 64;
        output += 40;
        block_count -= 8;
    }
    pack_8_blocks_bmi2(input, output, block_count);
}
#endif

using PackBlocksKernel = void (*)(const char*, uint8_t*, size_t);

PackBlocksKernel detect_pack_blocks_kernel() {
#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vbmi2") &&
        __builtin_cpu_supports("bmi2")) {
        return pack_8_blocks_avx512_vbmi2;
    }
    if (__builtin_cpu_supports("bmi2")) return pack_8_blocks_bmi2;
#endif
    return pack_8_blocks_scalar;
}

void pack_8_blocks(const char* input,
                   uint8_t* output,
                   size_t block_count) {
    static const PackBlocksKernel selected = detect_pack_blocks_kernel();
    selected(input, output, block_count);
}

uint8_t code_at(const uint8_t* packed, uint64_t offset) {
    const uint64_t bit_position = offset * 5;
    const size_t byte_position = static_cast<size_t>(bit_position >> 3);
    const unsigned shift = static_cast<unsigned>(bit_position & 7);
    uint16_t value = static_cast<uint16_t>(packed[byte_position] >> shift);
    if (shift > 3) {
        value |= static_cast<uint16_t>(packed[byte_position + 1]) <<
                 (8 - shift);
    }
    return static_cast<uint8_t>(value & 0x1F);
}

char code_to_char(uint8_t code) {
    uint8_t ascii = static_cast<uint8_t>(0x40 | (code & 0x1F));
    const uint8_t stop_x = static_cast<uint8_t>((code & 0x1F) ^ 27U);
    const uint8_t flank_x = static_cast<uint8_t>((code & 0x1F) ^ 28U);
    const uint8_t is_stop = static_cast<uint8_t>(
        (static_cast<uint8_t>(stop_x - 1U) >> 7) & 1U);
    const uint8_t is_flank = static_cast<uint8_t>(
        (static_cast<uint8_t>(flank_x - 1U) >> 7) & 1U);
    return static_cast<char>(ascii ^ ((is_stop | is_flank) * 0x71U));
}

uint64_t fix_stop_chars(uint64_t ascii) {
    constexpr uint64_t kStop = 0x5B5B5B5B5B5B5B5BULL;
    constexpr uint64_t kFlank = 0x5C5C5C5C5C5C5C5CULL;
    constexpr uint64_t kLowBits = 0x0101010101010101ULL;
    constexpr uint64_t kHighBits = 0x8080808080808080ULL;
    const uint64_t stop_x = ascii ^ kStop;
    const uint64_t flank_x = ascii ^ kFlank;
    const uint64_t high_mask =
        (~((stop_x | kHighBits) - kLowBits) |
         ~((flank_x | kHighBits) - kLowBits)) & kHighBits;
    return ascii ^ ((high_mask >> 7) * 0x71ULL);
}

void decode_8_scalar(const uint8_t* packed, uint64_t offset, char* output) {
    const uint8_t* source = packed + static_cast<size_t>((offset >> 3) * 5);
    const uint64_t value = load_5_bytes(source);
    for (unsigned i = 0; i < 8; ++i) {
        output[i] = code_to_char(
            static_cast<uint8_t>((value >> (i * 5)) & 0x1F));
    }
}

#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("bmi2")))
uint64_t decode_8_bmi2_word_raw(const uint8_t* source) {
    constexpr uint64_t kCodeMask = 0x1F1F1F1F1F1F1F1FULL;
    constexpr uint64_t kAsciiMask = 0x4040404040404040ULL;
    return _pdep_u64(load_5_bytes(source), kCodeMask) | kAsciiMask;
}

__attribute__((target("bmi2")))
uint64_t decode_8_bmi2_word(const uint8_t* source) {
    return fix_stop_chars(decode_8_bmi2_word_raw(source));
}

__attribute__((target("avx2")))
__m128i fix_stop_chars_avx2_128(__m128i decoded) {
    const __m128i stop = _mm_set1_epi8('[');
    const __m128i flank = _mm_set1_epi8('\\');
    const __m128i xor_value = _mm_set1_epi8(static_cast<char>(0x71));
    const __m128i match = _mm_or_si128(_mm_cmpeq_epi8(decoded, stop),
                                       _mm_cmpeq_epi8(decoded, flank));
    return _mm_xor_si128(decoded, _mm_and_si128(match, xor_value));
}

__attribute__((target("avx2")))
__m256i fix_stop_chars_avx2(__m256i decoded) {
    const __m256i stop = _mm256_set1_epi8('[');
    const __m256i flank = _mm256_set1_epi8('\\');
    const __m256i xor_value = _mm256_set1_epi8(static_cast<char>(0x71));
    const __m256i match = _mm256_or_si256(
        _mm256_cmpeq_epi8(decoded, stop),
        _mm256_cmpeq_epi8(decoded, flank));
    return _mm256_xor_si256(decoded,
                            _mm256_and_si256(match, xor_value));
}

__attribute__((target("avx2,bmi2")))
void decode_16_avx2_bmi2(const uint8_t* packed,
                         uint64_t offset,
                         char* output) {
    const uint8_t* source = packed + static_cast<size_t>((offset >> 3) * 5);
    const uint64_t word0 = decode_8_bmi2_word_raw(source);
    const uint64_t word1 = decode_8_bmi2_word_raw(source + 5);
    __m128i decoded = _mm_set_epi64x(static_cast<long long>(word1),
                                     static_cast<long long>(word0));
    decoded = fix_stop_chars_avx2_128(decoded);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output), decoded);
}

__attribute__((target("avx2,bmi2")))
void decode_32_avx2_bmi2(const uint8_t* packed,
                         uint64_t offset,
                         char* output) {
    const uint8_t* source = packed + static_cast<size_t>((offset >> 3) * 5);
    const uint64_t word0 = decode_8_bmi2_word_raw(source);
    const uint64_t word1 = decode_8_bmi2_word_raw(source + 5);
    const uint64_t word2 = decode_8_bmi2_word_raw(source + 10);
    const uint64_t word3 = decode_8_bmi2_word_raw(source + 15);
    __m256i decoded = _mm256_set_epi64x(
        static_cast<long long>(word3), static_cast<long long>(word2),
        static_cast<long long>(word1), static_cast<long long>(word0));
    decoded = fix_stop_chars_avx2(decoded);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output), decoded);
}

__attribute__((target("avx512f,avx512bw,avx512vl")))
__m512i fix_stop_chars_avx512(__m512i decoded) {
    const __mmask64 match =
        _mm512_cmpeq_epi8_mask(decoded, _mm512_set1_epi8('[')) |
        _mm512_cmpeq_epi8_mask(decoded, _mm512_set1_epi8('\\'));
    const __m512i xor_value = _mm512_maskz_mov_epi8(
        match, _mm512_set1_epi8(static_cast<char>(0x71)));
    return _mm512_xor_si512(decoded, xor_value);
}

__attribute__((target("avx512f,avx512bw,avx512vl,avx512vbmi,avx512vbmi2")))
void decode_16_avx512_vbmi2(const uint8_t* packed,
                            uint64_t offset,
                            char* output) {
    constexpr __mmask64 kLoadMask = static_cast<__mmask64>(0x1F1FULL);
    const uint8_t* source = packed + static_cast<size_t>((offset >> 3) * 5);
    __m512i expanded = _mm512_maskz_expandloadu_epi8(kLoadMask, source);
    const __m512i shifts = _mm512_set1_epi64(0x231E19140F0A0500ULL);
    __m512i decoded = _mm512_multishift_epi64_epi8(shifts, expanded);
    decoded = _mm512_and_si512(decoded, _mm512_set1_epi8(0x1F));
    decoded = _mm512_or_si512(decoded, _mm512_set1_epi8(0x40));
    decoded = fix_stop_chars_avx512(decoded);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output),
                     _mm512_castsi512_si128(decoded));
}

__attribute__((target("avx512f,avx512bw,avx512vl,avx512vbmi,avx512vbmi2")))
void decode_32_avx512_vbmi2(const uint8_t* packed,
                            uint64_t offset,
                            char* output) {
    constexpr __mmask64 kLoadMask = static_cast<__mmask64>(0x1F1F1F1FULL);
    const uint8_t* source = packed + static_cast<size_t>((offset >> 3) * 5);
    __m512i expanded = _mm512_maskz_expandloadu_epi8(kLoadMask, source);
    const __m512i shifts = _mm512_set1_epi64(0x231E19140F0A0500ULL);
    __m512i decoded = _mm512_multishift_epi64_epi8(shifts, expanded);
    decoded = _mm512_and_si512(decoded, _mm512_set1_epi8(0x1F));
    decoded = _mm512_or_si512(decoded, _mm512_set1_epi8(0x40));
    decoded = fix_stop_chars_avx512(decoded);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output),
                        _mm512_castsi512_si256(decoded));
}


__attribute__((target("avx512f,avx512bw,avx512vl,avx512vbmi,avx512vbmi2")))
void decode_64_avx512_vbmi2(const uint8_t* packed,
                            uint64_t offset,
                            char* output) {
    constexpr __mmask64 kLoadMask = 0x1F1F1F1F1F1F1F1FULL;
    const uint8_t* source = packed + static_cast<size_t>((offset >> 3) * 5);
    const __m512i expanded = _mm512_maskz_expandloadu_epi8(kLoadMask, source);
    const __m512i shifts = _mm512_set1_epi64(0x231E19140F0A0500ULL);
    __m512i decoded = _mm512_multishift_epi64_epi8(shifts, expanded);
    decoded = _mm512_and_si512(decoded, _mm512_set1_epi8(0x1F));
    decoded = _mm512_or_si512(decoded, _mm512_set1_epi8(0x40));
    _mm512_storeu_si512(output, fix_stop_chars_avx512(decoded));
}

#endif

DecodeKernel detect_kernel() {
#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vl") &&
        __builtin_cpu_supports("avx512vbmi") &&
        __builtin_cpu_supports("avx512vbmi2")) {
        return DecodeKernel::Avx512Vbmi2;
    }
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2")) {
        return DecodeKernel::Avx2Bmi2;
    }
#endif
    return DecodeKernel::Scalar32;
}

DecodeKernel kernel() {
    static const DecodeKernel selected = detect_kernel();
    return selected;
}

}  // namespace

uint64_t packed_bytes_for_residues(uint64_t residues) noexcept {
    return (residues * 5 + 7) / 8;
}

void append_packed_5bit(uint8_t* packed,
                        uint64_t& offset,
                        const char* residues,
                        size_t length) {
    size_t position = 0;
    while (position < length && (offset & 7ULL) != 0) {
        set_code(packed, offset++, encode_stop_aware(residues[position++]));
    }
    const size_t block_count = (length - position) / 8;
    if (block_count != 0) {
        pack_8_blocks(
            residues + position,
            packed + static_cast<size_t>((offset >> 3) * 5),
            block_count);
        const size_t packed_residues = block_count * 8;
        offset += packed_residues;
        position += packed_residues;
    }
    while (position < length) {
        set_code(packed, offset++, encode_stop_aware(residues[position++]));
    }
}

void decode_packed_5bit_range(const uint8_t* packed,
                              uint64_t offset,
                              uint64_t length,
                              char* output) {
    uint64_t position = 0;
    while (position < length && ((offset + position) & 7ULL) != 0) {
        output[position] = code_to_char(code_at(packed, offset + position));
        ++position;
    }
    const DecodeKernel selected = kernel();
#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    if (selected == DecodeKernel::Avx512Vbmi2) {
        while (position + 64 <= length) {
            decode_64_avx512_vbmi2(packed, offset + position,
                                   output + position);
            position += 64;
        }
        if (position + 32 <= length) {
            decode_32_avx512_vbmi2(packed, offset + position,
                                   output + position);
            position += 32;
        }
        if (position + 16 <= length) {
            decode_16_avx512_vbmi2(packed, offset + position,
                                   output + position);
            position += 16;
        }
    } else if (selected == DecodeKernel::Avx2Bmi2) {
        while (position + 32 <= length) {
            decode_32_avx2_bmi2(packed, offset + position,
                                output + position);
            position += 32;
        }
        if (position + 16 <= length) {
            decode_16_avx2_bmi2(packed, offset + position,
                                output + position);
            position += 16;
        }
        while (position + 8 <= length) {
            const uint8_t* source = packed +
                static_cast<size_t>(((offset + position) >> 3) * 5);
            const uint64_t decoded = decode_8_bmi2_word(source);
            std::memcpy(output + position, &decoded, sizeof(decoded));
            position += 8;
        }
    }
#endif
    while (position + 8 <= length) {
        decode_8_scalar(packed, offset + position, output + position);
        position += 8;
    }
    while (position < length) {
        output[position] = code_to_char(code_at(packed, offset + position));
        ++position;
    }
}

const char* packed_5bit_decode_kernel_name() noexcept {
    switch (kernel()) {
        case DecodeKernel::Avx512Vbmi2: return "avx512_vbmi2_64";
        case DecodeKernel::Avx2Bmi2: return "avx2_bmi2_32";
        case DecodeKernel::Scalar32: return "scalar_unrolled_32";
    }
    return "unknown";
}

}  // namespace aastore

#undef AASTORE_X86_SIMD
