# Vendored from https://github.com/WebCLAP/wclap-bridge -- see VENDORED.md.
# Built against the host's own CLAP headers and choc rather than the copies
# upstream carries, so one binary holds one version of each.
cmake_minimum_required(VERSION 3.24)

add_library(wclap-bridge STATIC
	source/wclap-bridge.cpp
	source/wclap-instance-wasmtime/wclap-instance-wasmtime.cpp
)
target_include_directories(wclap-bridge PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(wclap-bridge PUBLIC clap)

add_library(wclap-cpp INTERFACE)
target_include_directories(wclap-cpp INTERFACE ${CMAKE_CURRENT_LIST_DIR}/modules/wclap-cpp/include)

add_library(webview-gui STATIC ${CMAKE_CURRENT_LIST_DIR}/modules/webview-gui/source/webview-gui.cpp)
target_include_directories(webview-gui PUBLIC ${CMAKE_CURRENT_LIST_DIR}/modules/webview-gui/include)
target_link_libraries(webview-gui PUBLIC clap)
if(APPLE)
	target_link_libraries(webview-gui PRIVATE "-framework WebKit")
else()
	# Every other platform goes through choc's WebView.
	target_include_directories(webview-gui PRIVATE "${WCLAP_BRIDGE_CHOC_ROOT}")
	if(UNIX)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(wclap_gtk3 REQUIRED IMPORTED_TARGET gtk+-3.0)
		pkg_check_modules(wclap_webkit2 REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
		target_link_libraries(webview-gui PRIVATE PkgConfig::wclap_gtk3 PkgConfig::wclap_webkit2)
	endif()
endif()

target_link_libraries(wclap-bridge PRIVATE wclap-cpp webview-gui)

include(${CMAKE_CURRENT_LIST_DIR}/wasmtime-fetched.cmake)
target_link_libraries(wclap-bridge PRIVATE wasmtime)
