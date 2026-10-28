// 文件职责：用独立标量构造的输入验证 5 位字符解码。
// 测试不同起始位偏移和长度，并以紧邻保护页的缓冲检查越界读写。
#include "../src/codec.hpp"
#include "guarded_buffer.hpp"

#include <algorithm>
#include <iostream>

using aastore_test::GuardedBuffer;

// 验证全部编码和未对齐范围，并用保护页检查越界访问。
int main() {
    GuardedBuffer packed_buffer;
    GuardedBuffer output_buffer;
    size_t cases = 0;

    // 精确长度缓冲紧邻不可访问页，包括零长度情形。
    for (size_t offset = 0; offset < 16; ++offset) {
        for (size_t length = 0; length <= 263; ++length) {
            const size_t bytes = (5 * (offset + length) + 7) / 8;
            uint8_t* packed = packed_buffer.ending_at_guard(bytes);
            std::fill_n(packed, bytes, uint8_t{0});
            for (size_t i = 0; i < offset + length; ++i) {
                const unsigned code = (i * 13 + length) % 32;
                // 使用独立标量编码生成期望输入，并覆盖全部 32 种编码。
                for (unsigned bit = 0; bit < 5; ++bit) {
                    const size_t position = i * 5 + bit;
                    packed[position / 8] |= static_cast<uint8_t>(
                        ((code >> bit) & 1) << (position % 8));
                }
            }
            uint8_t* output = output_buffer.ending_at_guard(length);
            std::fill_n(output, length, uint8_t{0xA5});
            output[-1] = 0xA5;
            aastore::decode_packed_5bit_range(
                packed, offset, length, reinterpret_cast<char*>(output));
            if (output[-1] != 0xA5) return 1;
            for (size_t i = 0; i < length; ++i) {
                const unsigned code = ((offset + i) * 13 + length) % 32;
                const uint8_t expected = static_cast<uint8_t>(
                    code == 27 ? '*' : code == 28 ? '-' : '@' + code);
                if (output[i] != expected) {
                    std::cerr << "decode mismatch offset=" << offset
                              << " length=" << length << " index=" << i << '\n';
                    return 1;
                }
            }
            ++cases;
        }
    }
    std::cout << aastore::packed_5bit_decode_kernel_name()
              << ": " << cases << " guarded decode cases passed\n";
}
