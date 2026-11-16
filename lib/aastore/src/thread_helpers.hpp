#ifndef AASTORE_THREAD_HELPERS_HPP
#define AASTORE_THREAD_HELPERS_HPP

#include <thread>
#include <vector>

namespace aastore {

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
    std::vector<std::thread>& threads_;
};

}  // namespace aastore

#endif
