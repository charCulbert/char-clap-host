cmake_minimum_required(VERSION 3.21)
project(nativeClapHost VERSION 0.1.0 LANGUAGES C CXX)

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

add_library(nch-core STATIC
    src/bundle.cpp
    src/command.cpp
    src/commands.cpp
    src/event-list.cpp
    src/host.cpp
    src/json.cpp
    src/session.cpp
    src/thread-role.cpp
    src/validator.cpp
)
target_include_directories(nch-core PUBLIC src)
target_link_libraries(nch-core PUBLIC clap)

find_package(Threads REQUIRED)
target_link_libraries(nch-core PUBLIC Threads::Threads)

if(APPLE)
    target_link_libraries(nch-core PUBLIC "-framework CoreFoundation")
elseif(UNIX)
    target_link_libraries(nch-core PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(clap-host src/main.cpp)
target_link_libraries(clap-host PRIVATE nch-core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
