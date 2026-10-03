#include <aastore/builder.hpp>
#include <aastore/read_options.hpp>
#include <aastore/store.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" uint64_t aastore_consumer_record_count(const char* path);

namespace {
struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("aastore-installed-consumer-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    Fixture() { std::filesystem::create_directory(path); }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
}  // namespace

int main() {
    try {
        Fixture fixture;
        const auto fasta = fixture.path / "input.fa";
        const auto database = fixture.path / "database";
        {
            std::ofstream output(fasta);
            output << ">first example\nACDEFGHIK\n>second\nMNPQRSTVWY\n";
            if (!output) throw std::runtime_error("could not create FASTA fixture");
        }
        const auto built = aastore::build_fasta(fasta, database);
        if (built.sequences != 2 || built.residues != 19)
            throw std::runtime_error("unexpected build statistics");
        aastore::Store store(database);
        if (store.size() != 2 || store.get(0).view(0) != "ACDEFGHIK" ||
            store.get(1).view(0) != "MNPQRSTVWY")
            throw std::runtime_error("installed SDK sequence roundtrip failed");
        const auto batch = store.get(std::vector<aastore::SequenceId>{1, 0, 1});
        if (batch.size() != 3 || batch[0] != "MNPQRSTVWY" ||
            batch[1] != "ACDEFGHIK" || batch[2] != "MNPQRSTVWY")
            throw std::runtime_error("installed SDK batch roundtrip failed");
        const auto database_string = database.string();
        if (aastore_consumer_record_count(database_string.c_str()) != 2)
            throw std::runtime_error("SDK linked into a shared consumer failed");
        std::cout << "Installed SDK build, read, batch, and shared-consumer checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
