# Register small, self-contained runtime checks; no downloaded data is needed.
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    if(AASTORE_RUN_SMOKE_TESTS)
        message(FATAL_ERROR
            "AASTORE_RUN_SMOKE_TESTS requires CMAKE_CROSSCOMPILING_EMULATOR for a cross build")
    endif()
    message(STATUS "Cross build without emulator: smoke tests are not registered")
    return()
endif()

add_executable(aastore_smoke_test "${CMAKE_CURRENT_LIST_DIR}/smoke_test.cpp")
target_link_libraries(aastore_smoke_test PRIVATE aastore)
set(_aastore_smoke_dependencies aastore_smoke_test)
foreach(_aastore_case IN ITEMS pread memory auto io_uring)
    add_test(NAME aastore_smoke_${_aastore_case}
        COMMAND aastore_smoke_test ${_aastore_case})
    set_tests_properties(aastore_smoke_${_aastore_case} PROPERTIES
        LABELS "smoke" TIMEOUT 60)
endforeach()
set_tests_properties(aastore_smoke_io_uring PROPERTIES SKIP_RETURN_CODE 77)
unset(_aastore_case)

if(AASTORE_BUILD_PYTHON AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME aastore_smoke_python
        COMMAND ${CMAKE_COMMAND} -E env
            "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python"
            ${Python_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/smoke_test.py")
    set_tests_properties(aastore_smoke_python PROPERTIES LABELS "smoke" TIMEOUT 60)
    list(APPEND _aastore_smoke_dependencies _aastore)
endif()

if(AASTORE_RUN_SMOKE_TESTS)
    # Run on each default build, including incremental builds after a kernel or
    # runtime environment change. A failed check propagates to the build result.
    add_custom_target(aastore_smoke ALL
        COMMAND ${CMAKE_CTEST_COMMAND} -C "$<CONFIG>"
            --verbose --output-on-failure -R "^aastore_smoke_"
        DEPENDS ${_aastore_smoke_dependencies}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Running AAStore environment smoke tests"
        VERBATIM USES_TERMINAL)
endif()
unset(_aastore_smoke_dependencies)
