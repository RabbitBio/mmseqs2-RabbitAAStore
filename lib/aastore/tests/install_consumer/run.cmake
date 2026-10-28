cmake_minimum_required(VERSION 3.16)

# Keep toolchain lists in a cache file so semicolons and spaces survive intact.
function(aastore_test_cache_set path name value)
    set(delimiter "=")
    string(FIND "${value}" "]${delimiter}]" closing)
    while(NOT closing EQUAL -1)
        string(APPEND delimiter "=")
        string(FIND "${value}" "]${delimiter}]" closing)
    endwhile()
    file(APPEND "${path}"
        "set(${name} [${delimiter}[${value}]${delimiter}] CACHE STRING \"RabbitAAStore SDK consumer test\" FORCE)\n")
endfunction()

function(aastore_write_consumer_toolchain_cache path)
    file(WRITE "${path}" "# Toolchain settings for the relocated SDK test only.\n")
    set(variables
        CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM
        CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_ARG1
        CMAKE_CXX_COMPILER_TARGET CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
        CMAKE_CXX_COMPILER_LAUNCHER CMAKE_AR CMAKE_RANLIB CMAKE_LINKER
        CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK
        CMAKE_TRY_COMPILE_TARGET_TYPE CMAKE_CROSSCOMPILING_EMULATOR
        CMAKE_CXX_STANDARD CMAKE_CXX_STANDARD_REQUIRED CMAKE_CXX_EXTENSIONS
        CMAKE_CONFIGURATION_TYPES CMAKE_PREFIX_PATH CMAKE_FIND_ROOT_PATH
        CMAKE_FIND_ROOT_PATH_MODE_PROGRAM CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
        CMAKE_FIND_ROOT_PATH_MODE_INCLUDE CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
        CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS)
    # Setting SYSTEM_NAME on a native consumer would turn it into a cross build.
    if(CMAKE_CROSSCOMPILING)
        list(APPEND variables
            CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR)
    endif()
    set(configurations DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    list(APPEND configurations ${CMAKE_CONFIGURATION_TYPES} "${CMAKE_BUILD_TYPE}")
    list(REMOVE_DUPLICATES configurations)
    foreach(configuration IN LISTS configurations)
        if(NOT "${configuration}" STREQUAL "")
            string(TOUPPER "${configuration}" configuration)
            foreach(kind CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
                list(APPEND variables "CMAKE_${kind}_FLAGS_${configuration}")
            endforeach()
        endif()
    endforeach()
    foreach(variable IN LISTS variables)
        if(DEFINED ${variable})
            set(value "${${variable}}")
            if(variable STREQUAL "CMAKE_TOOLCHAIN_FILE" AND NOT "${value}" STREQUAL "")
                # Match CMake's relative-toolchain search order before moving
                # to the consumer's independent source and build directories.
                if(NOT IS_ABSOLUTE "${value}")
                    if(EXISTS "${CMAKE_BINARY_DIR}/${value}")
                        set(value "${CMAKE_BINARY_DIR}/${value}")
                    else()
                        set(value "${CMAKE_SOURCE_DIR}/${value}")
                    endif()
                endif()
                get_filename_component(value "${value}" ABSOLUTE)
                if(NOT EXISTS "${value}")
                    message(FATAL_ERROR "SDK test toolchain file does not exist: ${value}")
                endif()
            endif()
            aastore_test_cache_set("${path}" "${variable}" "${value}")
        endif()
    endforeach()
endfunction()

# Configuration includes this file for the helpers; CTest executes the driver.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/test_helpers.cmake")

foreach(required AASTORE_BUILD_DIR AASTORE_SOURCE_DIR AASTORE_TEST_TOOLCHAIN_CACHE
                 AASTORE_TEST_GENERATOR AASTORE_TEST_CTEST AASTORE_TEST_PACKAGE_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()

if(NOT EXISTS "${AASTORE_TEST_TOOLCHAIN_CACHE}")
    message(FATAL_ERROR "Missing SDK test toolchain cache: ${AASTORE_TEST_TOOLCHAIN_CACHE}")
endif()
include("${AASTORE_TEST_TOOLCHAIN_CACHE}")

set(test_root "${AASTORE_BUILD_DIR}/install-consumer-${AASTORE_TEST_CONFIG}")
set(stage "${test_root}/stage")
set(relocated "${test_root}/relocated-sdk")
set(consumer_build "${test_root}/consumer-build")
set(consumer_cache "${test_root}/consumer-cache.cmake")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")
configure_file("${AASTORE_TEST_TOOLCHAIN_CACHE}" "${consumer_cache}" COPYONLY)
set(consumer_prefix_path "${relocated}")
list(APPEND consumer_prefix_path ${CMAKE_PREFIX_PATH})
aastore_test_cache_set("${consumer_cache}" CMAKE_PREFIX_PATH "${consumer_prefix_path}")
aastore_test_cache_set("${consumer_cache}" aastore_DIR
    "${relocated}/${AASTORE_TEST_PACKAGE_DIR}")
aastore_test_cache_set("${consumer_cache}" CMAKE_BUILD_TYPE "${AASTORE_TEST_CONFIG}")

set(config_args)
if(NOT "${AASTORE_TEST_CONFIG}" STREQUAL "")
    list(APPEND config_args --config "${AASTORE_TEST_CONFIG}")
endif()
aastore_test_run_checked("${CMAKE_COMMAND}" --install "${AASTORE_BUILD_DIR}"
    --prefix "${stage}" --component Development ${config_args})
file(RENAME "${stage}" "${relocated}")

# Exported metadata must not retain source/build paths or the original prefix.
# System dependencies are discovered when the external consumer configures.
file(GLOB_RECURSE metadata "${relocated}/*.cmake" "${relocated}/*.pc")
foreach(path IN LISTS metadata)
    file(READ "${path}" contents)
    foreach(forbidden "${AASTORE_SOURCE_DIR}" "${AASTORE_BUILD_DIR}" "${stage}")
        string(FIND "${contents}" "${forbidden}" found)
        if(NOT found EQUAL -1)
            message(FATAL_ERROR "${path} retains non-relocatable path ${forbidden}")
        endif()
    endforeach()
endforeach()

set(generator_args -G "${AASTORE_TEST_GENERATOR}")
if(NOT "${AASTORE_TEST_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND generator_args -A "${AASTORE_TEST_GENERATOR_PLATFORM}")
endif()
if(NOT "${AASTORE_TEST_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND generator_args -T "${AASTORE_TEST_GENERATOR_TOOLSET}")
endif()
# Exercise the SDK with its selected dependency, including custom old liburing
# builds. These test-only hints are never embedded in installed SDK metadata.
if(DEFINED AASTORE_TEST_URING_INCLUDE_DIR
   AND NOT "${AASTORE_TEST_URING_INCLUDE_DIR}" STREQUAL ""
   AND DEFINED AASTORE_TEST_URING_LIBRARY
   AND NOT "${AASTORE_TEST_URING_LIBRARY}" STREQUAL "")
    aastore_test_cache_set("${consumer_cache}" AAStoreUring_INCLUDE_DIR
        "${AASTORE_TEST_URING_INCLUDE_DIR}")
    aastore_test_cache_set("${consumer_cache}" AAStoreUring_LIBRARY
        "${AASTORE_TEST_URING_LIBRARY}")
endif()
aastore_test_run_checked("${CMAKE_COMMAND}"
    -C "${consumer_cache}"
    -S "${AASTORE_SOURCE_DIR}/tests/install_consumer" -B "${consumer_build}"
    ${generator_args})
aastore_test_run_checked("${CMAKE_COMMAND}" --build "${consumer_build}" ${config_args}
    --parallel 2)

if(AASTORE_TEST_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    message(STATUS
        "Relocated installed SDK consumer configure/build passed; runtime test skipped (no cross-compiling emulator)")
    return()
endif()

set(ctest_config_args)
if(NOT "${AASTORE_TEST_CONFIG}" STREQUAL "")
    list(APPEND ctest_config_args -C "${AASTORE_TEST_CONFIG}")
endif()
execute_process(COMMAND "${AASTORE_TEST_CTEST}" ${ctest_config_args}
    --output-on-failure WORKING_DIRECTORY "${consumer_build}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed consumer failed\n${output}\n${error}")
endif()
message(STATUS "Relocated installed SDK consumer passed")
