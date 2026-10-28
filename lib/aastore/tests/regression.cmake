# Full regression targets, included from the RabbitAAStore root CMakeLists.txt.
if(AASTORE_BUILD_PYTHON AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME aastore_python_test
        COMMAND ${CMAKE_COMMAND} -E env
            "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python"
            ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_test.py)
endif()
add_executable(aastore_test tests/store_test.cpp)
target_link_libraries(aastore_test PRIVATE aastore)
add_executable(aastore_options_test tests/options_test.cpp)
target_link_libraries(aastore_options_test PRIVATE aastore)
add_test(NAME aastore_options_test COMMAND aastore_options_test)
set_tests_properties(aastore_options_test PROPERTIES LABELS "options")
set(_aastore_runtime_tests aastore_options_test aastore_codec_test aastore_codec_kernels_test)
foreach(_aastore_group IN ITEMS builder random sequential text concurrency)
    add_test(NAME aastore_store_${_aastore_group}
        COMMAND aastore_test --group ${_aastore_group})
    list(APPEND _aastore_runtime_tests aastore_store_${_aastore_group})
    set_tests_properties(aastore_store_${_aastore_group} PROPERTIES
        LABELS "store;${_aastore_group}")
endforeach()
unset(_aastore_group)
add_executable(aastore_codec_test tests/codec_test.cpp)
target_link_libraries(aastore_codec_test PRIVATE aastore)
add_test(NAME aastore_codec_test COMMAND aastore_codec_test)
# Include the private kernels in one independent test TU. Do not link aastore:
# the test already contains codec.cpp's public definitions, including with IPO.
add_executable(aastore_codec_kernels_test tests/codec_kernels_test.cpp)
target_compile_features(aastore_codec_kernels_test PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aastore_codec_kernels_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME aastore_codec_kernels_test COMMAND aastore_codec_kernels_test)
set_tests_properties(aastore_codec_kernels_test PROPERTIES LABELS "codec")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Compile the real backend against a private delayed-I/O shim, so old
    # kernel paths are tested even when the host disables io_uring.
    foreach(_aastore_uring_test IN ITEMS
            aastore_io_uring_compat_test aastore_io_uring_baseline_test)
        add_executable(${_aastore_uring_test}
            tests/io_uring_compat_test.cpp src/io_uring_backend.cpp)
        target_include_directories(${_aastore_uring_test} BEFORE PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/uring_compat")
        target_compile_definitions(${_aastore_uring_test} PRIVATE
            AASTORE_HAVE_IO_URING=1)
        target_link_libraries(${_aastore_uring_test} PRIVATE aastore)
        add_test(NAME ${_aastore_uring_test} COMMAND ${_aastore_uring_test})
        list(APPEND _aastore_runtime_tests ${_aastore_uring_test})
        set_tests_properties(${_aastore_uring_test} PROPERTIES
            LABELS "compatibility;io_uring")
    endforeach()
    unset(_aastore_uring_test)
    target_compile_definitions(aastore_io_uring_compat_test PRIVATE
        AASTORE_URING_HAS_READ=1 AASTORE_URING_HAS_PROBE=1)
    target_compile_definitions(aastore_io_uring_baseline_test PRIVATE
        AASTORE_TEST_LEGACY_LIBURING=1)
endif()
if(AASTORE_HAVE_IO_URING)
    target_compile_definitions(aastore_test PRIVATE AASTORE_TEST_IO_URING=1)
    add_executable(aastore_io_uring_test tests/io_uring_test.cpp)
    target_link_libraries(aastore_io_uring_test PRIVATE aastore)
    add_test(NAME aastore_io_uring_test COMMAND aastore_io_uring_test)
    list(APPEND _aastore_runtime_tests aastore_io_uring_test)
    set_tests_properties(aastore_io_uring_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND TARGET aastore_build AND TARGET aastore_bench)
    add_test(NAME aastore_benchmark_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_test.py
            $<TARGET_FILE:aastore_build> $<TARGET_FILE:aastore_bench>)
endif()
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross build: host Python and benchmark script tests are not registered")
    if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
        set_tests_properties(${_aastore_runtime_tests} PROPERTIES DISABLED TRUE)
        message(STATUS "Cross build without emulator: target runtime tests are disabled")
    endif()
endif()
unset(_aastore_runtime_tests)
include("${CMAKE_CURRENT_SOURCE_DIR}/tests/install_consumer/run.cmake")
set(_aastore_consumer_toolchain_cache
    "${CMAKE_CURRENT_BINARY_DIR}/install-consumer-toolchain.cmake")
aastore_write_consumer_toolchain_cache("${_aastore_consumer_toolchain_cache}")
add_test(NAME aastore_install_consumer
    COMMAND ${CMAKE_COMMAND}
        "-DAASTORE_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DAASTORE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DAASTORE_TEST_CONFIG=$<CONFIG>"
        "-DAASTORE_TEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DAASTORE_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DAASTORE_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DAASTORE_TEST_TOOLCHAIN_CACHE=${_aastore_consumer_toolchain_cache}"
        "-DAASTORE_TEST_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}"
        "-DAASTORE_TEST_CTEST=${CMAKE_CTEST_COMMAND}"
        "-DAASTORE_TEST_PACKAGE_DIR=${AASTORE_CMAKE_INSTALL_DIR}"
        "-DAASTORE_TEST_URING_INCLUDE_DIR=${AAStoreUring_INCLUDE_DIR}"
        "-DAASTORE_TEST_URING_LIBRARY=${AAStoreUring_LIBRARY}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_consumer/run.cmake")
set_tests_properties(aastore_install_consumer PROPERTIES LABELS packaging)
unset(_aastore_consumer_toolchain_cache)
if(NOT CMAKE_CROSSCOMPILING AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME aastore_cmake_uring_cross_probe
        COMMAND ${CMAKE_COMMAND}
            "-DAASTORE_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/uring-cross-probe-$<CONFIG>"
            "-DAASTORE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DAASTORE_TEST_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DAASTORE_TEST_GENERATOR=${CMAKE_GENERATOR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/uring_cross_probe.cmake")
    set_tests_properties(aastore_cmake_uring_cross_probe PROPERTIES
        LABELS "compatibility;cmake;io_uring")
    add_test(NAME aastore_cmake_cross_consumer
        COMMAND ${CMAKE_COMMAND}
            "-DAASTORE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DAASTORE_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/cross-consumer-$<CONFIG>"
            "-DAASTORE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DAASTORE_TEST_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DAASTORE_TEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DAASTORE_TEST_CTEST=${CMAKE_CTEST_COMMAND}"
            "-DAASTORE_TEST_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/cross_consumer.cmake")
    set_tests_properties(aastore_cmake_cross_consumer PROPERTIES
        LABELS "compatibility;cmake;packaging")
endif()
