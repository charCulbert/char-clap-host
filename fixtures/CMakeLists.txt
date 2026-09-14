# Fixture plug-ins.
#
# These are the char-wclap-examples sources built as native .clap bundles. Each
# one exercises a different corner of the specification, which is what makes
# them worth hosting: the host's own tests drive real plug-ins rather than
# mocks.

# add_fixture(<directory name> [RESOURCES])
#   Builds <directory>/Plugin.cpp and <directory>/Entry.cpp into
#   <build>/fixtures/<directory>.clap. With RESOURCES, the contents of
#   <directory>/resources are copied into the bundle, where the plug-in finds
#   them relative to the path clap_entry->init was given.
function(add_fixture name)
    cmake_parse_arguments(FIXTURE "RESOURCES" "" "" ${ARGN})
    set(target "fixture-${name}")
    add_library(${target} MODULE
        "${CMAKE_CURRENT_SOURCE_DIR}/${name}/Plugin.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/${name}/Entry.cpp"
    )
    target_link_libraries(${target} PRIVATE clap clap-helpers)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fixtures"
        CXX_VISIBILITY_PRESET hidden
    )
    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION clap
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.charlieculbert.nativeclaphost.fixture.${name}"
            MACOSX_BUNDLE_BUNDLE_NAME "${name}"
            MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        )
    else()
        # Everywhere else a .clap is just the shared object under that name.
        set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".clap")
    endif()
    if(FIXTURE_RESOURCES)
        if(APPLE)
            set(bundle_root "$<TARGET_BUNDLE_DIR:${target}>")
        else()
            set(bundle_root "${CMAKE_BINARY_DIR}/fixtures/${name}.clap.resources")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/${name}/resources" "${bundle_root}"
            COMMENT "Copying ${name} resources into its bundle")
    endif()
    set_property(GLOBAL APPEND PROPERTY NCH_FIXTURES ${target})
endfunction()

add_fixture(audio-ports)
add_fixture(audio-ports-activation)
add_fixture(effect)
add_fixture(gui-webview RESOURCES)
add_fixture(instrument)
add_fixture(latency)
add_fixture(log)
add_fixture(note-name)
add_fixture(note-ports)
add_fixture(param-indication)
add_fixture(parameters)
add_fixture(presets)
add_fixture(process-lifecycle)
add_fixture(remote-controls)
add_fixture(render)
add_fixture(state)
add_fixture(state-context)
add_fixture(tail)
add_fixture(thread-check)
add_fixture(transport)
add_fixture(voice-info)

get_property(nch_fixtures GLOBAL PROPERTY NCH_FIXTURES)
add_custom_target(fixtures DEPENDS ${nch_fixtures})
