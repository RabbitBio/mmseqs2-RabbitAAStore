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
option(AASTORE_BUILD_TESTS "Build the full AAStore regression tests" OFF)
option(AASTORE_BUILD_TOOLS "Build AAStore command-line tools" ${AASTORE_IS_TOP_LEVEL})
option(AASTORE_BUILD_BENCHMARKS "Build AAStore benchmarks" OFF)
option(AASTORE_BUILD_PYTHON "Build the Python bindings" OFF)
option(AASTORE_ENABLE_IO_URING "Enable the Linux io_uring read backend" ON)
set(AASTORE_SMOKE_DEFAULT ${AASTORE_IS_TOP_LEVEL})
if(CMAKE_CROSSCOMPILING)
    set(AASTORE_SMOKE_DEFAULT OFF)
endif()
option(AASTORE_RUN_SMOKE_TESTS "Run small environment checks as part of the default build"
       ${AASTORE_SMOKE_DEFAULT})

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
    file(RELATIVE_PATH _aastore_tools_libdir
        "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(aastore_build
            PROPERTIES INSTALL_RPATH "$ORIGIN/${_aastore_tools_libdir}")
    endif()
    unset(_aastore_tools_libdir)
    install(TARGETS aastore_build
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)
endif()

if(AASTORE_BUILD_BENCHMARKS)
    add_executable(aastore_bench benchmarks/read_benchmark.cpp)
    target_link_libraries(aastore_bench PRIVATE aastore)
endif()

if(AASTORE_BUILD_TESTS OR AASTORE_RUN_SMOKE_TESTS)
    enable_testing()
    include("${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.cmake")
endif()

if(AASTORE_BUILD_TESTS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/tests/regression.cmake")
endif()
