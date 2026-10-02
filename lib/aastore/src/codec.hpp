// 文件职责：声明连续 5 位编码的大小计算、追加编码与区间解码接口。
// 解码输出原始字符，编解码内核由运行时 CPU 能力决定。
#ifndef AASTORE_CODEC_HPP
#define AASTORE_CODEC_HPP

#include <cstddef>
#include <cstdint>

namespace aastore {

// 计算指定残基数量的 5 位编码字节数，向上取整。
uint64_t packed_bytes_for_residues(uint64_t residues) noexcept;

// 把规范化的大写残基及 */- 追加到缓冲，并推进以残基计的 offset。
// 目标待写位必须预先清零；调用方负责提供足够的输出空间。
void append_packed_5bit(uint8_t* packed,
                        uint64_t& offset,
                        const char* residues,
                        size_t length);

// 从以残基计的 offset 解码 length 个字符，不追加 NUL。
// 调用方须保证输入覆盖解码范围，输出至少可写 length 字节。
void decode_packed_5bit_range(const uint8_t* packed,
                              uint64_t offset,
                              uint64_t length,
                              char* output);

// 返回运行时选择的解码内核名称。
const char* packed_5bit_decode_kernel_name() noexcept;

}  // namespace aastore

#endif
