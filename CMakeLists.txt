cmake_minimum_required(VERSION 3.21)

project(Spout2LayoutFeed VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(NOT WIN32)
    message(FATAL_ERROR "Spout2 Layout Feed is Windows-only because Spout2 is a Windows API")
endif()

if(NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK must point to a Geode SDK checkout")
endif()

file(GLOB_RECURSE MOD_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${MOD_SOURCES})
# Spout.h pulls in its own Windows/OpenGL headers, which clash with the
# Geode precompiled header, so the bridge is compiled on its own.
set_source_files_properties(src/spout/spout_bridge.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    GEODE_MOD_ID="halil.spout2-layout-feed"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

add_subdirectory(third_party/spout)
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

target_link_libraries(${PROJECT_NAME} Spout2Static user32 gdi32 opengl32)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)
endif()

setup_geode_mod(${PROJECT_NAME})
