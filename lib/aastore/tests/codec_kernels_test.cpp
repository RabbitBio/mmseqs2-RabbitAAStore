// 文件职责：直接验证各解码内核，避免本机默认 AVX-512 掩盖 BMI2 尾块错误。
// 此测试独立编译 codec.cpp，不链接 aastore，不改变生产接口或运行时分派。
#include "../src/codec.cpp"
#include "guarded_buffer.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using aastore_test::GuardedBuffer;

using Decode = void (*)(const uint8_t*, uint64_t, char*);
struct Kernel {
    const char* name;
    size_t width;
    Decode decode;
};

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
void decode_bmi2_8(const uint8_t* packed, uint64_t offset, char* output) {
    const uint64_t word = aastore::decode_8_bmi2_word(packed + offset / 8 * 5);
    std::memcpy(output, &word, sizeof(word));
}
#endif

char expected_character(unsigned code) {
    return code == 27 ? '*' : code == 28 ? '-' : static_cast<char>('@' + code);
}

unsigned character_code(char value) {
    return value == '*' ? 27 : value == '-' ? 28 : static_cast<unsigned>(value - '@');
}

// 输入用独立的逐位编码构造，解码期望值也不调用生产转换函数。
void check(const Kernel& kernel, const std::vector<unsigned>& codes, size_t offset,
           GuardedBuffer& input, GuardedBuffer& result) {
    const size_t bytes = (5 * (offset + codes.size()) + 7) / 8;
    uint8_t* packed = input.ending_at_guard(bytes);
    std::fill_n(packed, bytes, uint8_t{0});
    for (size_t index = 0; index < offset + codes.size(); ++index) {
        const unsigned code = index < offset ? 9 : codes[index - offset];
        for (unsigned bit = 0; bit < 5; ++bit) {
            const size_t position = index * 5 + bit;
            packed[position / 8] |= static_cast<uint8_t>(
                ((code >> bit) & 1U) << (position % 8));
        }
    }
    char* output = reinterpret_cast<char*>(result.ending_at_guard(codes.size()));
    std::fill_n(output, codes.size(), static_cast<char>(0xA5));
    output[-1] = static_cast<char>(0xA5);
    kernel.decode(packed, offset, output);
    if (output[-1] != static_cast<char>(0xA5)) {
        throw std::runtime_error(std::string(kernel.name) + " overwrote output prefix");
    }
    for (size_t index = 0; index < codes.size(); ++index) {
        if (output[index] != expected_character(codes[index])) {
            throw std::runtime_error(std::string(kernel.name) + " mismatch offset=" +
                std::to_string(offset) + " index=" + std::to_string(index) +
                " expected=" + expected_character(codes[index]) + " actual=" + output[index]);
        }
    }
}

}  // namespace

int main() {
    try {
        std::vector<Kernel> kernels{{"scalar8", 8, aastore::decode_8_scalar}};
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
        __builtin_cpu_init();
        if (__builtin_cpu_supports("bmi2")) {
            kernels.push_back({"bmi2_8", 8, decode_bmi2_8});
        }
        if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2")) {
            kernels.push_back({"avx2_bmi2_16", 16, aastore::decode_16_avx2_bmi2});
            kernels.push_back({"avx2_bmi2_32", 32, aastore::decode_32_avx2_bmi2});
        }
        if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
            __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vbmi") &&
            __builtin_cpu_supports("avx512vbmi2")) {
            kernels.push_back({"avx512_vbmi2_16", 16, aastore::decode_16_avx512_vbmi2});
            kernels.push_back({"avx512_vbmi2_32", 32, aastore::decode_32_avx512_vbmi2});
            kernels.push_back({"avx512_vbmi2_64", 64, aastore::decode_64_avx512_vbmi2});
        }
#endif
        GuardedBuffer input;
        GuardedBuffer result;
        const std::vector<std::string> regressions{
            "*ZAAAAAA", "A*ZAAAAA", "*ZZZZZZZ", "-*AAAAAA", "-*ZAAAAA", "Z*AAAAAA"};
        for (const Kernel& kernel : kernels) {
            size_t cases = 0;
            for (size_t offset : {size_t{0}, size_t{8}, size_t{16}, size_t{24}}) {
                for (const std::string& regression : regressions) {
                    std::vector<unsigned> codes(kernel.width);
                    for (size_t index = 0; index < codes.size(); ++index) {
                        codes[index] = character_code(regression[index % regression.size()]);
                    }
                    check(kernel, codes, offset, input, result);
                    ++cases;
                }
                // 枚举全部32×32相邻码；同时覆盖字节、8字节lane与向量边界。
                std::vector<size_t> positions;
                for (size_t index = 0; index + 1 < kernel.width; ++index) {
                    if (kernel.width == 8 || index == 0 || index % 8 == 7 ||
                        index + 2 == kernel.width) positions.push_back(index);
                }
                for (size_t position : positions) {
                    for (unsigned first = 0; first < 32; ++first) {
                        for (unsigned second = 0; second < 32; ++second) {
                            std::vector<unsigned> codes(kernel.width, 1);
                            codes[position] = first;
                            codes[position + 1] = second;
                            check(kernel, codes, offset, input, result);
                            ++cases;
                        }
                    }
                }
            }
            std::cout << kernel.name << ": " << cases << " guarded kernel cases passed\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
