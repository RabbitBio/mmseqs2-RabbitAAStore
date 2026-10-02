# 文件职责：配置 C++17 AAStore 库、命令行工具、基准程序和可选测试。
# 检测 liburing 与链接时优化支持，并保持非 SIMD 专用代码的可移植编译基线。
cmake_minimum_required(VERSION 3.16)

project(AABinaryStoreByteRange VERSION 0.1.0 LANGUAGES CXX)

option(AASTORE_BUILD_TESTS "Build AAStore tests" ON)
option(AASTORE_BUILD_PYTHON "Build the Python bindings" OFF)
option(AASTORE_ENABLE_IO_URING "Enable the Linux io_uring read backend" ON)
set(AASTORE_IPO_DEFAULT ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    set(AASTORE_IPO_DEFAULT OFF)
endif()
option(AASTORE_ENABLE_IPO "Enable interprocedural optimization when supported"
       ${AASTORE_IPO_DEFAULT})

if(AASTORE_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AASTORE_IPO_SUPPORTED OUTPUT AASTORE_IPO_ERROR)
endif()
find_package(Threads REQUIRED)

if(AASTORE_ENABLE_IO_URING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(AASTORE_LIBURING_INCLUDE_DIR liburing.h)
    find_library(AASTORE_LIBURING_LIBRARY uring)
    if(AASTORE_LIBURING_INCLUDE_DIR AND AASTORE_LIBURING_LIBRARY)
        set(AASTORE_HAVE_IO_URING TRUE)
    else()
        message(WARNING "liburing not found; using the pread backend")
    endif()
endif()

add_library(aastore
    src/builder.cpp
    src/builder_file.cpp
    src/builder_sequence_writer.cpp
    src/builder_text_writer.cpp
    src/byte_range_planner.cpp
    src/codec.cpp
    src/fasta_reader.cpp
    src/io_uring_backend.cpp
    src/metadata.cpp
    src/store_batch.cpp
    src/store.cpp
    src/store_io.cpp
    src/store_sequential.cpp
    src/text_column.cpp
)
target_compile_features(aastore PUBLIC cxx_std_17)
target_include_directories(aastore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aastore PUBLIC Threads::Threads)
if(AASTORE_HAVE_IO_URING)
    target_compile_definitions(aastore PRIVATE AASTORE_HAVE_IO_URING=1)
    target_include_directories(aastore PRIVATE ${AASTORE_LIBURING_INCLUDE_DIR})
    target_link_libraries(aastore PUBLIC ${AASTORE_LIBURING_LIBRARY})
endif()
# SIMD 内核自带目标指令集属性，并在运行时选择。
# 其他代码保持编译器默认的可移植 x86-64 指令集基线。
target_compile_options(aastore PRIVATE -O3 -Wall -Wextra -Wpedantic)
if(AASTORE_ENABLE_IPO AND AASTORE_IPO_SUPPORTED)
    set_property(TARGET aastore PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(AASTORE_BUILD_PYTHON)
    set(PYBIND11_FINDPYTHON ON)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 3.0 CONFIG REQUIRED)
    set_property(TARGET aastore PROPERTY POSITION_INDEPENDENT_CODE ON)
    pybind11_add_module(_aastore NO_EXTRAS python/bindings.cpp)
    target_link_libraries(_aastore PRIVATE aastore)
    target_compile_definitions(_aastore PRIVATE
        AASTORE_PYTHON_VERSION="${PROJECT_VERSION}")
    set_target_properties(_aastore PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/python/aastore")
    configure_file(python/aastore/__init__.py python/aastore/__init__.py COPYONLY)
    install(TARGETS _aastore LIBRARY DESTINATION aastore)
endif()

add_executable(aastore_bench benchmarks/read_benchmark.cpp)
target_link_libraries(aastore_bench PRIVATE aastore)

add_executable(aastore_build tools/build_store.cpp)
target_link_libraries(aastore_build PRIVATE aastore)

add_executable(aastore_sort_block tools/sort_block.cpp)
target_link_libraries(aastore_sort_block PRIVATE aastore)

add_executable(aastore_reorder_createdb tools/reorder_createdb.cpp)
target_include_directories(aastore_reorder_createdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(aastore_reorder_createdb PRIVATE aastore)

add_executable(linclust_candidate_bench
    tools/linclust_candidate_benchmark.cpp)
target_link_libraries(linclust_candidate_bench PRIVATE aastore)

add_executable(aastore_window_bench
    tools/window_read_benchmark.cpp)
target_link_libraries(aastore_window_bench PRIVATE aastore)

if(AASTORE_BUILD_TESTS)
    enable_testing()
    if(AASTORE_BUILD_PYTHON)
        add_test(NAME aastore_python_test
            COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python"
                ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_test.py)
    endif()
    add_executable(aastore_test tests/store_test.cpp)
    target_link_libraries(aastore_test PRIVATE aastore)
    if(AASTORE_HAVE_IO_URING)
        target_compile_definitions(aastore_test PRIVATE
            AASTORE_TEST_IO_URING=1)
        add_executable(aastore_io_uring_test tests/io_uring_test.cpp)
        target_link_libraries(aastore_io_uring_test PRIVATE aastore)
        add_test(NAME aastore_io_uring_test COMMAND aastore_io_uring_test)
        set_tests_properties(aastore_io_uring_test PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME aastore_test COMMAND aastore_test)
    add_executable(aastore_codec_test tests/codec_test.cpp)
    target_link_libraries(aastore_codec_test PRIVATE aastore)
    add_test(NAME aastore_codec_test COMMAND aastore_codec_test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME aastore_benchmark_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_test.py
                $<TARGET_FILE:aastore_build> $<TARGET_FILE:aastore_bench>)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(aastore PUBLIC stdc++fs)
endif()
