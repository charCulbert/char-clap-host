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
    src/app-data.cpp
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
    src/midi-player.cpp
    src/note-encoding.cpp
    src/plugin-instance.cpp
    src/plugin-panel.cpp
    src/presets.cpp
    src/random.cpp
    src/recent-files.cpp
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
    src/host-page.cpp
    src/thread-pool.cpp
    src/thread-role.cpp
    src/timeline.cpp
    src/validator.cpp
)
target_include_directories(nch-core PUBLIC src)
target_link_libraries(nch-core PUBLIC clap rtaudio rtmidi)

# choc's Linux window layer is GTK-based, and its optional WebView layer adds
# WebKitGTK.  Keep the platform dependencies here rather than relying on the
# consuming executable to discover headers and transitive libraries.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    target_link_libraries(nch-core PUBLIC PkgConfig::GTK3)
endif()

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

# choc's WebView uses WebKit on macOS, WebView2 on Windows and webkit2gtk on
# Linux (libwebkit2gtk-4.1-dev); the host's own windows are webviews too.
option(NCH_WITH_WEBVIEW "Display clap.webview interfaces with choc" ON)
# Compost provides the components the host's own windows are built from. The
# path is a fact about the source tree whether or not a webview shows it, so
# the code that serves it compiles either way.
set(COMPOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/compost" CACHE PATH "compost")
target_compile_definitions(nch-core PUBLIC NCH_COMPOST_ROOT="${COMPOST_ROOT}")
# choc is header-only and used beyond the webview (files, MIDI, WAV, base64).
set(CHOC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/choc" CACHE PATH "choc")
target_include_directories(nch-core PUBLIC "${CHOC_ROOT}")
# choc is a header-only library, so its inline functions and function-local
# statics are emitted as weak symbols. A plug-in that also uses choc has the
# same symbols, and dyld coalesces weak definitions across images: whichever
# loads first wins, and the plug-in silently ends up running the host's copy
# of choc with its own data. Hiding the host's copy keeps the two apart.
set_target_properties(nch-core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(NCH_WITH_WEBVIEW)
    target_compile_definitions(nch-core PUBLIC NCH_WITH_WEBVIEW)
    if(APPLE)
        set_source_files_properties(src/webview.cpp PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(nch-core PUBLIC "-framework WebKit")
    elseif(UNIX)
        pkg_check_modules(WEBKIT2GTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
        target_link_libraries(nch-core PUBLIC PkgConfig::WEBKIT2GTK)
    endif()
endif()

# WCLAPs (CLAP compiled to WebAssembly) load through the vendored wclap-bridge,
# which runs the module in Wasmtime and hands back an ordinary plug-in factory.
# Configuring downloads the Wasmtime C API, so it can be turned off.
option(NCH_WITH_WCLAP "Load .wclap plug-ins through wclap-bridge and Wasmtime" ON)
if(NCH_WITH_WCLAP)
    add_subdirectory(external/wclap-bridge wclap-bridge EXCLUDE_FROM_ALL)
    # Designated initializers in wclap-bridge need C++20 on MSVC (Clang allows them in C++17).
    set_target_properties(wclap-bridge PROPERTIES CXX_STANDARD 20)
    target_compile_definitions(nch-core PUBLIC NCH_WITH_WCLAP)
    target_link_libraries(nch-core PUBLIC wclap-bridge)
endif()

add_executable(clap-host src/main.cpp)
target_link_libraries(clap-host PRIVATE nch-core)

if(APPLE)
    # WebKit needs the process to be a real application bundle: a WKWebView in
    # a bare executable with no bundle identifier never composites, and the
    # plug-in's interface stays blank. The binary still runs from the terminal
    # exactly as before, it just lives inside clap-host.app now.
    # resources/clap-host.icns is committed; resources/make-icon.swift redraws it.
    set(NCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/clap-host.icns")
    target_sources(clap-host PRIVATE "${NCH_ICON}")
    set_source_files_properties("${NCH_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(clap-host PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "clap-host.icns"
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
elseif(WIN32)
    target_sources(clap-host PRIVATE resources/clap-host.rc)
    # The window layer's menu bar and open dialog.
    target_link_libraries(nch-core PUBLIC comctl32 comdlg32)
endif()

add_subdirectory(fixtures)

# `cmake --install build` puts the application where the Finder can find it and
# a launcher where a shell and an agent can. Installed rather than symlinked
# into the build directory, which a clean rebuild would take away.
if(APPLE)
    set(NCH_APP_DIR "/Applications" CACHE PATH "Where clap-host.app is installed")
    set(NCH_BIN_DIR "$ENV{HOME}/.local/bin" CACHE PATH "Where the clap-host launcher is installed")

    install(TARGETS clap-host BUNDLE DESTINATION "${NCH_APP_DIR}")
    # The launcher runs the binary inside the bundle, so the command line and
    # the Finder start exactly the same program -- which matters, because
    # WebKit will not composite a webview outside an application bundle.
    install(CODE "
        file(MAKE_DIRECTORY \"${NCH_BIN_DIR}\")
        file(WRITE \"${NCH_BIN_DIR}/clap-host\"
             \"#!/bin/sh\\n# Runs the binary inside clap-host.app: a webview needs the bundle.\\nexec \\\"${NCH_APP_DIR}/clap-host.app/Contents/MacOS/clap-host\\\" \\\"$@\\\"\\n\")
        execute_process(COMMAND chmod +x \"${NCH_BIN_DIR}/clap-host\")
        message(STATUS \"Installed ${NCH_APP_DIR}/clap-host.app and ${NCH_BIN_DIR}/clap-host\")
    ")
elseif(WIN32)
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Programs/clap-host" nch_default_app_dir)
    set(NCH_APP_DIR "${nch_default_app_dir}" CACHE PATH "Where clap-host.exe is installed")

    install(TARGETS clap-host RUNTIME DESTINATION "${NCH_APP_DIR}")
    # The Start menu and PATH stand in for /Applications and ~/.local/bin.
    install(CODE "
        file(TO_NATIVE_PATH \"${NCH_APP_DIR}\" app_dir)
        execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                                -File \"${CMAKE_CURRENT_SOURCE_DIR}/cmake/install-windows.ps1\" \"\${app_dir}\"
                        COMMAND_ERROR_IS_FATAL ANY)
        message(STATUS \"Installed ${NCH_APP_DIR}/clap-host.exe, a Start menu shortcut, and the folder on PATH\")
    ")
else()
    install(TARGETS clap-host RUNTIME DESTINATION bin)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
