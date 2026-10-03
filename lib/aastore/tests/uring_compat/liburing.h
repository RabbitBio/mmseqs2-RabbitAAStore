// 文件职责：为兼容性测试提供延迟执行请求的私有 liburing 替身。
// 仅测试目标包含本目录，生产库和安装 SDK 不使用此头文件。
#ifndef AASTORE_TEST_LIBURING_H
#define AASTORE_TEST_LIBURING_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <sys/uio.h>

enum { IORING_OP_READV = 1, IORING_OP_READ = 22 };
struct io_uring_sqe {
    unsigned opcode = 0;
    int fd = -1;
    const void* address = nullptr;
    unsigned length = 0;
    off_t offset = 0;
    unsigned long long user_data = 0;
};
struct io_uring_cqe {
    unsigned long long user_data = 0;
    int res = 0;
};
struct io_uring_params {};
struct io_uring_probe { bool read; bool readv; };
struct AastoreTestRing;
struct io_uring { AastoreTestRing* state = nullptr; };

extern "C" {
int io_uring_queue_init_params(unsigned, io_uring*, io_uring_params*);
void io_uring_queue_exit(io_uring*);
#if !defined(AASTORE_TEST_LEGACY_LIBURING)
io_uring_probe* io_uring_get_probe_ring(io_uring*);
int io_uring_opcode_supported(io_uring_probe*, int);
#endif
io_uring_sqe* io_uring_get_sqe(io_uring*);
#if !defined(AASTORE_TEST_LEGACY_LIBURING)
void io_uring_prep_read(io_uring_sqe*, int, void*, unsigned, off_t);
#endif
void io_uring_prep_readv(io_uring_sqe*, int, const iovec*, unsigned, off_t);
int io_uring_submit(io_uring*);
int io_uring_wait_cqe(io_uring*, io_uring_cqe**);
int io_uring_peek_cqe(io_uring*, io_uring_cqe**);
unsigned io_uring_cq_ready(io_uring*);
void io_uring_cqe_seen(io_uring*, io_uring_cqe*);
}

#endif
