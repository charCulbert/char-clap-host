cmake_minimum_required(VERSION 3.21)
project(nativeClapHost VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)  # the Cocoa window layer
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# So editors and clangd see the real compile flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/clap" CACHE PATH "CLAP SDK")
if(NOT EXISTS "${CLAP_SDK_ROOT}/include/clap/clap.h")
    message(FATAL_ERROR "The CLAP SDK is missing. Run: git submodule update --init --recursive")
endif()
set(CLAP_HELPERS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/clap-helpers" CACHE PATH "clap-helpers")
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)
add_subdirectory("${CLAP_HELPERS_ROOT}" clap-helpers EXCLUDE_FROM_ALL)

# RtAudio and RtMidi cover every target platform's device APIs, which is the
# same reason clap-wrapper's standalone uses them.
# Both libraries name their uninstall target "uninstall"; renaming keeps the
# second add_subdirectory from colliding with the first.
set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio-uninstall" CACHE STRING "" FORCE)
set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi-uninstall" CACHE STRING "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
add_subdirectory(external/rtaudio EXCLUDE_FROM_ALL)
add_subdirectory(external/rtmidi EXCLUDE_FROM_ALL)

# macOS keeps a file of its own because the application bundle, menu bar,
# activation policy and quit handling live there. Everywhere else choc's
# DesktopWindow covers Win32 and GTK in one implementation.
option(NCH_FORCE_CHOC_WINDOW "Use the choc window layer even on macOS" OFF)
if(APPLE AND NOT NCH_FORCE_CHOC_WINDOW)
    set(nch_window_source src/native-window-mac.mm)
else()
    set(nch_window_source src/native-window-choc.cpp)
endif()

add_library(nch-core STATIC
    ${nch_window_source}
    src/bundle.cpp
    src/command.cpp
    src/commands.cpp
    src/commands-audio.cpp
    src/commands-devices.cpp
    src/commands-ext.cpp
    src/device-settings.cpp
    src/devices.cpp
    src/commands-state.cpp
    src/engine.cpp
    src/midi-file.cpp
    src/note-encoding.cpp
    src/plugin-instance.cpp
    src/plugin-panel.cpp
    src/presets.cpp
    src/random.cpp
    src/validation.cpp
    src/validation-tests.cpp
    src/state-stream.cpp
    src/process-buffers.cpp
    src/process-check.cpp
    src/wav.cpp
    src/webview.cpp
    src/event-generator.cpp
    src/event-list.cpp
    src/gui.cpp
    src/host.cpp
    src/host-extensions.cpp
    src/host-services.cpp
    src/json.cpp
    src/session.cpp
    src/settings-window.cpp
    src/thread-pool.cpp
    src/thread-role.cpp
    src/timeline.cpp
    src/validator.cpp
)
target_include_directories(nch-core PUBLIC src)
target_link_libraries(nch-core PUBLIC clap rtaudio rtmidi)

find_package(Threads REQUIRED)
target_link_libraries(nch-core PUBLIC Threads::Threads)

if(APPLE)
    target_link_libraries(nch-core PUBLIC "-framework CoreFoundation" "-framework Cocoa" "-framework QuartzCore")
elseif(UNIX)
    target_link_libraries(nch-core PUBLIC ${CMAKE_DL_LIBS})
    if(X11_FOUND)
        target_link_libraries(nch-core PUBLIC X11::X11)
    endif()
endif()

# choc's WebView needs WebKit on macOS and gtk+webkit2gtk on Linux, so the
# webview path is opt-out where it is cheap and opt-in where it is not.
if(APPLE)
    set(nch_webview_default ON)
else()
    set(nch_webview_default OFF)
endif()
option(NCH_WITH_WEBVIEW "Display clap.webview interfaces with choc" ${nch_webview_default})
if(NCH_WITH_WEBVIEW)
    set(CHOC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/choc" CACHE PATH "choc")
    target_compile_definitions(nch-core PUBLIC NCH_WITH_WEBVIEW)
    target_include_directories(nch-core PUBLIC "${CHOC_ROOT}")
    # Compost provides the device selector the settings window is built from.
    set(COMPOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/compost" CACHE PATH "compost")
    target_compile_definitions(nch-core PUBLIC NCH_COMPOST_ROOT="${COMPOST_ROOT}")
    if(APPLE)
        set_source_files_properties(src/webview.cpp PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(nch-core PUBLIC "-framework WebKit")
    endif()
    # choc is a header-only library, so its inline functions and function-local
    # statics are emitted as weak symbols. A plug-in that also uses choc has the
    # same symbols, and dyld coalesces weak definitions across images: whichever
    # loads first wins, and the plug-in silently ends up running the host's copy
    # of choc with its own data. Hiding the host's copy keeps the two apart.
    set_target_properties(nch-core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endif()

add_executable(clap-host src/main.cpp)
target_link_libraries(clap-host PRIVATE nch-core)

if(APPLE)
    # WebKit needs the process to be a real application bundle: a WKWebView in
    # a bare executable with no bundle identifier never composites, and the
    # plug-in's interface stays blank. The binary still runs from the terminal
    # exactly as before, it just lives inside clap-host.app now.
    set_target_properties(clap-host PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.charlieculbert.nativeclaphost"
        MACOSX_BUNDLE_BUNDLE_NAME "clap-host"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in")
    # A convenience symlink so `build/clap-host` keeps working.
    add_custom_command(TARGET clap-host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                "$<TARGET_FILE:clap-host>" "${CMAKE_BINARY_DIR}/clap-host-cli"
        COMMENT "Linking build/clap-host-cli to the bundled executable")
endif()

add_subdirectory(fixtures)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
