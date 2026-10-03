// 文件职责：声明 AAPCMP3 序列边界元数据的只读映射与查询接口。
// 通过块基址和 32 位块内偏移还原全局残基位置，避免为每条序列保存完整 64 位位置。
#ifndef AASTORE_METADATA_HPP
#define AASTORE_METADATA_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace aastore {

// 序列边界索引读取器：持有 meta.bin 的只读映射，按 ID 查询字符起点。
// 相邻边界之差就是序列长度；不持有或解码 packed.bin 的序列数据。
class Metadata {
public:
    // 校验 AAPCMP3 元数据并建立只读映射。
    explicit Metadata(const std::filesystem::path& store);
    // 解除元数据映射。
    ~Metadata();

    // 禁止复制，避免重复释放同一个映射。
    Metadata(const Metadata&) = delete;
    // 禁止复制赋值，保持映射的独占所有权。
    Metadata& operator=(const Metadata&) = delete;

    // 返回序列总数，不包括末尾哨兵边界。
    uint64_t sequence_count() const noexcept { return sequence_count_; }
    // 返回全部序列的残基总数。
    uint64_t total_residues() const noexcept { return total_residues_; }
    // 返回连续 5 位编码数据的有效字节数。
    uint64_t packed_bytes() const noexcept;
    // 返回全局残基边界，sequence_index 可等于序列总数以查询末尾。
    uint64_t start_position(uint64_t sequence_index) const;

private:
    // 读取并映射当前 AAPCMP3 元数据。
    void open(const std::filesystem::path& store);
    // 解除映射并重置指针和映射相关计数。
    void clear() noexcept;

    // 序列条数；边界数组另有一个末尾哨兵，因此共 sequence_count_ + 1 项。
    uint64_t sequence_count_ = 0;
    // 所有序列的字符总数，也是最后一个边界的位置；不是 packed 字节数。
    uint64_t total_residues_ = 0;

    // meta.bin 只读映射的起点；由本对象负责 munmap，下面两个数组借用它。
    void* mapping_ = nullptr;
    // 整个映射的字节数，包含文件头、偏移数组和块基址数组。
    size_t mapping_bytes_ = 0;
    // 按全局边界编号索引的 32 位块内字符偏移，共 sequence_count_ + 1 项。
    const uint32_t* offsets_ = nullptr;
    // 64 位块基址数组的字节起点；可能未按 8 字节对齐，查询时用 memcpy 读取。
    const char* bases_ = nullptr;
    // 每块边界数的 log2；index >> block_shift_ 得到所属块编号。
    uint32_t block_shift_ = 0;
};

}  // namespace aastore

#endif
