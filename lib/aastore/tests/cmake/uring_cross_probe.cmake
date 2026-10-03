cmake_minimum_required(VERSION 3.16)

if(NOT AASTORE_TEST_BINARY_DIR)
    message(FATAL_ERROR "AASTORE_TEST_BINARY_DIR is required")
endif()
if(NOT AASTORE_TEST_MODULE_DIR)
    get_filename_component(AASTORE_TEST_MODULE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../cmake" ABSOLUTE)
endif()
file(REMOVE_RECURSE "${AASTORE_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${AASTORE_TEST_BINARY_DIR}")

# Keep generated fixture sources beside the test builds, outside the checkout.
set(fixture_source "${AASTORE_TEST_BINARY_DIR}/fixture")
file(MAKE_DIRECTORY "${fixture_source}/libraries" "${fixture_source}/probe")
file(WRITE "${fixture_source}/liburing.h" [=[
#ifndef AASTORE_TEST_CROSS_PROBE_LIBURING_H
#define AASTORE_TEST_CROSS_PROBE_LIBURING_H

#include <sys/uio.h>

enum { IORING_OP_READV = 1, IORING_OP_READ = 22 };
struct io_uring_sqe {
    unsigned opcode;
    unsigned long long user_data;
};
struct io_uring_cqe {
    unsigned long long user_data;
    int res;
};
struct io_uring_sq { io_uring_sqe* sqes; };
struct io_uring_cq { io_uring_cqe* cqes; };
struct io_uring { io_uring_sq sq; io_uring_cq cq; int ring_fd; };
struct io_uring_params { unsigned flags; };
struct io_uring_probe { unsigned operations; };

// These declarations deliberately require linked definitions. No probe runs.
extern "C" {
int io_uring_queue_init_params(unsigned, io_uring*, io_uring_params*);
void io_uring_queue_exit(io_uring*);
io_uring_sqe* io_uring_get_sqe(io_uring*);
void io_uring_prep_readv(io_uring_sqe*, int, const iovec*, unsigned,
                        unsigned long long);
int io_uring_submit(io_uring*);
int io_uring_peek_cqe(io_uring*, io_uring_cqe**);
int io_uring_wait_cqe(io_uring*, io_uring_cqe**);
unsigned io_uring_cq_ready(io_uring*);
void io_uring_cqe_seen(io_uring*, io_uring_cqe*);
void io_uring_prep_read(io_uring_sqe*, int, void*, unsigned,
                       unsigned long long);
io_uring_probe* io_uring_get_probe_ring(io_uring*);
int io_uring_opcode_supported(io_uring_probe*, int);
}

#endif
]=])
file(WRITE "${fixture_source}/libraries/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.16)
project(AAStoreUringProbeLibraries LANGUAGES CXX)

add_library(empty STATIC empty.cpp)
add_library(baseline STATIC liburing.cpp)
add_library(complete STATIC liburing.cpp)
target_compile_definitions(complete PRIVATE AASTORE_TEST_COMPLETE_LIBURING)
target_include_directories(baseline PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(complete PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_target_properties(empty baseline complete PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(empty baseline complete PROPERTIES
        "ARCHIVE_OUTPUT_DIRECTORY_${config}" "${CMAKE_CURRENT_BINARY_DIR}/lib")
endforeach()
]=])
file(WRITE "${fixture_source}/libraries/liburing.cpp" [=[
#include <cstdlib>
#include <liburing.h>

// Calling any target-side API fails. These tests may compile and link only.
extern "C" {
int io_uring_queue_init_params(unsigned, io_uring*, io_uring_params*) {
    std::abort();
}
void io_uring_queue_exit(io_uring*) { std::abort(); }
io_uring_sqe* io_uring_get_sqe(io_uring*) { std::abort(); }
void io_uring_prep_readv(io_uring_sqe*, int, const iovec*, unsigned,
                        unsigned long long) { std::abort(); }
int io_uring_submit(io_uring*) { std::abort(); }
int io_uring_peek_cqe(io_uring*, io_uring_cqe**) { std::abort(); }
int io_uring_wait_cqe(io_uring*, io_uring_cqe**) { std::abort(); }
unsigned io_uring_cq_ready(io_uring*) { std::abort(); }
void io_uring_cqe_seen(io_uring*, io_uring_cqe*) { std::abort(); }

#ifdef AASTORE_TEST_COMPLETE_LIBURING
void io_uring_prep_read(io_uring_sqe*, int, void*, unsigned,
                       unsigned long long) { std::abort(); }
io_uring_probe* io_uring_get_probe_ring(io_uring*) { std::abort(); }
int io_uring_opcode_supported(io_uring_probe*, int) { std::abort(); }
#endif
}
]=])
file(WRITE "${fixture_source}/libraries/empty.cpp" [=[
// A valid archive that provides none of the APIs declared in liburing.h.
extern "C" int aastore_empty_archive() { return 0; }
]=])
file(WRITE "${fixture_source}/probe/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.16)
project(AAStoreUringCrossProbe LANGUAGES CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The regression must configure a cross-compiling project")
endif()

set(AAStoreUring_INCLUDE_DIR "${AASTORE_TEST_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(AAStoreUring_LIBRARY "${AASTORE_TEST_LIBRARY}" CACHE FILEPATH "" FORCE)
# Recreate an old configure cache whose compile-only checks all succeeded.
# Its file fingerprint is correct, so file-change invalidation cannot mask a
# missing migration/recheck of these cached API capabilities.
file(SHA256 "${AAStoreUring_INCLUDE_DIR}/liburing.h" header_hash)
file(SHA256 "${AAStoreUring_LIBRARY}" library_hash)
set(AAStoreUring_CAPABILITY_FILES
    "${AAStoreUring_INCLUDE_DIR}|${AAStoreUring_LIBRARY}|${header_hash}|${library_hash}"
    CACHE INTERNAL "" FORCE)
foreach(capability IN ITEMS BASELINE READ PROBE)
    set("AAStoreUring_HAS_${capability}" TRUE CACHE INTERNAL "" FORCE)
endforeach()

include("${AASTORE_TEST_MODULE_DIR}/FindAAStoreUring.cmake")

if(AASTORE_TEST_EXPECT_FOUND)
    if(NOT AAStoreUring_FOUND)
        message(FATAL_ERROR "A usable liburing library was rejected")
    endif()
    if(NOT AAStoreUring_ABI MATCHES "^[0-9]+(:[0-9]+)+$")
        message(FATAL_ERROR "Cross-compilation did not extract the ABI layout")
    endif()
else()
    if(AAStoreUring_FOUND OR AAStoreUring_HAS_BASELINE)
        message(FATAL_ERROR "An archive without liburing symbols was accepted")
    endif()
endif()
foreach(capability IN ITEMS READ PROBE)
    if(AASTORE_TEST_EXPECT_OPTIONAL)
        if(NOT AAStoreUring_HAS_${capability})
            message(FATAL_ERROR "A linked ${capability} API was rejected")
        endif()
    elseif(AAStoreUring_HAS_${capability})
        message(FATAL_ERROR "An unlinked ${capability} API was accepted")
    endif()
endforeach()

if(NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "The caller's try_compile target type changed")
endif()
]=])

set(common_arguments)
if(AASTORE_TEST_CXX_COMPILER)
    list(APPEND common_arguments "-DCMAKE_CXX_COMPILER=${AASTORE_TEST_CXX_COMPILER}")
endif()
if(DEFINED AASTORE_TEST_CXX_FLAGS)
    list(APPEND common_arguments "-DCMAKE_CXX_FLAGS=${AASTORE_TEST_CXX_FLAGS}")
endif()
if(AASTORE_TEST_GENERATOR)
    list(APPEND common_arguments -G "${AASTORE_TEST_GENERATOR}")
endif()

function(run_command)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}\n${error}")
    endif()
endfunction()

set(library_build "${AASTORE_TEST_BINARY_DIR}/libraries")
run_command("${CMAKE_COMMAND}" -S "${fixture_source}/libraries"
    -B "${library_build}" ${common_arguments} -DCMAKE_BUILD_TYPE=Release)
run_command("${CMAKE_COMMAND}" --build "${library_build}" --config Release)

# Setting CMAKE_SYSTEM_NAME marks even a same-architecture compiler as cross.
# This exercises CMake's cross-compiling behavior without a platform toolchain.
set(toolchain "${AASTORE_TEST_BINARY_DIR}/cross-toolchain.cmake")
file(WRITE "${toolchain}"
    "set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n")
foreach(library IN ITEMS empty baseline complete)
    set(expected_found TRUE)
    set(expected_optional FALSE)
    if(library STREQUAL "empty")
        set(expected_found FALSE)
    elseif(library STREQUAL "complete")
        set(expected_optional TRUE)
    endif()
    run_command("${CMAKE_COMMAND}" -S "${fixture_source}/probe"
        -B "${AASTORE_TEST_BINARY_DIR}/${library}"
        ${common_arguments} "-DCMAKE_TOOLCHAIN_FILE=${toolchain}"
        "-DAASTORE_TEST_MODULE_DIR=${AASTORE_TEST_MODULE_DIR}"
        "-DAASTORE_TEST_INCLUDE_DIR=${fixture_source}"
        "-DAASTORE_TEST_LIBRARY=${library_build}/lib/lib${library}.a"
        "-DAASTORE_TEST_EXPECT_FOUND=${expected_found}"
        "-DAASTORE_TEST_EXPECT_OPTIONAL=${expected_optional}")
endforeach()
message(STATUS "liburing cross-compilation checks passed for all three archives")
