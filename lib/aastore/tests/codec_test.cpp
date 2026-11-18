#include "../src/codec.hpp"
#include "../src/simd_config.hpp"
#include "guarded_buffer.hpp"

#include <algorithm>
#include <iostream>
#include <string>

using aastore_test::GuardedBuffer;

int main() {
    const std::string kernel = aastore::packed_5bit_decode_kernel_name();
    if ((AASTORE_MAX_SIMD_BITS == 0 && kernel != "scalar_unrolled_32") ||
        (AASTORE_MAX_SIMD_BITS == 256 && kernel.find("avx512") == 0)) {
        std::cerr << "kernel exceeds configured SIMD level: " << kernel << '\n';
        return 1;
    }
    GuardedBuffer packed_buffer;
    GuardedBuffer output_buffer;
    size_t cases = 0;

    for (size_t offset = 0; offset < 16; ++offset) {
        for (size_t length = 0; length <= 263; ++length) {
            const size_t bytes = (5 * (offset + length) + 7) / 8;
            uint8_t* packed = packed_buffer.ending_at_guard(bytes);
            std::fill_n(packed, bytes, uint8_t{0});
            for (size_t i = 0; i < offset + length; ++i) {
                const unsigned code = (i * 13 + length) % 32;
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
