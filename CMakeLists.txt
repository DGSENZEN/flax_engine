cmake_minimum_required(VERSION 3.15)
project(flax_engine C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Emit build/compile_commands.json so clangd can resolve includes (raylib, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/build")

find_package(raylib REQUIRED)

file(GLOB_RECURSE FLAX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(flax ${FLAX_SOURCES})

target_include_directories(flax PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(flax PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/map_editor")
target_include_directories(flax PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/external")
target_link_libraries(flax PRIVATE raylib)

if(APPLE)
    target_link_libraries(flax PRIVATE
        "-framework IOKit"
        "-framework Cocoa"
        "-framework OpenGL")
endif()

# Compile-time path to the assets directory so the engine can load files
# regardless of the current working directory.
target_compile_definitions(flax PRIVATE
    FLAX_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
