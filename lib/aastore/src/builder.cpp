// 文件职责：组织 FASTA 解析、序列编码、文本写出及建库结果发布。
// 使用临时文件完成构建与同步，再替换正式文件；失败时清理本次临时输出。
#include "aastore/builder.hpp"

#include "builder_writer.hpp"
#include "fasta_reader.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace aastore {
namespace {

// 按覆盖策略检查目标文件，再将临时文件重命名到目标路径。
void replace_file(const std::filesystem::path& from,
                  const std::filesystem::path& to,
                  bool overwrite) {
    if (!overwrite && std::filesystem::exists(to)) {
        throw std::runtime_error("store file already exists: " +
                                 to.string());
    }
    std::filesystem::rename(from, to);
}

// 打开指定路径并执行 fsync，重试中断且报告同步或关闭失败。
void sync_path(const std::filesystem::path& path, int open_flags) {
    const int descriptor = ::open(path.c_str(), open_flags);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open for fsync " + path.string() +
                                 ": " + std::strerror(errno));
    }

    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result < 0 && errno == EINTR);
    const int sync_error = result < 0 ? errno : 0;
    const int close_result = ::close(descriptor);
    if (sync_error != 0) {
        throw std::runtime_error("fsync failed for " + path.string() +
                                 ": " + std::strerror(sync_error));
    }
    if (close_result < 0) {
        throw std::runtime_error("close failed after fsync for " +
                                 path.string() + ": " +
                                 std::strerror(errno));
    }
}

// 同步普通文件的数据与元数据，并报告 fsync 或关闭错误。
void sync_file(const std::filesystem::path& path) {
    sync_path(path, O_RDONLY | O_CLOEXEC);
}

// 同步目录项，使已完成的文件重命名得到持久化处理。
void sync_directory(const std::filesystem::path& path) {
    sync_path(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
}

// 删除覆盖建库后不再启用的旧列文件；路径不存在时无需处理。
void remove_file_if_present(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "cannot remove disabled store column", path, error);
    }
}

// 尽力删除本次建库的临时文件，不让清理错误覆盖原始异常。
void cleanup_build_files(
        const std::array<std::filesystem::path, 6>& paths) noexcept {
    std::error_code error;
    for (const std::filesystem::path& path : paths) {
        std::filesystem::remove(path, error);
        error.clear();
    }
}

// 取得路径的父目录；未显式指定父目录时使用当前工作目录。
std::filesystem::path parent_directory(
        const std::filesystem::path& path) {
    return path.parent_path().empty()
        ? std::filesystem::current_path()
        : path.parent_path();
}

}  // namespace

// 顺序解析 FASTA，驱动序列与文本写入器，完成文件同步和发布并返回建库统计。
BuildStats build_fasta(const std::filesystem::path& fasta,
                       const std::filesystem::path& store,
                       const BuildOptions& options) {
    const bool write_text_columns =
        options.write_names || options.write_descriptions;
    const BuildThreadPlan threads =
        plan_build_threads(options.encode_threads, write_text_columns);
    FastaLineReader input(fasta);

    std::filesystem::create_directories(store);
    const std::filesystem::path packed_path = store / "packed.bin";
    const std::filesystem::path meta_path = store / "meta.bin";
    const std::filesystem::path names_path = store / "names.bin";
    const std::filesystem::path names_meta_path = store / "names.meta";
    const std::filesystem::path descriptions_path =
        store / "descriptions.bin";
    const std::filesystem::path descriptions_meta_path =
        store / "descriptions.meta";
    if (!options.overwrite &&
        (std::filesystem::exists(packed_path) ||
         std::filesystem::exists(meta_path) ||
         std::filesystem::exists(names_path) ||
         std::filesystem::exists(names_meta_path) ||
         std::filesystem::exists(descriptions_path) ||
         std::filesystem::exists(descriptions_meta_path))) {
        throw std::runtime_error("store already exists: " + store.string());
    }

    const std::string suffix = ".tmp." + std::to_string(::getpid());
    const std::filesystem::path packed_tmp = packed_path.string() + suffix;
    const std::filesystem::path meta_tmp = meta_path.string() + suffix;
    const std::filesystem::path names_tmp = names_path.string() + suffix;
    const std::filesystem::path names_meta_tmp =
        names_meta_path.string() + suffix;
    const std::filesystem::path descriptions_tmp =
        descriptions_path.string() + suffix;
    const std::filesystem::path descriptions_meta_tmp =
        descriptions_meta_path.string() + suffix;
    const std::array<std::filesystem::path, 6> temporary = {
        packed_tmp, meta_tmp, names_tmp, names_meta_tmp,
        descriptions_tmp, descriptions_meta_tmp};
    cleanup_build_files(temporary);

    try {
        SequenceBuildWriter sequences(packed_tmp, meta_tmp, threads);
        TextBuildWriter text(
            names_tmp, names_meta_tmp,
            descriptions_tmp, descriptions_meta_tmp,
            options.write_names, options.write_descriptions,
            threads.text_writer_async);

        std::string normalized_line;
        std::string_view line;
        bool line_is_normalized = true;
        bool sequence_open = false;
        uint64_t line_number = 0;
        while (input.next(line, line_is_normalized)) {
            ++line_number;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty() && line.front() == '>') {
                if (sequence_open) sequences.finish_sequence();
                text.append_header(line);
                sequence_open = true;
            } else if (!line.empty()) {
                if (!sequence_open) {
                    throw std::runtime_error(
                        "FASTA sequence before first header at line " +
                        std::to_string(line_number));
                }
                if (line_is_normalized) {
                    sequences.append_residues(line);
                } else {
                    normalize_sequence_line(line, normalized_line);
                    sequences.append_residues(normalized_line);
                }
            }
        }
        if (sequence_open) sequences.finish_sequence();
        sequences.finish();
        text.finish(sequences.stats().sequences);

        BuildStats stats = sequences.stats();
        stats.name_bytes = text.name_bytes();
        stats.description_bytes = text.description_bytes();

        sync_file(packed_tmp);
        sync_file(meta_tmp);
        if (options.write_names) {
            sync_file(names_tmp);
            sync_file(names_meta_tmp);
        }
        if (options.write_descriptions) {
            sync_file(descriptions_tmp);
            sync_file(descriptions_meta_tmp);
        }

        replace_file(packed_tmp, packed_path, options.overwrite);
        replace_file(meta_tmp, meta_path, options.overwrite);
        if (options.write_names) {
            replace_file(names_tmp, names_path, options.overwrite);
            replace_file(names_meta_tmp, names_meta_path,
                         options.overwrite);
        }
        if (options.write_descriptions) {
            replace_file(descriptions_tmp, descriptions_path,
                         options.overwrite);
            replace_file(descriptions_meta_tmp, descriptions_meta_path,
                         options.overwrite);
        }
        if (options.overwrite) {
            if (!options.write_names) {
                remove_file_if_present(names_path);
                remove_file_if_present(names_meta_path);
            }
            if (!options.write_descriptions) {
                remove_file_if_present(descriptions_path);
                remove_file_if_present(descriptions_meta_path);
            }
        }
        sync_directory(store);
        sync_directory(parent_directory(store));
        return stats;
    } catch (...) {
        cleanup_build_files(temporary);
        throw;
    }
}

}  // namespace aastore
