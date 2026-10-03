// 文件职责：从 FASTA 头拆分名称与描述，写出可选文本列及字节边界索引。
// 两列共享有界后台写入队列，也支持在解析线程中同步写出。
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

// 判断字符是否为 ASCII 空格或制表、换行等空白字符。
bool is_ascii_space(unsigned char value) {
    return value == ' ' || (value >= '\t' && value <= '\r');
}

// 名称和描述共用一个有界队列，避免为两列重复创建调度线程。
class TextWritePipeline {
public:
    // 绑定启用的文本列，并按配置创建共享的后台写入线程。
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

    // 未正常完成时取消排队任务并等待后台写入线程退出。
    ~TextWritePipeline() {
        if (!finished_) cancel_and_join();
    }

    // 禁止复制文本列队列和工作线程。
    TextWritePipeline(const TextWritePipeline&) = delete;
    // 禁止复制赋值，保持流水线资源的独占所有权。
    TextWritePipeline& operator=(const TextWritePipeline&) = delete;

    // 同步写出两列字节，或交换缓冲后投入有界队列，队列满时施加背压。
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

    // 关闭生产端并等待排队文本写完，再传播后台写入异常。
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
    // 一次文本写入任务，两列分别持有自己的连续字节，不包含记录分隔符。
    struct TextChunk {
        // 本任务要追加到名称文件的字节，名称列关闭时为空。
        std::vector<char> names;
        // 本任务要追加到描述文件的字节，描述列关闭时为空。
        std::vector<char> descriptions;
    };

    // 顺序取出文本块并写入启用的列，释放队列位置以唤醒生产者。
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

    // 保存首个写入异常并取消队列，唤醒等待中的提交线程。
    void fail(std::exception_ptr error) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error_) error_ = error;
            cancelled_ = true;
            closed_ = true;
        }
        changed_.notify_all();
    }

    // 取消未完成任务并回收后台线程，不从清理路径抛出异常。
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

    // 最多排队的文本块数，不包括写线程已经取走的当前块。
    static constexpr size_t kQueueCapacity = 2;
    // 借用的名称数据写入器；nullptr 表示不输出名称。
    BufferedFileWriter* names_ = nullptr;
    // 借用的描述数据写入器；nullptr 表示不输出描述。
    BufferedFileWriter* descriptions_ = nullptr;
    // 是否将写入交给后台线程；false 时 submit 在调用线程直接写出。
    bool asynchronous_ = false;
    // 按提交顺序排队的文本块，持有等待写出的字节缓冲。
    std::deque<TextChunk> chunks_;
    // 两列共用的后台顺序写入线程。
    std::thread writer_;
    // 保护任务队列以及关闭、取消和异常状态。
    std::mutex mutex_;
    // 通知队列有任务、有空位或需要结束。
    std::condition_variable changed_;
    // 生产端已关闭，写线程排空队列后退出。
    bool closed_ = false;
    // 已请求取消，允许工作线程不再处理排队任务。
    bool cancelled_ = false;
    // 后台线程是否已回收，避免析构重复取消。
    bool finished_ = false;
    // 首个后台写入异常，提交端或 finish 负责传播。
    std::exception_ptr error_;
};

}  // namespace

// 文本建库状态：拆分 FASTA 头，分别累计名称/描述的字节边界并批量写出。
class TextBuildWriter::Impl {
public:
    // 仅为启用的文本列创建数据和元数据写入器，并建立共享输出流水线。
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

    // 校验 FASTA 头，按空白拆出名称与描述，追加文本并记录各列结束偏移。
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

    // 提交剩余文本并排空队列，关闭数据文件后完成记录边界索引。
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

    // 把文本分段追加到固定容量的字符缓冲，缓冲满时提交两列的待写内容。
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

    // 将名称与描述缓冲交给流水线，并准备下一轮可复用缓冲。
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

    // 是否创建名称数据和名称边界索引。
    bool write_names_ = false;
    // 是否创建描述数据和描述边界索引。
    bool write_descriptions_ = false;
    // 是否已经排空流水线并发布文本索引。
    bool finished_ = false;
    // 已接收名称的总字节数，也是下一条名称的起点，不含索引文件大小。
    uint64_t name_bytes_ = 0;
    // 已接收描述的总字节数，也是下一条描述的起点。
    uint64_t description_bytes_ = 0;
    // 独占名称数据写入器，名称列关闭时为空。
    std::unique_ptr<BufferedFileWriter> names_;
    // 独占描述数据写入器，描述列关闭时为空。
    std::unique_ptr<BufferedFileWriter> descriptions_;
    // 记录每条名称的结束字节位置。
    std::unique_ptr<CompactTextMetadataWriter> name_metadata_;
    // 记录每条描述的结束字节位置。
    std::unique_ptr<CompactTextMetadataWriter> description_metadata_;
    // 两列共用的写入流水线，须早于被借用的写入器销毁。
    std::unique_ptr<TextWritePipeline> pipeline_;
    // 调用线程暂存的名称字节，满块时提交流水线。
    std::vector<char> names_chunk_;
    // 调用线程暂存的描述字节，可与名称块同时提交但长度互相独立。
    std::vector<char> descriptions_chunk_;
};

// 绑定名称、描述及其索引路径，创建可选同步或异步的文本输出实现。
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

// 释放文本输出实现及其文件、缓冲和后台线程。
TextBuildWriter::~TextBuildWriter() = default;

// 将一条 FASTA 头交给内部实现拆分并追加到启用的文本列。
void TextBuildWriter::append_header(std::string_view header) {
    impl_->append_header(header);
}

// 结束文本输出，并校验两列的记录边界与序列总数一致。
void TextBuildWriter::finish(uint64_t record_count) {
    impl_->finish(record_count);
}

// 返回已追加的名称字节总数，未启用名称列时为零。
uint64_t TextBuildWriter::name_bytes() const noexcept {
    return impl_->name_bytes_;
}

// 返回已追加的描述字节总数，未启用描述列时为零。
uint64_t TextBuildWriter::description_bytes() const noexcept {
    return impl_->description_bytes_;
}

}  // namespace aastore
