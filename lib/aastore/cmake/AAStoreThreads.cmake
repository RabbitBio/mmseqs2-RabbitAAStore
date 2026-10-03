# FindThreads must resolve pthread symbols even when other toolchain checks
# deliberately build only static archives. Do not change the caller's policy.
function(aastore_find_threads)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    # Discard successes cached by earlier compile-only FindThreads checks.
    foreach(check CMAKE_HAVE_LIBC_PTHREAD CMAKE_HAVE_PTHREADS_CREATE
            CMAKE_HAVE_PTHREAD_CREATE THREADS_HAVE_PTHREAD_ARG)
        unset(${check})
        unset(${check} CACHE)
    endforeach()
    find_package(Threads ${ARGN})
    foreach(variable Threads_FOUND CMAKE_THREAD_LIBS_INIT
            CMAKE_USE_PTHREADS_INIT CMAKE_USE_WIN32_THREADS_INIT
            CMAKE_HP_PTHREADS_INIT)
        set(${variable} "${${variable}}" PARENT_SCOPE)
    endforeach()
endfunction()
