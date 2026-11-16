#include "builder_writer.hpp"

#include "builder_file.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace aastore {
namespace {

bool is_ascii_space(unsigned char value) {
    return value == ' ' || (value >= '\t' && value <= '\r');
}

class TextWritePipeline {
public:
    TextWritePipeline(BufferedFileWriter* names,
                      BufferedFileWriter* descriptions,
                      bool asynchronous)
        : names_(names), descriptions_(descriptions),
          asynchronous_(asynchronous) {
        if (!asynchronous_ ||
            (names_ == nullptr && descriptions_ == nullptr)) {
            return;
        }
        try {
            writer_ = std::thread(&TextWritePipeline::write_loop, this);
        } catch (...) {
            cancel_and_join();
            throw;
        }
    }

    ~TextWritePipeline() {
        if (!finished_) cancel_and_join();
    }

    TextWritePipeline(const TextWritePipeline&) = delete;
    TextWritePipeline& operator=(const TextWritePipeline&) = delete;

    void submit(std::vector<char>& names,
                std::vector<char>& descriptions) {
        if (names.empty() && descriptions.empty()) return;
        if (!asynchronous_) {
            if (names_ != nullptr && !names.empty()) {
                names_->append(names.data(), names.size());
            }
            if (descriptions_ != nullptr && !descriptions.empty()) {
                descriptions_->append(descriptions.data(),
                                      descriptions.size());
            }
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        while (!cancelled_ && chunks_.size() >= kQueueCapacity) {
            changed_.wait(lock);
        }
        if (cancelled_) {
            const std::exception_ptr error = error_;
            lock.unlock();
            if (error) std::rethrow_exception(error);
            throw std::runtime_error("text write pipeline was cancelled");
        }
        TextChunk chunk;
        chunk.names.swap(names);
        chunk.descriptions.swap(descriptions);
        chunks_.push_back(std::move(chunk));
        lock.unlock();
        changed_.notify_one();
    }

    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
        if (writer_.joinable()) writer_.join();
        finished_ = true;
        if (error_) std::rethrow_exception(error_);
    }

private:
    struct TextChunk {
        std::vector<char> names;
        std::vector<char> descriptions;
    };

    void write_loop() noexcept {
        while (true) {
            TextChunk chunk;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                while (!cancelled_ && !closed_ && chunks_.empty()) {
                    changed_.wait(lock);
                }
                if (cancelled_) return;
                if (chunks_.empty()) {
                    if (closed_) return;
                    continue;
                }
                chunk = std::move(chunks_.front());
                chunks_.pop_front();
            }
            changed_.notify_one();
            try {
                if (names_ != nullptr && !chunk.names.empty()) {
                    names_->append(chunk.names.data(), chunk.names.size());
                }
                if (descriptions_ != nullptr &&
                    !chunk.descriptions.empty()) {
                    descriptions_->append(chunk.descriptions.data(),
                                          chunk.descriptions.size());
                }
            } catch (...) {
                fail(std::current_exception());
                return;
            }
        }
    }

    void fail(std::exception_ptr error) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error_) error_ = error;
            cancelled_ = true;
            closed_ = true;
        }
        changed_.notify_all();
    }

    void cancel_and_join() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            closed_ = true;
        }
        changed_.notify_all();
        if (writer_.joinable()) writer_.join();
        finished_ = true;
    }

    static constexpr size_t kQueueCapacity = 2;
    BufferedFileWriter* names_ = nullptr;
    BufferedFileWriter* descriptions_ = nullptr;
    bool asynchronous_ = false;
    std::deque<TextChunk> chunks_;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_ = false;
    bool cancelled_ = false;
    bool finished_ = false;
    std::exception_ptr error_;
};

}  // namespace

class TextBuildWriter::Impl {
public:
    Impl(const std::filesystem::path& names_path,
         const std::filesystem::path& names_metadata_path,
         const std::filesystem::path& descriptions_path,
         const std::filesystem::path& descriptions_metadata_path,
         bool write_names,
         bool write_descriptions,
         bool asynchronous)
        : write_names_(write_names),
          write_descriptions_(write_descriptions) {
        if (write_names_) {
            names_ = std::make_unique<BufferedFileWriter>(names_path);
            name_metadata_ =
                std::make_unique<CompactTextMetadataWriter>(
                    names_metadata_path);
            names_chunk_.reserve(kBuildStreamBufferBytes);
        }
        if (write_descriptions_) {
            descriptions_ =
                std::make_unique<BufferedFileWriter>(descriptions_path);
            description_metadata_ =
                std::make_unique<CompactTextMetadataWriter>(
                    descriptions_metadata_path);
            descriptions_chunk_.reserve(kBuildStreamBufferBytes);
        }
        pipeline_ = std::make_unique<TextWritePipeline>(
            names_.get(), descriptions_.get(), asynchronous);
    }

    void append_header(std::string_view header) {
        if (finished_) {
            throw std::logic_error("append header on finished text writer");
        }
        if (header.empty() || header.front() != '>') {
            throw std::invalid_argument("invalid FASTA header");
        }

        size_t name_begin = 1;
        while (name_begin < header.size() &&
               is_ascii_space(
                   static_cast<unsigned char>(header[name_begin]))) {
            ++name_begin;
        }
        size_t name_end = name_begin;
        while (name_end < header.size() &&
               !is_ascii_space(
                   static_cast<unsigned char>(header[name_end]))) {
            ++name_end;
        }

        if (write_names_) {
            const size_t name_size = name_end - name_begin;
            append_text(names_chunk_, header.data() + name_begin,
                        name_size);
            name_bytes_ += name_size;
            name_metadata_->append(name_bytes_);
        }
        if (write_descriptions_) {
            size_t description_begin = name_end;
            while (description_begin < header.size() &&
                   is_ascii_space(static_cast<unsigned char>(
                       header[description_begin]))) {
                ++description_begin;
            }
            const size_t description_size =
                header.size() - description_begin;
            append_text(descriptions_chunk_,
                        header.data() + description_begin,
                        description_size);
            description_bytes_ += description_size;
            description_metadata_->append(description_bytes_);
        }
    }

    void finish(uint64_t record_count) {
        if (finished_) return;
        flush_chunks();
        pipeline_->finish();
        if (names_) names_->close();
        if (descriptions_) descriptions_->close();
        if (name_metadata_) {
            name_metadata_->finish(record_count, name_bytes_);
        }
        if (description_metadata_) {
            description_metadata_->finish(
                record_count, description_bytes_);
        }
        finished_ = true;
    }

    void append_text(std::vector<char>& chunk,
                     const char* data,
                     size_t size) {
        while (size != 0) {
            if (chunk.size() == kBuildStreamBufferBytes) flush_chunks();
            const size_t available =
                kBuildStreamBufferBytes - chunk.size();
            const size_t take = std::min(available, size);
            const size_t old_size = chunk.size();
            chunk.resize(old_size + take);
            std::memcpy(chunk.data() + old_size, data, take);
            data += take;
            size -= take;
        }
    }

    void flush_chunks() {
        pipeline_->submit(names_chunk_, descriptions_chunk_);
        names_chunk_.clear();
        descriptions_chunk_.clear();
        if (write_names_ && names_chunk_.capacity() == 0) {
            names_chunk_.reserve(kBuildStreamBufferBytes);
        }
        if (write_descriptions_ &&
            descriptions_chunk_.capacity() == 0) {
            descriptions_chunk_.reserve(kBuildStreamBufferBytes);
        }
    }

    bool write_names_ = false;
    bool write_descriptions_ = false;
    bool finished_ = false;
    uint64_t name_bytes_ = 0;
    uint64_t description_bytes_ = 0;
    std::unique_ptr<BufferedFileWriter> names_;
    std::unique_ptr<BufferedFileWriter> descriptions_;
    std::unique_ptr<CompactTextMetadataWriter> name_metadata_;
    std::unique_ptr<CompactTextMetadataWriter> description_metadata_;
    std::unique_ptr<TextWritePipeline> pipeline_;
    std::vector<char> names_chunk_;
    std::vector<char> descriptions_chunk_;
};

TextBuildWriter::TextBuildWriter(
        const std::filesystem::path& names_path,
        const std::filesystem::path& names_metadata_path,
        const std::filesystem::path& descriptions_path,
        const std::filesystem::path& descriptions_metadata_path,
        bool write_names,
        bool write_descriptions,
        bool asynchronous)
    : impl_(std::make_unique<Impl>(
          names_path, names_metadata_path,
          descriptions_path, descriptions_metadata_path,
          write_names, write_descriptions, asynchronous)) {}

TextBuildWriter::~TextBuildWriter() = default;

void TextBuildWriter::append_header(std::string_view header) {
    impl_->append_header(header);
}

void TextBuildWriter::finish(uint64_t record_count) {
    impl_->finish(record_count);
}

uint64_t TextBuildWriter::name_bytes() const noexcept {
    return impl_->name_bytes_;
}

uint64_t TextBuildWriter::description_bytes() const noexcept {
    return impl_->description_bytes_;
}

}  // namespace aastore
