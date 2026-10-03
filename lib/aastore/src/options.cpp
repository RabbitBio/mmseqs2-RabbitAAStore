// 文件职责：集中校验公开选项，在打开文件或执行读取前拒绝无效配置。
#include "aastore/builder.hpp"
#include "aastore/read_options.hpp"

#include <stdexcept>

namespace aastore {

void BuildOptions::validate() const {
    if (encode_threads == 0) {
        throw std::invalid_argument("build threads must be greater than zero");
    }
}

void StoreOptions::validate() const {
    switch (storage_mode) {
        case StorageMode::ByteRange:
        case StorageMode::FullMemory:
            break;
        case StorageMode::PrefixMemory:
            if (cache_bytes == 0) {
                throw std::invalid_argument(
                    "prefix-memory capacity must be greater than zero");
            }
            break;
        default:
            throw std::invalid_argument("unknown storage mode");
    }
}

void ReadOptions::validate() const {
    switch (io_backend) {
        case IoBackend::Auto:
        case IoBackend::Pread:
        case IoBackend::IoUring:
            break;
        default:
            throw std::invalid_argument("unknown I/O backend");
    }
}

}  // namespace aastore
