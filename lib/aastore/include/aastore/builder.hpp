// 文件职责：声明从 FASTA 建库的公开入口、建库选项和统计结果。
// build_fasta 按输入记录顺序生成连续序列 ID，并按配置保存名称与描述。
#ifndef AASTORE_BUILDER_HPP
#define AASTORE_BUILDER_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace aastore {

struct BuildOptions {
    bool overwrite = false;
    bool write_names = true;
    bool write_descriptions = true;
    // 包含 FASTA 解析线程在内的建库总线程预算。
    // 为保持旧调用方的源码兼容性，保留原字段名。
    size_t encode_threads = 1;

    // 校验建库线程预算；无效配置抛出 std::invalid_argument，不修改选项。
    void validate() const;
};

struct BuildStats {
    uint64_t sequences = 0;
    uint64_t residues = 0;
    uint64_t packed_bytes = 0;
    uint64_t name_bytes = 0;
    uint64_t description_bytes = 0;
};

// 按 FASTA 顺序建库，可选写出名称和描述；物理 ID 从零递增。
BuildStats build_fasta(const std::filesystem::path& fasta,
                       const std::filesystem::path& store,
                       const BuildOptions& options = {});

}  // namespace aastore

#endif
