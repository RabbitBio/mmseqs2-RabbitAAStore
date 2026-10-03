# Find liburing on the current machine, never in the SDK producer's directories.
find_path(AAStoreUring_INCLUDE_DIR NAMES liburing.h)
find_library(AAStoreUring_LIBRARY NAMES uring)

set(AAStoreUring_ABI "")
set(_aastore_uring_abi_checks
    AAStoreUring_SIZE_RING AAStoreUring_SIZE_SQ AAStoreUring_SIZE_CQ
    AAStoreUring_SIZE_SQE AAStoreUring_SIZE_CQE
    AAStoreUring_OFFSET_RING_CQ AAStoreUring_OFFSET_RING_FD
    AAStoreUring_OFFSET_CQ_CQES AAStoreUring_OFFSET_SQ_SQES)
if(AAStoreUring_INCLUDE_DIR AND AAStoreUring_LIBRARY)
    set(_aastore_uring_files "${AAStoreUring_INCLUDE_DIR}|${AAStoreUring_LIBRARY}")
    # Package upgrades may replace headers/libraries without changing paths.
    foreach(_aastore_file IN ITEMS
            "${AAStoreUring_INCLUDE_DIR}/liburing.h"
            "${AAStoreUring_INCLUDE_DIR}/liburing/io_uring.h"
            "${AAStoreUring_LIBRARY}")
        if(EXISTS "${_aastore_file}")
            file(SHA256 "${_aastore_file}" _aastore_file_hash)
            string(APPEND _aastore_uring_files "|${_aastore_file_hash}")
        endif()
    endforeach()
    unset(_aastore_file)
    unset(_aastore_file_hash)
    if(NOT "${_aastore_uring_files}" STREQUAL "${AAStoreUring_CAPABILITY_FILES}")
        unset(AAStoreUring_HAS_BASELINE CACHE)
        unset(AAStoreUring_HAS_READ CACHE)
        unset(AAStoreUring_HAS_PROBE CACHE)
        foreach(_aastore_check IN LISTS _aastore_uring_abi_checks)
            unset(${_aastore_check} CACHE)
            unset(HAVE_${_aastore_check} CACHE)
        endforeach()
        unset(_aastore_check)
        set(AAStoreUring_CAPABILITY_FILES "${_aastore_uring_files}"
            CACHE INTERNAL "liburing files checked for API capabilities")
    endif()
    unset(_aastore_uring_files)
    if(NOT TARGET AAStoreUring::AAStoreUring)
        add_library(AAStoreUring::AAStoreUring UNKNOWN IMPORTED)
        set_target_properties(AAStoreUring::AAStoreUring PROPERTIES
            IMPORTED_LOCATION "${AAStoreUring_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AAStoreUring_INCLUDE_DIR}")
    endif()

    # Distribution packages can provide READV without the later READ/probe APIs.
    # Check the headers and library together instead of assuming a version.
    function(_aastore_uring_check_capabilities)
        include(CheckCXXSourceCompiles)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_LIBRARIES AAStoreUring::AAStoreUring)
        set(CMAKE_REQUIRED_QUIET TRUE)
        # Some cross toolchains use STATIC_LIBRARY for try_compile. API checks
        # must also resolve the library's symbols, without running target code.
        # Function scope preserves the caller's value (including an unset one).
        set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
        # Recheck on every configure: older caches may record compile-only
        # successes, and linker flags or the target sysroot may have changed.
        foreach(_aastore_api IN ITEMS
                AAStoreUring_HAS_BASELINE AAStoreUring_HAS_READ AAStoreUring_HAS_PROBE)
            unset(${_aastore_api})
            unset(${_aastore_api} CACHE)
        endforeach()
        check_cxx_source_compiles([[
            #include <liburing.h>
            int main() {
                io_uring ring = {};
                io_uring_params params = {};
                io_uring_queue_init_params(1, &ring, &params);
                io_uring_sqe* sqe = io_uring_get_sqe(&ring);
                iovec buffer = {};
                io_uring_prep_readv(sqe, -1, &buffer, 1, 0);
                sqe->user_data = 0;
                io_uring_submit(&ring);
                io_uring_cqe* cqe = 0;
                io_uring_peek_cqe(&ring, &cqe);
                io_uring_wait_cqe(&ring, &cqe);
                (void)cqe->res;
                (void)cqe->user_data;
                (void)io_uring_cq_ready(&ring);
                io_uring_cqe_seen(&ring, cqe);
                io_uring_queue_exit(&ring);
                return IORING_OP_READV == 1 ? 0 : 1;
            }
        ]] AAStoreUring_HAS_BASELINE)
        if(AAStoreUring_HAS_BASELINE)
            check_cxx_source_compiles([[
                #include <liburing.h>
                int main() {
                    io_uring_sqe sqe = {};
                    char buffer = 0;
                    io_uring_prep_read(&sqe, -1, &buffer, 1, 0);
                    return sqe.opcode == IORING_OP_READ ? 0 : 1;
                }
            ]] AAStoreUring_HAS_READ)
            check_cxx_source_compiles([[
                #include <cstdlib>
                #include <liburing.h>
                int main() {
                    io_uring ring = {};
                    io_uring_probe* probe = io_uring_get_probe_ring(&ring);
                    const int supported = probe
                        ? io_uring_opcode_supported(probe, IORING_OP_READV) : 0;
                    std::free(probe);
                    return supported;
                }
            ]] AAStoreUring_HAS_PROBE)

            # Inline helpers and the linked library share these public structures.
            # A baseline API can exist in libraries with incompatible ring layouts.
            # CheckTypeSize extracts values from compiled objects without executing
            # target programs, including when cross-compiling.
            set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
            include(CheckTypeSize)
            set(CMAKE_REQUIRED_INCLUDES "${AAStoreUring_INCLUDE_DIR}")
            set(_aastore_extra_include_files "${CMAKE_EXTRA_INCLUDE_FILES}")
            set(CMAKE_EXTRA_INCLUDE_FILES liburing.h cstddef)
            check_type_size("struct io_uring" AAStoreUring_SIZE_RING LANGUAGE CXX)
            check_type_size("struct io_uring_sq" AAStoreUring_SIZE_SQ LANGUAGE CXX)
            check_type_size("struct io_uring_cq" AAStoreUring_SIZE_CQ LANGUAGE CXX)
            check_type_size("struct io_uring_sqe" AAStoreUring_SIZE_SQE LANGUAGE CXX)
            check_type_size("struct io_uring_cqe" AAStoreUring_SIZE_CQE LANGUAGE CXX)
            # Array sizes represent each offset plus one, allowing offset zero.
            check_type_size("char[offsetof(struct io_uring, cq) + 1]"
                AAStoreUring_OFFSET_RING_CQ LANGUAGE CXX)
            check_type_size("char[offsetof(struct io_uring, ring_fd) + 1]"
                AAStoreUring_OFFSET_RING_FD LANGUAGE CXX)
            check_type_size("char[offsetof(struct io_uring_cq, cqes) + 1]"
                AAStoreUring_OFFSET_CQ_CQES LANGUAGE CXX)
            check_type_size("char[offsetof(struct io_uring_sq, sqes) + 1]"
                AAStoreUring_OFFSET_SQ_SQES LANGUAGE CXX)
            set(CMAKE_EXTRA_INCLUDE_FILES "${_aastore_extra_include_files}")
            unset(_aastore_extra_include_files)
            set(_aastore_abi_values)
            set(_aastore_abi_complete TRUE)
            foreach(_aastore_check IN LISTS _aastore_uring_abi_checks)
                if(NOT "${${_aastore_check}}" MATCHES "^[1-9][0-9]*$")
                    set(_aastore_abi_complete FALSE)
                endif()
                list(APPEND _aastore_abi_values "${${_aastore_check}}")
            endforeach()
            if(_aastore_abi_complete)
                string(JOIN ":" AAStoreUring_ABI ${_aastore_abi_values})
            endif()
            unset(_aastore_check)
            unset(_aastore_abi_values)
            unset(_aastore_abi_complete)
        endif()
        cmake_pop_check_state()
        set(AAStoreUring_ABI "${AAStoreUring_ABI}" PARENT_SCOPE)
    endfunction()
    _aastore_uring_check_capabilities()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AAStoreUring
    REQUIRED_VARS AAStoreUring_LIBRARY AAStoreUring_INCLUDE_DIR
                  AAStoreUring_HAS_BASELINE AAStoreUring_ABI)
mark_as_advanced(AAStoreUring_INCLUDE_DIR AAStoreUring_LIBRARY)
unset(_aastore_uring_abi_checks)
