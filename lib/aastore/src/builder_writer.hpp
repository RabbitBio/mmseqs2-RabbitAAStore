// 文件职责：声明建库线程预算分配、序列写入器和可选文本列写入器。
// FASTA 解析器通过这些接口提交字符与记录边界，无需直接管理编码和写盘线程。
#ifndef AASTORE_BUILDER_WRITER_HPP
#define AASTORE_BUILDER_WRITER_HPP

#include "aastore/builder.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace aastore {

// 把建库总线程预算分配给 packed 编码、packed 写入和文本写入。
struct BuildThreadPlan {
    size_t packed_encoder_threads = 0;
    bool packed_writer_async = false;
    bool packed_fused_worker = false;
    bool text_writer_async = false;
};

// 在总线程预算内选择同步、融合工作线程或独立编码与写入线程布局。
BuildThreadPlan plan_build_threads(size_t total_threads,
                                   bool write_text_columns);

// 接收连续残基流和序列边界，负责 packed.bin 与 meta.bin 的写出。
class SequenceBuildWriter {
public:
    // 打开 packed 与序列元数据输出，并按线程方案建立编码写入流水线。
    SequenceBuildWriter(const std::filesystem::path& packed_path,
                        const std::filesystem::path& metadata_path,
                        const BuildThreadPlan& threads);
    // 释放写入资源并取消未完成的后台任务；完整建库须显式调用 finish。
    ~SequenceBuildWriter();

    // 禁止复制含输出文件和工作线程的写入器。
    SequenceBuildWriter(const SequenceBuildWriter&) = delete;
    // 禁止复制赋值，保持写入流水线的独占所有权。
    SequenceBuildWriter& operator=(const SequenceBuildWriter&) = delete;

    // 追加已规范化的序列字符，允许同一序列由多次调用组成。
    void append_residues(std::string_view residues);
    // 记录当前序列的结束边界，不强制刷新编码块。
    void finish_sequence();
    // 提交末尾字符块，等待流水线排空并完成 packed 与元数据输出。
    void finish();
    // 返回当前累计的序列数量、字符数量和打包字节统计。
    const BuildStats& stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 解析 FASTA 头并负责可选名称、描述数据及其元数据写出。
class TextBuildWriter {
public:
    // 按列开关创建名称、描述及其索引输出，可选使用一个后台写入线程。
    TextBuildWriter(const std::filesystem::path& names_path,
                    const std::filesystem::path& names_metadata_path,
                    const std::filesystem::path& descriptions_path,
                    const std::filesystem::path& descriptions_metadata_path,
                    bool write_names,
                    bool write_descriptions,
                    bool asynchronous);
    // 释放文本输出和后台线程；完整列文件须先显式调用 finish。
    ~TextBuildWriter();

    // 禁止复制文本文件、缓冲与后台工作线程。
    TextBuildWriter(const TextBuildWriter&) = delete;
    // 禁止复制赋值，保持文本输出状态的独占所有权。
    TextBuildWriter& operator=(const TextBuildWriter&) = delete;

    // 拆分一条 FASTA 头中的名称和描述，追加启用的文本列及其边界。
    void append_header(std::string_view header);
    // 刷出文本、等待后台写入完成，并按记录总数结束文本元数据。
    void finish(uint64_t record_count);
    // 返回已接收的名称数据总字节数，不含文本元数据。
    uint64_t name_bytes() const noexcept;
    // 返回已接收的描述数据总字节数，不含文本元数据。
    uint64_t description_bytes() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aastore

#endif
