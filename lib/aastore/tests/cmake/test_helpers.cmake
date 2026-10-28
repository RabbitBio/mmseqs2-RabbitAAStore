include_guard(GLOBAL)

# Run a test setup command and retain both output streams on failure.
function(aastore_test_run_checked)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        string(JOIN " " command ${ARGV})
        message(FATAL_ERROR "Command failed (${result}): ${command}\n${output}\n${error}")
    endif()
endfunction()
