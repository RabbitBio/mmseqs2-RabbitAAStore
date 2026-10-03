// 文件职责：声明缓冲式 FASTA 行读取和序列字符规范化接口。
// 行读取同时报告序列是否已规范化，让建库流程跳过不必要的字符转换。
#ifndef AASTORE_FASTA_READER_HPP
#define AASTORE_FASTA_READER_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace aastore {

// 顺序读取 FASTA 行，并在扫描时报告序列字符是否已经规范化。
class FastaLineReader {
public:
    // 打开 FASTA 输入文件并建立可复用的顺序读缓冲。
    explicit FastaLineReader(const std::filesystem::path& path);
    // 关闭输入文件并释放行缓冲及跨块暂存空间。
    ~FastaLineReader();

    // 禁止复制包含当前位置和缓冲视图的读取器。
    FastaLineReader(const FastaLineReader&) = delete;
    // 禁止复制赋值，保持文件与缓冲的独占所有权。
    FastaLineReader& operator=(const FastaLineReader&) = delete;

    // 读取下一行并报告序列字符是否已规范化；到达末尾返回 false。
    // 返回的视图只在下一次 next 调用前有效。
    bool next(std::string_view& line, bool& normalized_sequence);

private:
    // 隐藏文件描述符、读取位置和跨缓冲行拼接状态的实现类型。
    class Impl;
    // 独占读取实现；next 返回的视图借用该实现中的缓冲。
    std::unique_ptr<Impl> impl_;
};

// 去除空白、转为大写，并把不支持的字符替换为 X。
void normalize_sequence_line(std::string_view line,
                             std::string& normalized_line);

}  // namespace aastore

#endif
