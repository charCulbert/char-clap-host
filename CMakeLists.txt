cmake_minimum_required(VERSION 3.21)
project(nativeClapHost VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)  # the Cocoa window layer
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/clap" CACHE PATH "CLAP SDK")
if(NOT EXISTS "${CLAP_SDK_ROOT}/include/clap/clap.h")
    message(FATAL_ERROR "The CLAP SDK is missing. Run: git submodule update --init --recursive")
endif()
add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)

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

if(APPLE)
    set(nch_window_source src/native-window-mac.mm)
else()
    set(nch_window_source src/native-window-stub.cpp)
endif()

add_library(nch-core STATIC
    ${nch_window_source}
    src/bundle.cpp
    src/command.cpp
    src/commands.cpp
    src/commands-audio.cpp
    src/commands-devices.cpp
    src/commands-ext.cpp
    src/devices.cpp
    src/commands-state.cpp
    src/engine.cpp
    src/presets.cpp
    src/state-stream.cpp
    src/process-buffers.cpp
    src/wav.cpp
    src/event-list.cpp
    src/gui.cpp
    src/host.cpp
    src/host-extensions.cpp
    src/host-services.cpp
    src/json.cpp
    src/session.cpp
    src/thread-role.cpp
    src/validator.cpp
)
target_include_directories(nch-core PUBLIC src)
target_link_libraries(nch-core PUBLIC clap rtaudio rtmidi)

find_package(Threads REQUIRED)
target_link_libraries(nch-core PUBLIC Threads::Threads)

if(APPLE)
    target_link_libraries(nch-core PUBLIC "-framework CoreFoundation" "-framework Cocoa")
elseif(UNIX)
    target_link_libraries(nch-core PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(clap-host src/main.cpp)
target_link_libraries(clap-host PRIVATE nch-core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
