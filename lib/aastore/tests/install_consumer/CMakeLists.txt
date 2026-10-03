cmake_minimum_required(VERSION 3.16)
project(aastore_install_consumer LANGUAGES CXX)

find_package(aastore 0.1 CONFIG REQUIRED)

# A static SDK must also work inside a shared library. The SDK target must supply
# the C++ standard and include paths without this consumer setting them itself.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bridge.cpp" [[
#include <aastore/store.hpp>
#include <cstdint>

extern "C" uint64_t aastore_consumer_record_count(const char* path) {
    aastore::Store store(path);
    return store.size();
}
]])
add_library(consumer_bridge SHARED "${CMAKE_CURRENT_BINARY_DIR}/bridge.cpp")
target_link_libraries(consumer_bridge PRIVATE aastore::aastore)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE consumer_bridge aastore::aastore)

enable_testing()
add_test(NAME installed_sdk_roundtrip COMMAND consumer)
