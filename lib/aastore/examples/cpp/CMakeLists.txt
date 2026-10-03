cmake_minimum_required(VERSION 3.16)
project(AAStoreExample LANGUAGES CXX)

find_package(aastore 0.1 CONFIG REQUIRED)

add_executable(aastore_example main.cpp)
target_link_libraries(aastore_example PRIVATE aastore::aastore)

enable_testing()
add_test(NAME aastore_example COMMAND aastore_example)
