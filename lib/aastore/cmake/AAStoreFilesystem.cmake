# Detect the selected standard library's filesystem link requirements.
# Compile and link a probe without running it, including when cross-compiling.
function(aastore_find_filesystem_library output_variable)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    if(NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 17)
        set(CMAKE_CXX_STANDARD 17)
    endif()
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    # A toolchain can use STATIC_LIBRARY for other checks. This probe must link
    # an executable to detect filesystem symbols absent from the main library.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(_aastore_filesystem_source [[
        #include <filesystem>
        int main() {
            const auto path = std::filesystem::current_path();
            return std::filesystem::exists(path) ? 0 : 1;
        }
    ]])

    # Recheck on configure: flags, sysroots, or installed standard libraries can
    # change without changing the compiler executable or version.
    unset(AASTORE_FILESYSTEM_IN_STDLIB CACHE)
    check_cxx_source_compiles("${_aastore_filesystem_source}"
        AASTORE_FILESYSTEM_IN_STDLIB)
    set(_aastore_filesystem_library "")
    if(NOT AASTORE_FILESYSTEM_IN_STDLIB)
        set(CMAKE_REQUIRED_LIBRARIES stdc++fs)
        unset(AASTORE_FILESYSTEM_WITH_STDCXXFS CACHE)
        check_cxx_source_compiles("${_aastore_filesystem_source}"
            AASTORE_FILESYSTEM_WITH_STDCXXFS)
        if(AASTORE_FILESYSTEM_WITH_STDCXXFS)
            set(_aastore_filesystem_library stdc++fs)
        else()
            set(CMAKE_REQUIRED_LIBRARIES c++fs)
            unset(AASTORE_FILESYSTEM_WITH_CXXFS CACHE)
            check_cxx_source_compiles("${_aastore_filesystem_source}"
                AASTORE_FILESYSTEM_WITH_CXXFS)
            if(AASTORE_FILESYSTEM_WITH_CXXFS)
                set(_aastore_filesystem_library c++fs)
            else()
                message(FATAL_ERROR
                    "The selected C++ toolchain cannot compile and link std::filesystem "
                    "with C++17 or newer (tried the standard library, stdc++fs and c++fs). "
                    "See the CMake configure logs for the compiler diagnostics.")
            endif()
        endif()
    endif()
    cmake_pop_check_state()
    set(${output_variable} "${_aastore_filesystem_library}" PARENT_SCOPE)
endfunction()
