# ============================================================================
# CMakeLists.txt — cross-platform bridge for the Bambu networking plugin
# Builds the bridge binary for Linux (g++/clang) and Windows (MSVC or MinGW g++).
#
# NOTE (ABI):
#   Linux  -> load libbambu_networking.so built by GCC/Clang. Build this bridge
#             with g++/clang++ (libstdc++).  OK.
#   Windows-> load bambu_networking.dll built by MSVC.  To call it correctly the
#             bridge MUST be built with MSVC (same std::string / std::function
#             ABI).  MinGW g++ will NOT be ABI-compatible for C++ types passed
#             across the boundary, so the plugin calls will fail/crash.
# ============================================================================
cmake_minimum_required(VERSION 3.13)
project(bambu_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Link snapshot codecs into the bridge; no external decoder at runtime.
set(_bambu_library_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
if(NOT MSVC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()
find_package(JPEG REQUIRED)
find_path(OPENH264_INCLUDE_DIR NAMES wels/codec_api.h)
find_library(OPENH264_LIBRARY_RELEASE NAMES openh264 libopenh264.a)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_bambu_library_suffixes})
if(NOT OPENH264_INCLUDE_DIR OR NOT OPENH264_LIBRARY_RELEASE)
    message(FATAL_ERROR "OpenH264 development headers and static library are required (see README.md)")
endif()
if(MSVC AND VCPKG_TARGET_TRIPLET)
    find_library(OPENH264_LIBRARY_DEBUG NAMES openh264 libopenh264.a
        PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
endif()
include(SelectLibraryConfigurations)
select_library_configurations(OPENH264)

add_executable(bambu-bridge
    src/main.cpp
    src/bridge.cpp
    src/plugin_loader.cpp
    src/json.cpp
    src/rpc_handler.cpp
    src/stdio_transport.cpp
    src/mcp_stdio.cpp
    src/http_server.cpp
    src/camera_stream.cpp
)

target_include_directories(bambu-bridge PRIVATE include src)

if(WIN32)
    target_compile_definitions(bambu-bridge PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_link_libraries(bambu-bridge PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(bambu-bridge PRIVATE Threads::Threads)
    target_link_libraries(bambu-bridge PRIVATE ${CMAKE_DL_LIBS})
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bambu-bridge PRIVATE -Wall -Wextra)
endif()

add_executable(bambu-bridge-self-test
    src/self_test.cpp
    src/bridge.cpp
    src/plugin_loader.cpp
    src/json.cpp
    src/rpc_handler.cpp
    src/stdio_transport.cpp
    src/mcp_stdio.cpp
    src/http_server.cpp
    src/camera_stream.cpp
)
target_include_directories(bambu-bridge-self-test PRIVATE include src)
if(WIN32)
    target_compile_definitions(bambu-bridge-self-test PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_link_libraries(bambu-bridge-self-test PRIVATE ws2_32)
else()
    target_link_libraries(bambu-bridge-self-test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

foreach(target bambu-bridge bambu-bridge-self-test)
    target_include_directories(${target} PRIVATE ${OPENH264_INCLUDE_DIR})
    target_link_libraries(${target} PRIVATE ${OPENH264_LIBRARY} JPEG::JPEG)
endforeach()

# Optional: point at your plugin directory at build time for convenience.
set(BAMBU_PLUGIN_DIR "" CACHE PATH "Directory containing bamboo_networking(.so/.dll)")
if(BAMBU_PLUGIN_DIR)
    target_compile_definitions(bambu-bridge PRIVATE
        BAMBU_DEFAULT_PLUGIN_DIR="${BAMBU_PLUGIN_DIR}")
endif()
