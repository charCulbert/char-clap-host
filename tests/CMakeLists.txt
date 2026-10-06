# Unit tests for host internals. Plug-in-facing behaviour is covered by the
# script tests, which drive the real binary against the fixture plug-ins.
add_executable(nch-tests
    bundle.test.cpp
    json.test.cpp
    command.test.cpp
    device-settings.test.cpp
    event-list.test.cpp
    midi-file.test.cpp
    midi-player.test.cpp
    note-encoding.test.cpp
    plugin-instance.test.cpp
    process-buffers.test.cpp
    random.test.cpp
    session-commands.test.cpp
    thread-pool.test.cpp
    timeline.test.cpp
    validator.test.cpp
    wav.test.cpp
    main.cpp
)
target_link_libraries(nch-tests PRIVATE nch-core)
# The unit tests drive real fixture plug-ins rather than mocks, so they need
# them built and need to know where they landed.
add_dependencies(nch-tests fixtures)
target_compile_definitions(nch-tests PRIVATE NCH_FIXTURE_DIR="${CMAKE_BINARY_DIR}/fixtures"
                                            NCH_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME unit COMMAND nch-tests)

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    message(STATUS "Python 3 not found; script tests are skipped")
    return()
endif()

# Every tests/scripts/*.txt runs the real host and is compared to the .expected
# transcript beside it. Regenerate one with:
#   tests/run-script.py tests/scripts/<name>.txt --host build/clap-host \
#       --fixtures build/fixtures --accept
file(GLOB script_tests "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.txt")
foreach(script IN LISTS script_tests)
    get_filename_component(script_name "${script}" NAME_WE)
    add_test(NAME "script.${script_name}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-script.py" "${script}"
                --host "$<TARGET_FILE:clap-host>"
                --fixtures "${CMAKE_BINARY_DIR}/fixtures")
    set_tests_properties("script.${script_name}" PROPERTIES TIMEOUT 120)
endforeach()

# Exercise the one-shot contract with an actual terminal on stdin; ordinary
# CTest pipes would miss the interactive-mode regression.
if(UNIX)
    add_test(NAME cli.one-shot-tty
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/one-shot-tty.py"
                "$<TARGET_FILE:clap-host>" "${CMAKE_BINARY_DIR}/fixtures/instrument.clap")
    set_tests_properties(cli.one-shot-tty PROPERTIES TIMEOUT 45)
endif()

# Golden audio: render a known performance and compare it sample by sample.
add_test(NAME "audio.instrument"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/render-and-compare.py"
            --host "$<TARGET_FILE:clap-host>"
            --fixtures "${CMAKE_BINARY_DIR}/fixtures"
            --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden")
set_tests_properties("audio.instrument" PROPERTIES TIMEOUT 120)
