#ifndef AASTORE_TEST_GUARDED_BUFFER_HPP
#define AASTORE_TEST_GUARDED_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace aastore_test {

// Place an exact-size test buffer immediately before an inaccessible page.
class GuardedBuffer {
public:
    GuardedBuffer() : page_(static_cast<std::size_t>(::sysconf(_SC_PAGESIZE))) {
        void* mapping = ::mmap(nullptr, page_ * 2, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) throw std::runtime_error("mmap failed");
        data_ = static_cast<std::uint8_t*>(mapping);
        if (::mprotect(data_ + page_, page_, PROT_NONE) != 0) {
            ::munmap(data_, page_ * 2);
            throw std::runtime_error("mprotect failed");
        }
    }
    ~GuardedBuffer() { ::munmap(data_, page_ * 2); }
    GuardedBuffer(const GuardedBuffer&) = delete;
    GuardedBuffer& operator=(const GuardedBuffer&) = delete;

    std::uint8_t* ending_at_guard(std::size_t size) {
        if (size >= page_) throw std::runtime_error("test buffer too large");
        return data_ + page_ - size;
    }

private:
    std::size_t page_;
    std::uint8_t* data_;
};

}  // namespace aastore_test

#endif
