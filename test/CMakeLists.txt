if(NOT WITH_TESTS AND NOT WITH_PERFORMANCE_TESTS)
    return()
endif()


message("Building tests")


# libdnf
add_subdirectory(libdnf)

# performance tests are available only for libdnf
if(WITH_PERFORMANCE_TESTS)
    return()
endif()

# libdnf-cli
add_subdirectory(libdnf-cli)

# bindings
add_subdirectory(go)
add_subdirectory(perl5)
add_subdirectory(python3)
add_subdirectory(ruby)

# components
add_subdirectory(dnfdaemon-server)
