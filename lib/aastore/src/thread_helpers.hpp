// 文件职责：在正常返回和异常退出时等待已启动的工作线程，保护其借用资源。
#ifndef AASTORE_THREAD_HELPERS_HPP
#define AASTORE_THREAD_HELPERS_HPP

#include <thread>
#include <vector>

namespace aastore {

// 线程启动或调用线程任务抛异常时，先等待已启动的线程结束再释放其借用资源。
class ThreadJoinGuard {
public:
    explicit ThreadJoinGuard(std::vector<std::thread>& threads) noexcept
        : threads_(threads) {}
    ~ThreadJoinGuard() { join_all(); }
    ThreadJoinGuard(const ThreadJoinGuard&) = delete;
    ThreadJoinGuard& operator=(const ThreadJoinGuard&) = delete;

    void join_all() noexcept {
        for (std::thread& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
    }

private:
    // 借用调用方的线程容器，不拥有它；容器和线程借用的数据须晚于 guard 销毁。
    std::vector<std::thread>& threads_;
};

}  // namespace aastore

#endif
