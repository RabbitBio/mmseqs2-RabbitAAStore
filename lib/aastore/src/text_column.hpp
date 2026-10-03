// 文件职责：声明名称或描述文本列的打开、批量读取和常驻大小查询接口。
// 文本列拥有独立的数据与边界索引文件，结果按原始请求顺序返回。
#ifndef AASTORE_TEXT_COLUMN_HPP
#define AASTORE_TEXT_COLUMN_HPP

#include "aastore/store.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aastore {

// 一列名称或描述的读取器，持有该列数据及 AATEXT1 边界索引的访问资源。
class TextColumn {
public:
    // 打开并校验文本列，常驻模式将完整正文读入自有内存。
    TextColumn(const std::filesystem::path& data_path,
               const std::filesystem::path& metadata_path,
               uint64_t expected_records,
               bool resident = false);
    // 关闭文件、释放常驻正文并解除元数据映射。
    ~TextColumn();

    // 禁止复制拥有文件和映射的文本列。
    TextColumn(const TextColumn&) = delete;
    // 禁止复制赋值，保持资源的独占所有权。
    TextColumn& operator=(const TextColumn&) = delete;

    // 批量读取文本，结果按请求顺序访问，文本不追加 NUL。
    TextBatch batch_get(const std::vector<SequenceId>& ids,
                        const ReadOptions& options) const;
    // 返回常驻模式下的数据大小，不含元数据且不代表实时 RSS。
    uint64_t resident_bytes() const noexcept;

private:
    // 隐藏文件、索引映射以及可选常驻缓冲的实现类型。
    class Impl;
    // 独占列读取实现，析构时统一关闭文件并解除映射。
    std::unique_ptr<Impl> impl_;
};

}  // namespace aastore

#endif
