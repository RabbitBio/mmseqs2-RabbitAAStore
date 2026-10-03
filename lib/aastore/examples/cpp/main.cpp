#include <aastore/store.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path = root / ("aastore-example-" + std::to_string(stamp) + "-" +
                           std::to_string(attempt));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create temporary example directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        const auto input = temporary.path / "proteins.fa";
        const auto database = temporary.path / "proteins.aastore";
        {
            std::ofstream fasta(input);
            fasta << ">first example protein\nACDEFG\n>second\nMKT\n";
            fasta.close();
            if (!fasta) throw std::runtime_error("cannot write example FASTA");
        }
        aastore::build_fasta(input, database);
        aastore::Store store(database);
        auto batch = store.get({1, 0, 1});
        if (batch.size() != 3 || batch[0] != "MKT" ||
            batch[1] != "ACDEFG" || batch[2] != "MKT") {
            throw std::runtime_error("unexpected sequence data");
        }
        for (size_t index = 0; index < batch.size(); ++index) {
            std::cout << batch.view(index) << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "AAStore example: " << error.what() << '\n';
        return 1;
    }
}
