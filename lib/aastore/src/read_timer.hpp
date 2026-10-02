// 文件职责：提供可关闭的读取诊断计时器，关闭时不访问时钟。
#ifndef AASTORE_READ_TIMER_HPP
#define AASTORE_READ_TIMER_HPP

#include <chrono>
#include <cstdint>

namespace aastore {

class DiagnosticTimer {
public:
    // 仅在启用诊断时记录起点。
    explicit DiagnosticTimer(bool enabled) : enabled_(enabled) {
        if (enabled_) start_ = Clock::now();
    }

    // 返回经过的纳秒数；未启用时直接返回零。
    uint64_t elapsed_ns() const {
        if (!enabled_) return 0;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start_).count());
    }

private:
    using Clock = std::chrono::steady_clock;
    bool enabled_;
    Clock::time_point start_{};
};

}  // namespace aastore

#endif
