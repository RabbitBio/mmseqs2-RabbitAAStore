// 文件职责：提供 FASTA 转 AAStore 数据库的命令行入口。
// 解析路径、线程和文本列开关，调用通用建库接口并报告耗时与输出规模。
#include "aastore/store.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

struct Arguments {
    std::filesystem::path fasta;
    std::filesystem::path store;
    bool overwrite = false;
    bool write_names = true;
    bool write_descriptions = true;
    size_t threads = 1;
};

// 输出建库工具的命令行用法和可选参数。
void print_usage(std::ostream& output) {
    output << "usage: aastore_build FASTA STORE [OPTIONS]\n"
              "  --overwrite         replace an existing store\n"
              "  --no-names          omit the name column\n"
              "  --no-descriptions   omit the description column\n"
              "  --sequences-only    omit both annotation columns\n"
              "  --threads N         total build threads, including parser (default: 1)\n";
}

// 解析非零整数并校验可用 size_t 表示，否则报告对应参数错误。
size_t parse_positive_size(const std::string& value,
                           const std::string& option) {
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0 ||
        parsed > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument("invalid value for " + option +
                                    ": " + value);
    }
    return static_cast<size_t>(parsed);
}

// 解析 FASTA、输出目录和建库选项，拒绝缺失或未知参数。
Arguments parse_arguments(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage(std::cout);
        std::exit(0);
    }
    if (argc < 3) {
        throw std::invalid_argument("expected FASTA and STORE paths");
    }

    Arguments arguments;
    arguments.fasta = argv[1];
    arguments.store = argv[2];
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--overwrite") {
            arguments.overwrite = true;
        } else if (option == "--no-names") {
            arguments.write_names = false;
        } else if (option == "--no-descriptions") {
            arguments.write_descriptions = false;
        } else if (option == "--sequences-only") {
            arguments.write_names = false;
            arguments.write_descriptions = false;
        } else if (option == "--threads") {
            if (++index >= argc) {
                throw std::invalid_argument("missing value for --threads");
            }
            arguments.threads =
                parse_positive_size(argv[index], option);
        } else {
            throw std::invalid_argument("unknown option: " +
                                        option);
        }
    }
    return arguments;
}

}  // namespace

// 执行 FASTA 建库并输出规模和耗时统计，失败时返回非零退出码。
int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        aastore::BuildOptions options;
        options.overwrite = arguments.overwrite;
        options.write_names = arguments.write_names;
        options.write_descriptions = arguments.write_descriptions;
        options.encode_threads = arguments.threads;

        const auto started = std::chrono::steady_clock::now();
        const aastore::BuildStats stats = aastore::build_fasta(
            arguments.fasta, arguments.store, options);
        const double elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();

        std::cout << "input_fasta=" << arguments.fasta << '\n'
                  << "output_store=" << arguments.store << '\n'
                  << "names_written=" << std::boolalpha
                  << arguments.write_names << '\n'
                  << "descriptions_written="
                  << arguments.write_descriptions << '\n'
                  << "build_threads=" << arguments.threads << '\n'
                  << "sequences=" << stats.sequences << '\n'
                  << "residues=" << stats.residues << '\n'
                  << "packed_bytes=" << stats.packed_bytes << '\n'
                  << "name_bytes=" << stats.name_bytes << '\n'
                  << "description_bytes=" << stats.description_bytes << '\n'
                  << "build_seconds=" << elapsed_seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        print_usage(std::cerr);
        return 2;
    }
}
