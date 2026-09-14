# Unit tests for host internals. Plug-in-facing behaviour is covered by the
# script tests, which drive the real binary against the fixture plug-ins.
add_executable(nch-tests
    json.test.cpp
    command.test.cpp
    midi-file.test.cpp
    wav.test.cpp
    main.cpp
)
target_link_libraries(nch-tests PRIVATE nch-core)
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

# Golden audio: render a known performance and compare it sample by sample.
add_test(NAME "audio.instrument"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/render-and-compare.py"
            --host "$<TARGET_FILE:clap-host>"
            --fixtures "${CMAKE_BINARY_DIR}/fixtures"
            --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden")
set_tests_properties("audio.instrument" PROPERTIES TIMEOUT 120)
