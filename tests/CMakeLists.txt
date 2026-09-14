# Unit tests for host internals. Plug-in-facing behaviour is covered by the
# script tests, which drive the real binary.
add_executable(nch-tests
    json.test.cpp
    command.test.cpp
    main.cpp
)
target_link_libraries(nch-tests PRIVATE nch-core)
add_test(NAME nch-tests COMMAND nch-tests)
