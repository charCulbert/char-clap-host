# Vendored from https://github.com/WebCLAP/wclap-bridge -- see VENDORED.md.
# Built against the host's own CLAP headers rather than the copy upstream
# carries, so one binary holds one version of them.
cmake_minimum_required(VERSION 3.24)

add_library(wclap-bridge STATIC
	source/wclap-bridge.cpp
	source/wclap-instance-wasmtime/wclap-instance-wasmtime.cpp
)
target_include_directories(wclap-bridge PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(wclap-bridge PUBLIC clap)

add_library(wclap-cpp INTERFACE)
target_include_directories(wclap-cpp INTERFACE ${CMAKE_CURRENT_LIST_DIR}/modules/wclap-cpp/include)

target_link_libraries(wclap-bridge PRIVATE wclap-cpp)

include(${CMAKE_CURRENT_LIST_DIR}/wasmtime-fetched.cmake)
target_link_libraries(wclap-bridge PRIVATE wasmtime)
