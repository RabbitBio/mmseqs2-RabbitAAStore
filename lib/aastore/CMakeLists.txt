# Build and install the C++ SDK, optional tools, benchmarks, and Python bindings.
cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AASTORE_VERSION LIMIT_COUNT 1)
string(STRIP "${AASTORE_VERSION}" AASTORE_VERSION)
if(NOT AASTORE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a numeric major.minor.patch version")
endif()
project(AABinaryStore VERSION ${AASTORE_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(AASTORE_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AASTORE_IS_TOP_LEVEL ON)
    option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
endif()
option(AASTORE_BUILD_TESTS "Build AAStore tests" ${AASTORE_IS_TOP_LEVEL})
option(AASTORE_BUILD_TOOLS "Build AAStore command-line tools" ${AASTORE_IS_TOP_LEVEL})
option(AASTORE_BUILD_BENCHMARKS "Build AAStore benchmarks" ${AASTORE_IS_TOP_LEVEL})
option(AASTORE_BUILD_PYTHON "Build the Python bindings" OFF)
option(AASTORE_ENABLE_IO_URING "Enable the Linux io_uring read backend" ON)

set(AASTORE_IPO_DEFAULT ${AASTORE_IS_TOP_LEVEL})
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    set(AASTORE_IPO_DEFAULT OFF)
endif()
option(AASTORE_ENABLE_IPO "Enable interprocedural optimization when supported"
       ${AASTORE_IPO_DEFAULT})
if(AASTORE_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AASTORE_IPO_SUPPORTED OUTPUT AASTORE_IPO_ERROR
                        LANGUAGES CXX)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AAStoreThreads.cmake")
aastore_find_threads(REQUIRED)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AAStoreFilesystem.cmake")
aastore_find_filesystem_library(AASTORE_FILESYSTEM_LIBRARY)
set(AASTORE_HAVE_IO_URING OFF)
if(AASTORE_ENABLE_IO_URING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_aastore_module_path "${CMAKE_MODULE_PATH}")
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(AAStoreUring QUIET)
    set(CMAKE_MODULE_PATH "${_aastore_module_path}")
    unset(_aastore_module_path)
    if(AAStoreUring_FOUND)
        set(AASTORE_HAVE_IO_URING ON)
    else()
        message(WARNING "usable liburing not found; using the pread backend")
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
    src/options.cpp
    src/store_batch.cpp
    src/store.cpp
    src/store_io.cpp
    src/store_sequential.cpp
    src/text_column.cpp
)
add_library(aastore::aastore ALIAS aastore)
target_compile_features(aastore PUBLIC cxx_std_17)
target_include_directories(aastore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aastore PUBLIC Threads::Threads)
set_target_properties(aastore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
)
if(PROJECT_VERSION_MAJOR EQUAL 0)
    # Each pre-1.0 minor may change the ABI; patches preserve it.
    set_target_properties(aastore PROPERTIES
        SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    set(AASTORE_VERSION_COMPATIBILITY SameMinorVersion)
else()
    set_target_properties(aastore PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})
    set(AASTORE_VERSION_COMPATIBILITY SameMajorVersion)
endif()
if(AASTORE_HAVE_IO_URING)
    target_compile_definitions(aastore PRIVATE AASTORE_HAVE_IO_URING=1)
    if(AAStoreUring_HAS_READ)
        target_compile_definitions(aastore PRIVATE AASTORE_URING_HAS_READ=1)
    endif()
    if(AAStoreUring_HAS_PROBE)
        target_compile_definitions(aastore PRIVATE AASTORE_URING_HAS_PROBE=1)
    endif()
    # The installed static target references a dependency name, not the build
    # host's absolute liburing library/include paths. Consumers rediscover it.
    target_link_libraries(aastore PRIVATE AAStoreUring::AAStoreUring)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aastore PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(AASTORE_ENABLE_IPO AND AASTORE_IPO_SUPPORTED)
    set_property(TARGET aastore PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

set(AASTORE_PKGCONFIG_PRIVATE_LIBS "${CMAKE_THREAD_LIBS_INIT}")
set(AASTORE_PKGCONFIG_PUBLIC_LIBS "")
if(AASTORE_HAVE_IO_URING)
    string(APPEND AASTORE_PKGCONFIG_PRIVATE_LIBS " -luring")
endif()
if(AASTORE_FILESYSTEM_LIBRARY)
    target_link_libraries(aastore PUBLIC ${AASTORE_FILESYSTEM_LIBRARY})
    set(AASTORE_PKGCONFIG_PUBLIC_LIBS "-l${AASTORE_FILESYSTEM_LIBRARY}")
endif()
string(STRIP "${AASTORE_PKGCONFIG_PRIVATE_LIBS}" AASTORE_PKGCONFIG_PRIVATE_LIBS)

set(AASTORE_PACKAGE_NEEDS_URING OFF)
set(AASTORE_PACKAGE_NEEDS_URING_PROBE OFF)
set(AASTORE_PACKAGE_URING_ABI "")
get_target_property(_aastore_library_type aastore TYPE)
if(AASTORE_HAVE_IO_URING AND _aastore_library_type STREQUAL "STATIC_LIBRARY")
    set(AASTORE_PACKAGE_NEEDS_URING ON)
    set(AASTORE_PACKAGE_URING_ABI "${AAStoreUring_ABI}")
    if(AAStoreUring_HAS_PROBE)
        set(AASTORE_PACKAGE_NEEDS_URING_PROBE ON)
    endif()
endif()
unset(_aastore_library_type)

install(TARGETS aastore EXPORT aastore-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Development
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/aastore
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Development
    FILES_MATCHING PATTERN "*.hpp"
)
install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/aastore"
    COMPONENT Development
)
set(AASTORE_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/aastore")
configure_package_config_file(
    cmake/aastore-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config.cmake"
    INSTALL_DESTINATION "${AASTORE_CMAKE_INSTALL_DIR}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ${AASTORE_VERSION_COMPATIBILITY}
)
install(EXPORT aastore-targets
    FILE aastore-targets.cmake
    NAMESPACE aastore::
    DESTINATION "${AASTORE_CMAKE_INSTALL_DIR}"
    COMPONENT Development
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/aastore-config-version.cmake"
    cmake/AAStoreThreads.cmake
    cmake/FindAAStoreUring.cmake
    DESTINATION "${AASTORE_CMAKE_INSTALL_DIR}"
    COMPONENT Development
)
# Resolve prefix from pcfiledir so the SDK may be moved as a unit, including
# GNUInstallDirs configurations using lib64 or a multiarch lib directory.
file(RELATIVE_PATH AASTORE_PKGCONFIG_PREFIX_RELATIVE
    "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
file(RELATIVE_PATH AASTORE_PKGCONFIG_LIBDIR_RELATIVE
    "${CMAKE_INSTALL_PREFIX}" "${CMAKE_INSTALL_FULL_LIBDIR}")
file(RELATIVE_PATH AASTORE_PKGCONFIG_INCLUDEDIR_RELATIVE
    "${CMAKE_INSTALL_PREFIX}" "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
configure_file(cmake/aastore.pc.in "${CMAKE_CURRENT_BINARY_DIR}/aastore.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/aastore.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT Development
)

if(AASTORE_BUILD_PYTHON)
    set(PYBIND11_FINDPYTHON ON)
    # FindPython split Development.Module from Development in CMake 3.18.
    # Match pybind11's fallback so direct builds work with our 3.16 minimum.
    if(CMAKE_VERSION VERSION_LESS 3.18)
        find_package(Python 3.9 COMPONENTS Interpreter REQUIRED)
        # Old FindPython uses distutils, absent from Python 3.12+. In a venv,
        # point its development-file search at the interpreter's base install.
        if(NOT DEFINED Python_ROOT_DIR AND NOT DEFINED ENV{Python_ROOT_DIR}
           AND NOT CMAKE_CROSSCOMPILING)
            execute_process(
                COMMAND "${Python_EXECUTABLE}" -c "import sys; print(sys.base_prefix)"
                OUTPUT_VARIABLE _aastore_python_root
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _aastore_python_root_result
                ERROR_QUIET)
            if(_aastore_python_root_result EQUAL 0)
                set(Python_ROOT_DIR "${_aastore_python_root}")
            endif()
            unset(_aastore_python_root)
            unset(_aastore_python_root_result)
        endif()
        find_package(Python 3.9 COMPONENTS Interpreter Development REQUIRED)
    else()
        find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
    endif()
    find_package(pybind11 3.0 CONFIG REQUIRED)
    pybind11_add_module(_aastore NO_EXTRAS python/bindings.cpp)
    target_link_libraries(_aastore PRIVATE aastore)
    target_compile_definitions(_aastore PRIVATE
        AASTORE_PYTHON_VERSION="${PROJECT_VERSION}")
    set_target_properties(_aastore PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/python/aastore")
    configure_file(python/aastore/__init__.py python/aastore/__init__.py COPYONLY)
    install(TARGETS _aastore
        LIBRARY DESTINATION aastore COMPONENT Python
        RUNTIME DESTINATION aastore COMPONENT Python)
endif()

if(AASTORE_BUILD_TOOLS)
    add_executable(aastore_build tools/build_store.cpp)
    target_link_libraries(aastore_build PRIVATE aastore)
    add_executable(aastore_sort_block tools/sort_block.cpp)
    target_link_libraries(aastore_sort_block PRIVATE aastore)
    add_executable(aastore_reorder_createdb tools/reorder_createdb.cpp)
    target_include_directories(aastore_reorder_createdb PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aastore_reorder_createdb PRIVATE aastore)
    file(RELATIVE_PATH _aastore_tools_libdir
        "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(aastore_build aastore_sort_block aastore_reorder_createdb
            PROPERTIES INSTALL_RPATH "$ORIGIN/${_aastore_tools_libdir}")
    endif()
    unset(_aastore_tools_libdir)
    install(TARGETS aastore_build aastore_sort_block aastore_reorder_createdb
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)
endif()

if(AASTORE_BUILD_BENCHMARKS)
    add_executable(aastore_bench benchmarks/read_benchmark.cpp)
    target_link_libraries(aastore_bench PRIVATE aastore)
    add_executable(linclust_candidate_bench tools/linclust_candidate_benchmark.cpp)
    target_link_libraries(linclust_candidate_bench PRIVATE aastore)
    add_executable(aastore_window_bench tools/window_read_benchmark.cpp)
    target_link_libraries(aastore_window_bench PRIVATE aastore)
endif()

if(AASTORE_BUILD_TESTS)
    enable_testing()
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
    set(_aastore_runtime_tests aastore_options_test aastore_codec_test)
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
                "-DAASTORE_TEST_CONFIG=$<CONFIG>"
                "-DAASTORE_TEST_GENERATOR=${CMAKE_GENERATOR}"
                "-DAASTORE_TEST_CTEST=${CMAKE_CTEST_COMMAND}"
                "-DAASTORE_TEST_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
                "-DAASTORE_TEST_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/cross_consumer.cmake")
        set_tests_properties(aastore_cmake_cross_consumer PROPERTIES
            LABELS "compatibility;cmake;packaging")
    endif()
endif()
