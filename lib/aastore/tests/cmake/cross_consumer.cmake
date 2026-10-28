cmake_minimum_required(VERSION 3.16)

include("${CMAKE_CURRENT_LIST_DIR}/test_helpers.cmake")

foreach(required AASTORE_SOURCE_DIR AASTORE_TEST_BINARY_DIR AASTORE_TEST_CXX_COMPILER
                 AASTORE_TEST_GENERATOR AASTORE_TEST_CTEST AASTORE_TEST_SYSTEM_NAME)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()

# A same-architecture cross build exercises both branches without requiring a
# second compiler. Toolchain and emulator paths intentionally contain spaces.
set(emulator_template [====[
cmake_minimum_required(VERSION 3.16)

if(NOT AASTORE_EMULATOR_SENTINEL STREQUAL "argument with spaces")
    message(FATAL_ERROR "Emulator arguments were not preserved")
endif()
set(command)
set(after_separator OFF)
math(EXPR last_argument "${CMAKE_ARGC} - 1")
foreach(index RANGE 0 ${last_argument})
    if(after_separator)
        list(APPEND command "${CMAKE_ARGV${index}}")
    elseif(CMAKE_ARGV${index} STREQUAL "--")
        set(after_separator ON)
    endif()
endforeach()
# The fixture sysroot '/' makes old CMake strip '/' from build RPATHs.
list(GET command 0 executable)
get_filename_component(library_dir "${executable}" DIRECTORY)
set(ENV{LD_LIBRARY_PATH} "${library_dir}:$ENV{LD_LIBRARY_PATH}")
execute_process(COMMAND ${command} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Emulated SDK consumer failed: ${result}")
endif()
file(APPEND "${AASTORE_EMULATOR_MARKER}"
    "Executed ${command}; sentinel=${AASTORE_EMULATOR_SENTINEL}\n")
]====])
set(toolchain_template [====[
set(CMAKE_SYSTEM_NAME [==[@AASTORE_TEST_SYSTEM_NAME@]==])
set(CMAKE_CXX_COMPILER [==[@AASTORE_TEST_CXX_COMPILER@]==])
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_SYSROOT [==[@fixture_sysroot@]==])
if(CMAKE_SOURCE_DIR STREQUAL [==[@AASTORE_SOURCE_DIR@/tests/install_consumer]==])
    file(APPEND [==[@toolchain_marker@]==] "Consumer toolchain loaded\n")
else()
    # The consumer must inherit these settings from the producer's cache.
    set(CMAKE_CXX_COMPILER_ARG1 "-DAASTORE_CROSS_COMPILER_ARGUMENT=1")
    set(CMAKE_CXX_FLAGS [==[@AASTORE_TEST_CXX_FLAGS@ -DAASTORE_CROSS_TOOLCHAIN=1 -include "@case_dir@/runtime marker.hpp"]==])
    set(CMAKE_CROSSCOMPILING_EMULATOR [==[@fixture_emulator@]==])
endif()
# CMake 3.20 derives ARG1 from the compiler list during compiler detection.
# The consumer uses only the argument inherited from the producer's cache.
if(CMAKE_CXX_COMPILER_ARG1)
    list(APPEND CMAKE_CXX_COMPILER "${CMAKE_CXX_COMPILER_ARG1}")
endif()
]====])
set(runtime_marker_template [====[
#include <cstdio>

#ifndef AASTORE_CROSS_TOOLCHAIN
#error "SDK consumer lost the toolchain compiler flags"
#endif
#ifndef AASTORE_CROSS_COMPILER_ARGUMENT
#error "SDK consumer lost the toolchain compiler argument"
#endif

namespace {
struct AAStoreCrossConsumerRuntimeMarker {
    AAStoreCrossConsumerRuntimeMarker() {
        if (std::FILE* stream = std::fopen("@marker_cxx_path@", "a")) {
            std::fputs("Target executable was actually started\n", stream);
            std::fclose(stream);
        }
    }
};
const AAStoreCrossConsumerRuntimeMarker aastore_cross_consumer_runtime_marker;
}  // namespace
]====])

file(REMOVE_RECURSE "${AASTORE_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${AASTORE_TEST_BINARY_DIR}")
set(emulator_file "${AASTORE_TEST_BINARY_DIR}/emulator fixture.cmake")
file(WRITE "${emulator_file}" "${emulator_template}")
foreach(mode IN ITEMS without-emulator with-emulator)
    set(case_dir "${AASTORE_TEST_BINARY_DIR}/${mode}")
    set(producer_build "${case_dir}/producer-build")
    set(toolchain_file "${producer_build}/toolchains/cross toolchain.cmake")
    set(runtime_marker "${case_dir}/consumer-executed.txt")
    set(emulator_marker "${case_dir}/emulator-executed.txt")
    set(toolchain_marker "${case_dir}/consumer-toolchain-loaded.txt")
    file(MAKE_DIRECTORY "${producer_build}/toolchains")
    set(fixture_sysroot "${AASTORE_TEST_SYSROOT}")
    if(AASTORE_TEST_SYSTEM_NAME STREQUAL "Linux" AND "${fixture_sysroot}" STREQUAL "")
        set(fixture_sysroot /)
    endif()
    set(fixture_emulator "")
    if(mode STREQUAL "with-emulator")
        set(fixture_emulator "${CMAKE_COMMAND};-DAASTORE_EMULATOR_MARKER=${emulator_marker};-DAASTORE_EMULATOR_SENTINEL=argument with spaces;-P;${emulator_file};--")
    endif()
    string(CONFIGURE "${toolchain_template}" toolchain_contents @ONLY)
    file(WRITE "${toolchain_file}" "${toolchain_contents}")
    set(marker_cxx_path "${runtime_marker}")
    string(REPLACE "\\" "\\\\" marker_cxx_path "${marker_cxx_path}")
    string(REPLACE "\"" "\\\"" marker_cxx_path "${marker_cxx_path}")
    string(CONFIGURE "${runtime_marker_template}" runtime_marker_contents @ONLY)
    file(WRITE "${case_dir}/runtime marker.hpp" "${runtime_marker_contents}")
    set(generator_args -G "${AASTORE_TEST_GENERATOR}")
    if(NOT "${AASTORE_TEST_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND generator_args -A "${AASTORE_TEST_GENERATOR_PLATFORM}")
    endif()
    if(NOT "${AASTORE_TEST_GENERATOR_TOOLSET}" STREQUAL "")
        list(APPEND generator_args -T "${AASTORE_TEST_GENERATOR_TOOLSET}")
    endif()
    aastore_test_run_checked("${CMAKE_COMMAND}" -S "${AASTORE_SOURCE_DIR}" -B "${producer_build}"
        ${generator_args} "-DCMAKE_TOOLCHAIN_FILE=toolchains/cross toolchain.cmake"
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DAASTORE_BUILD_TESTS=ON
        -DAASTORE_BUILD_TOOLS=OFF -DAASTORE_BUILD_BENCHMARKS=OFF -DAASTORE_BUILD_PYTHON=OFF
        -DAASTORE_ENABLE_IO_URING=OFF -DAASTORE_ENABLE_IPO=OFF)
    aastore_test_run_checked("${CMAKE_COMMAND}" --build "${producer_build}"
        --config Release --target aastore --parallel 2)
    execute_process(COMMAND "${AASTORE_TEST_CTEST}" -C Release
        -R "^aastore_install_consumer$" --output-on-failure
        WORKING_DIRECTORY "${producer_build}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${mode} SDK test failed\n${output}\n${error}")
    endif()
    if(NOT EXISTS "${toolchain_marker}")
        message(FATAL_ERROR "Consumer did not load the selected toolchain")
    endif()
    if(mode STREQUAL "with-emulator")
        if(NOT EXISTS "${emulator_marker}" OR NOT EXISTS "${runtime_marker}")
            message(FATAL_ERROR "Consumer roundtrip did not run through the emulator")
        endif()
    else()
        if(EXISTS "${emulator_marker}" OR EXISTS "${runtime_marker}")
            message(FATAL_ERROR "Consumer was executed without an emulator")
        endif()
        file(READ "${producer_build}/Testing/Temporary/LastTest.log" install_output)
        if(NOT install_output MATCHES "configure/build passed; runtime test skipped \\(no cross-compiling emulator\\)")
            message(FATAL_ERROR "Missing explicit cross-consumer runtime skip status")
        endif()
    endif()
endforeach()
message(STATUS "Cross SDK consumer configure/build and emulator handling passed")
