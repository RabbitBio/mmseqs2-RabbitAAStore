// 文件职责：统一持有边界索引的只读映射或完整读取缓冲。
#ifndef AASTORE_INDEX_DATA_HPP
#define AASTORE_INDEX_DATA_HPP

#include "aastore/read_options.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace aastore {

class IndexData {
public:
    IndexData() = default;
    ~IndexData() { clear(); }
    IndexData(const IndexData&) = delete;
    IndexData& operator=(const IndexData&) = delete;

    // 打开前调用方已校验文件尺寸；读错或短读直接报告，不切换加载方式。
    void open(const std::filesystem::path& path, size_t bytes, IndexLoadMode mode) {
        if (mode != IndexLoadMode::Mmap && mode != IndexLoadMode::Read) {
            throw std::invalid_argument("unknown index load mode");
        }
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            throw std::runtime_error("cannot open " + path.string() + ": " +
                                     std::strerror(errno));
        }
        try {
            if (mode == IndexLoadMode::Read) {
                std::unique_ptr<char[]> buffer(new char[bytes]);
                size_t done = 0;
                while (done < bytes) {
                    const size_t count = std::min(bytes - done, size_t{8 * 1024 * 1024});
                    const ssize_t result = ::read(descriptor, buffer.get() + done, count);
                    if (result < 0) {
                        if (errno == EINTR) continue;
                        throw std::runtime_error("cannot read " + path.string() + ": " +
                                                 std::strerror(errno));
                    }
                    if (result == 0) {
                        throw std::runtime_error("short index read: " + path.string());
                    }
                    done += static_cast<size_t>(result);
                }
                clear();
                owned_ = std::move(buffer);
                data_ = owned_.get();
            } else {
                void* mapping = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE,
                                       descriptor, 0);
                if (mapping == MAP_FAILED) {
                    throw std::runtime_error("cannot mmap " + path.string() + ": " +
                                             std::strerror(errno));
                }
                clear();
                mapping_ = mapping;
                data_ = static_cast<const char*>(mapping);
            }
            bytes_ = bytes;
        } catch (...) {
            ::close(descriptor);
            throw;
        }
        ::close(descriptor);
    }

    const char* data() const noexcept { return data_; }

    void clear() noexcept {
        if (mapping_ != nullptr) ::munmap(mapping_, bytes_);
        mapping_ = nullptr;
        owned_.reset();
        data_ = nullptr;
        bytes_ = 0;
    }

private:
    void* mapping_ = nullptr;
    std::unique_ptr<char[]> owned_;
    const char* data_ = nullptr;
    size_t bytes_ = 0;
};

}  // namespace aastore

#endif
