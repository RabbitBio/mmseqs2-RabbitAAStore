// 文件职责：把规范化字符流编码为连续 5 位数据，并记录每条序列的边界。
// 有界槽位协调解析、编码和按序写出；线程预算不足时使用同步或融合执行路径。
#include "builder_writer.hpp"

#include "builder_file.hpp"
#include "codec.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace aastore {
namespace {

constexpr size_t kPackingTargetBytesPerJob = 512 * 1024;
constexpr size_t kPackingResiduesPerJob =
    (kPackingTargetBytesPerJob / 5) * 8;
constexpr size_t kPackingBytesPerJob =
    (kPackingResiduesPerJob * 5 + 7) / 8;

// 固定槽位流水线让解析、编码和顺序写出在受控内存内重叠。
class PackedWritePipeline {
public:
    // 分配可复用编码槽位，按配置启动编码线程、顺序写线程或融合工作线程。
    PackedWritePipeline(BufferedFileWriter& output,
                        size_t encoder_threads,
                        bool writer_async,
                        bool fused_worker)
        : output_(output),
          slots_(encoder_threads == 0 && !writer_async && !fused_worker
                     ? 1
                     : std::max<size_t>(4, encoder_threads + 2)),
          work_queue_(slots_.size()),
          active_encoders_(encoder_threads),
          encoder_threads_(encoder_threads),
          fused_worker_(fused_worker),
          synchronous_(encoder_threads == 0 && !writer_async &&
                       !fused_worker) {
        if (fused_worker_ && (encoder_threads != 0 || writer_async)) {
            throw std::invalid_argument(
                "invalid fused packed pipeline configuration");
        }
        for (Slot& slot : slots_) {
            slot.residues.resize(kPackingResiduesPerJob);
            slot.packed.resize(kPackingBytesPerJob);
        }
        if (synchronous_) return;
        if (!writer_async && !fused_worker_) {
            throw std::invalid_argument(
                "packed pipeline requires an asynchronous writer");
        }
        try {
            encoders_.reserve(encoder_threads);
            for (size_t worker = 0; worker < encoder_threads; ++worker) {
                encoders_.emplace_back(&PackedWritePipeline::encode_loop,
                                       this);
            }
            writer_ = fused_worker_
                ? std::thread(&PackedWritePipeline::encode_write_loop, this)
                : std::thread(&PackedWritePipeline::write_loop, this);
        } catch (...) {
            cancel_and_join();
            throw;
        }
    }

    // 流水线未正常结束时取消任务并等待后台线程退出。
    ~PackedWritePipeline() {
        if (!finished_) cancel_and_join();
    }

    // 禁止复制槽位、队列及其工作线程。
    PackedWritePipeline(const PackedWritePipeline&) = delete;
    // 禁止复制赋值，保持流水线状态的独占所有权。
    PackedWritePipeline& operator=(const PackedWritePipeline&) = delete;

    // 交换输入缓冲以提交一个字符块；槽位耗尽时等待回收，同步模式直接完成写出。
    void submit(std::vector<char>& residues, size_t residue_count) {
        if (residue_count == 0 || residue_count > kPackingResiduesPerJob) {
            throw std::invalid_argument("invalid packed pipeline job size");
        }
        if (synchronous_) {
            Slot& slot = slots_[0];
            slot.residues.swap(residues);
            slot.residue_count = residue_count;
            try {
                encode_slot(slot);
                output_.append(
                    reinterpret_cast<const char*>(slot.packed.data()),
                    slot.packed_bytes);
            } catch (...) {
                slot.residues.swap(residues);
                throw;
            }
            slot.residues.swap(residues);
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        const size_t slot_index =
            static_cast<size_t>(submitted_ % slots_.size());
        while (!cancelled_ && slots_[slot_index].state != SlotState::Free) {
            changed_.wait(lock);
        }
        if (cancelled_) {
            const std::exception_ptr error = error_;
            lock.unlock();
            if (error) std::rethrow_exception(error);
            throw std::runtime_error("packed write pipeline was cancelled");
        }
        if (submissions_closed_) {
            throw std::logic_error("packed write pipeline is closed");
        }

        Slot& slot = slots_[slot_index];
        slot.residues.swap(residues);
        slot.residue_count = residue_count;
        slot.job_id = submitted_;
        slot.state = encoder_threads_ == 0 && !fused_worker_
            ? SlotState::Encoding
            : SlotState::Queued;
        if (encoder_threads_ != 0 || fused_worker_) {
            if (work_count_ == work_queue_.size()) {
                throw std::logic_error("packed pipeline work queue overflow");
            }
            work_queue_[work_tail_] = slot_index;
            work_tail_ = (work_tail_ + 1) % work_queue_.size();
            ++work_count_;
        }
        ++submitted_;
        lock.unlock();
        changed_.notify_all();

        if (encoder_threads_ == 0 && !fused_worker_) {
            try {
                encode_slot(slot);
            } catch (...) {
                fail(std::current_exception());
                throw;
            }
            {
                std::lock_guard<std::mutex> complete_lock(mutex_);
                if (cancelled_) return;
                slot.state = SlotState::Complete;
            }
            changed_.notify_all();
        }
    }

    // 关闭提交端并排空工作线程，随后传播后台记录的首个异常。
    void finish() {
        if (synchronous_) {
            finished_ = true;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            submissions_closed_ = true;
        }
        changed_.notify_all();
        join_threads();
        finished_ = true;
        if (error_) std::rethrow_exception(error_);
    }

private:
    enum class SlotState : uint8_t {
        Free,
        Queued,
        Encoding,
        Complete
    };

    struct Slot {
        std::vector<char> residues;
        std::vector<uint8_t> packed;
        size_t residue_count = 0;
        size_t packed_bytes = 0;
        uint64_t job_id = 0;
        SlotState state = SlotState::Free;
    };

    // 清零槽位的有效输出区并执行 5 位编码，校验输入字符全部被处理。
    static void encode_slot(Slot& slot) {
        slot.packed_bytes = static_cast<size_t>(
            packed_bytes_for_residues(slot.residue_count));
        std::fill(slot.packed.begin(),
                  slot.packed.begin() + slot.packed_bytes, uint8_t{0});
        uint64_t encoded = 0;
        append_packed_5bit(slot.packed.data(), encoded,
                           slot.residues.data(), slot.residue_count);
        if (encoded != slot.residue_count) {
            throw std::logic_error("incomplete packed job encoding");
        }
    }

    // 消费编码任务并标记完成槽位；失败时取消流水线并通知等待者。
    void encode_loop() noexcept {
        while (true) {
            size_t slot_index = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                while (!cancelled_ && work_count_ == 0 &&
                       !submissions_closed_) {
                    changed_.wait(lock);
                }
                if (cancelled_ ||
                    (work_count_ == 0 && submissions_closed_)) {
                    break;
                }
                slot_index = work_queue_[work_head_];
                work_head_ = (work_head_ + 1) % work_queue_.size();
                --work_count_;
                slots_[slot_index].state = SlotState::Encoding;
            }
            try {
                encode_slot(slots_[slot_index]);
            } catch (...) {
                fail(std::current_exception());
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_) break;
                slots_[slot_index].state = SlotState::Complete;
            }
            changed_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_encoders_ != 0) --active_encoders_;
        }
        changed_.notify_all();
    }

    // 在同一后台线程中按提交顺序编码和写出，完成后立即归还槽位。
    void encode_write_loop() noexcept {
        while (true) {
            size_t slot_index = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                while (!cancelled_ && work_count_ == 0 &&
                       !submissions_closed_) {
                    changed_.wait(lock);
                }
                if (cancelled_ ||
                    (work_count_ == 0 && submissions_closed_)) {
                    break;
                }
                slot_index = work_queue_[work_head_];
                work_head_ = (work_head_ + 1) % work_queue_.size();
                --work_count_;
                slots_[slot_index].state = SlotState::Encoding;
            }
            try {
                Slot& slot = slots_[slot_index];
                encode_slot(slot);
                output_.append(
                    reinterpret_cast<const char*>(slot.packed.data()),
                    slot.packed_bytes);
            } catch (...) {
                fail(std::current_exception());
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_) break;
                slots_[slot_index].state = SlotState::Free;
                ++next_write_;
            }
            changed_.notify_all();
        }
    }

    // 等待下一个任务编码完成，严格按提交顺序写出 packed 数据并释放槽位。
    void write_loop() noexcept {
        while (true) {
            size_t slot_index = 0;
            size_t packed_bytes = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                while (!cancelled_) {
                    if (next_write_ < submitted_) {
                        const Slot& next_slot = slots_[static_cast<size_t>(
                            next_write_ % slots_.size())];
                        if (next_slot.state == SlotState::Complete) break;
                    } else if (submissions_closed_ && active_encoders_ == 0) {
                        break;
                    }
                    changed_.wait(lock);
                }
                if (cancelled_) return;
                if (next_write_ == submitted_) {
                    if (submissions_closed_ && active_encoders_ == 0) return;
                    continue;
                }
                slot_index = static_cast<size_t>(
                    next_write_ % slots_.size());
                const Slot& slot = slots_[slot_index];
                if (slot.job_id != next_write_) {
                    lock.unlock();
                    fail(std::make_exception_ptr(std::logic_error(
                        "packed pipeline output order mismatch")));
                    return;
                }
                packed_bytes = slot.packed_bytes;
            }

            Slot& slot = slots_[slot_index];
            try {
                output_.append(
                    reinterpret_cast<const char*>(slot.packed.data()),
                    packed_bytes);
            } catch (...) {
                fail(std::current_exception());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_) return;
                slot.state = SlotState::Free;
                ++next_write_;
            }
            changed_.notify_all();
        }
    }

    // 保存首个后台异常，关闭提交并唤醒所有受取消状态影响的线程。
    void fail(std::exception_ptr error) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error_) error_ = error;
            cancelled_ = true;
            submissions_closed_ = true;
        }
        changed_.notify_all();
    }

    // 等待全部编码线程和写入线程退出。
    void join_threads() noexcept {
        for (std::thread& encoder : encoders_) {
            if (encoder.joinable()) encoder.join();
        }
        if (writer_.joinable()) writer_.join();
    }

    // 发出取消通知并回收线程，用于构造失败或未显式完成的析构路径。
    void cancel_and_join() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            submissions_closed_ = true;
        }
        changed_.notify_all();
        join_threads();
        finished_ = true;
    }

    BufferedFileWriter& output_;
    std::vector<Slot> slots_;
    std::vector<size_t> work_queue_;
    std::vector<std::thread> encoders_;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable changed_;
    size_t work_head_ = 0;
    size_t work_tail_ = 0;
    size_t work_count_ = 0;
    size_t active_encoders_ = 0;
    size_t encoder_threads_ = 0;
    bool fused_worker_ = false;
    uint64_t submitted_ = 0;
    uint64_t next_write_ = 0;
    bool synchronous_ = false;
    bool submissions_closed_ = false;
    bool cancelled_ = false;
    bool finished_ = false;
    std::exception_ptr error_;
};

}  // namespace

// 为解析线程之外的预算安排文本写出、packed 编码和写出，保证不超出总线程数。
BuildThreadPlan plan_build_threads(size_t total_threads,
                                   bool write_text_columns) {
    if (total_threads == 0) {
        throw std::invalid_argument(
            "build threads must be greater than zero");
    }
    BuildThreadPlan plan;
    if (total_threads == 1) return plan;

    if (write_text_columns) {
        plan.text_writer_async = true;
        if (total_threads == 3) {
            plan.packed_fused_worker = true;
        } else if (total_threads >= 4) {
            plan.packed_writer_async = true;
            plan.packed_encoder_threads = total_threads - 3;
        }
    } else if (total_threads == 2) {
        plan.packed_fused_worker = true;
    } else {
        plan.packed_writer_async = true;
        plan.packed_encoder_threads = total_threads - 2;
    }
    return plan;
}

class SequenceBuildWriter::Impl {
public:
    // 创建 packed 与边界输出，绑定流水线并分配固定容量的字符块缓冲。
    Impl(const std::filesystem::path& packed_path,
         const std::filesystem::path& metadata_path,
         const BuildThreadPlan& threads)
        : packed_(packed_path),
          metadata_(metadata_path),
          pipeline_(packed_, threads.packed_encoder_threads,
                    threads.packed_writer_async,
                    threads.packed_fused_worker),
          residue_chunk_(kPackingResiduesPerJob) {}

    // 将规范化字符填入编码块，满块提交流水线，并累计全局残基位置。
    void append_residues(std::string_view residues) {
        if (finished_) {
            throw std::logic_error("append on finished sequence writer");
        }
        size_t consumed = 0;
        while (consumed < residues.size()) {
            if (chunk_fill_ == kPackingResiduesPerJob) flush_chunk();
            const size_t take = std::min(
                residues.size() - consumed,
                kPackingResiduesPerJob - chunk_fill_);
            std::memcpy(residue_chunk_.data() + chunk_fill_,
                        residues.data() + consumed, take);
            chunk_fill_ += take;
            consumed += take;
            stats_.residues += take;
        }
    }

    // 结束当前记录并追加残基边界；不因记录结束而打断连续编码块。
    void finish_sequence() {
        if (finished_) {
            throw std::logic_error("finish sequence on closed writer");
        }
        ++stats_.sequences;
        metadata_.append(stats_.residues);
    }

    // 提交末尾块并排空流水线，关闭 packed 文件并完成序列索引。
    void finish() {
        if (finished_) return;
        flush_chunk();
        pipeline_.finish();
        packed_.close();
        metadata_.finish(stats_.sequences, stats_.residues);
        finished_ = true;
    }

    // 提交非空字符块并累计对应编码字节数，重置块内有效长度。
    void flush_chunk() {
        if (chunk_fill_ == 0) return;
        pipeline_.submit(residue_chunk_, chunk_fill_);
        stats_.packed_bytes += packed_bytes_for_residues(chunk_fill_);
        chunk_fill_ = 0;
    }

    BufferedFileWriter packed_;
    CompactMetadataWriter metadata_;
    PackedWritePipeline pipeline_;
    std::vector<char> residue_chunk_;
    size_t chunk_fill_ = 0;
    BuildStats stats_;
    bool finished_ = false;
};

// 构造序列写入实现，绑定目标文件和线程布局。
SequenceBuildWriter::SequenceBuildWriter(
        const std::filesystem::path& packed_path,
        const std::filesystem::path& metadata_path,
        const BuildThreadPlan& threads)
    : impl_(std::make_unique<Impl>(packed_path, metadata_path, threads)) {}

// 释放内部写入实现，由其成员回收文件、缓冲和未完成的后台任务。
SequenceBuildWriter::~SequenceBuildWriter() = default;

// 将一段已规范化字符追加到内部序列编码流。
void SequenceBuildWriter::append_residues(std::string_view residues) {
    impl_->append_residues(residues);
}

// 通知内部写入器记录当前序列的结束位置。
void SequenceBuildWriter::finish_sequence() {
    impl_->finish_sequence();
}

// 显式结束序列输出，等待所有字符编码并写出索引。
void SequenceBuildWriter::finish() {
    impl_->finish();
}

// 借用内部累计统计，供建库协调器汇总序列数量和编码大小。
const BuildStats& SequenceBuildWriter::stats() const noexcept {
    return impl_->stats_;
}

}  // namespace aastore
