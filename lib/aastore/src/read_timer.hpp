// 文件职责：提供可关闭的读取诊断计时器，关闭时不访问时钟。
#ifndef AASTORE_READ_TIMER_HPP
#define AASTORE_READ_TIMER_HPP

#include <chrono>
#include <cstdint>

namespace aastore {

// 可选的作用域阶段计时器，只保存起点，不负责发布或累加统计。
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
    // 是否采集时间；关闭时构造和查询都不读取系统时钟。
    bool enabled_;
    // 启用诊断时记录的单调时钟起点，避免系统时间调整影响耗时。
    Clock::time_point start_{};
};

}  // namespace aastore

#endif
