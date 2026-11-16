#include <aastore/store.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
size_t parse_threads(const std::string& text) {
    size_t consumed = 0;
    if (text.empty() || !std::all_of(text.begin(), text.end(),
            [](char value) { return value >= '0' && value <= '9'; })) {
        throw std::invalid_argument("threads must be a positive integer");
    }
    const auto value = std::stoull(text, &consumed);
    if (value == 0 || consumed != text.size() ||
        value > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument("threads must be a positive integer");
    }
    return static_cast<size_t>(value);
}

void print_sequence(aastore::SequenceId id, std::string_view sequence) {
    std::cout << "  ID=" << id << " length=" << sequence.size()
              << " sequence=" << sequence.substr(0, 60);
    if (sequence.size() > 60) std::cout << "...";
    std::cout << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 4 || (argc > 1 && std::string(argv[1]) == "--help")) {
        std::cout << "Usage: aastore_example [input.fa] [output.aastore] [threads]\n"
                     "Defaults (run inside examples/): data/huge_100k.fa "
                     "output/huge_100k.aastore 4\n";
        return argc > 4 ? 1 : 0;
    }
    try {
        const std::filesystem::path input = argc > 1 ? argv[1] : "data/huge_100k.fa";
        const std::filesystem::path database = argc > 2 ? argv[2] : "output/huge_100k.aastore";
        const size_t threads = argc > 3 ? parse_threads(argv[3]) : 4;
        if (!std::filesystem::is_regular_file(input)) {
            throw std::runtime_error("FASTA not found: " + input.string());
        }

        if (!std::filesystem::exists(database)) {
            if (!database.parent_path().empty()) {
                std::filesystem::create_directories(database.parent_path());
            }
            aastore::BuildOptions options;
            options.threads = threads;
            const auto start = std::chrono::steady_clock::now();
            const auto built = aastore::build_fasta(input, database, options);
            const auto seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "Database built: " << built.sequences << " sequences, "
                      << built.residues << " residues, elapsed time: " << seconds << " s\n";
        } else {
            std::cout << "Reusing existing database: " << database.string() << '\n';
        }

        aastore::Store store(database);
        if (store.size() == 0) throw std::runtime_error("the database is empty");
        std::cout << "Database: " << store.size() << " sequences, "
                  << store.total_residues() << " residues, decoding kernel: "
                  << store.decode_kernel_name() << '\n';

        aastore::ReadOptions reads;
        reads.threads = threads;
        reads.io_backend = aastore::IoBackend::Auto;

        std::cout << "Single-sequence retrieval:\n";
        const auto single = store.get(0, reads);
        print_sequence(0, single.view(0));

        const std::vector<aastore::SequenceId> ids{store.size() - 1, 0, store.size() - 1};
        const auto batch = store.get(ids, reads);
        std::cout << "Batch retrieval with unordered IDs (duplicates preserved):\n";
        for (size_t i = 0; i < batch.size(); ++i) print_sequence(ids[i], batch.view(i));
        if (batch.size() != ids.size() || batch.view(0) != batch.view(2) ||
            batch.view(1) != single.view(0)) {
            throw std::runtime_error("unexpected batch order or data");
        }

        const auto sequential = store.sequential_get(0, std::min<uint64_t>(3, store.size()));
        std::cout << "Sequential retrieval of the first " << sequential.size() << " sequences:\n";
        for (size_t i = 0; i < sequential.size(); ++i) print_sequence(i, sequential.view(i));
        if (sequential.view(0) != single.view(0)) {
            throw std::runtime_error("unexpected sequential data");
        }

        const auto records = store.batch_get_records({0, store.size() - 1}, aastore::RecordFields::All, reads);
        std::cout << "Names and descriptions of the first and last records:\n";
        for (size_t i = 0; i < records.size(); ++i) {
            std::cout << "  name=" << records.name_view(i)
                      << " description=" << records.description_view(i) << '\n';
        }
        std::cout << "Example complete. Database retained at " << database.string() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "RabbitAAStore example: " << error.what() << '\n';
        return 1;
    }
}
