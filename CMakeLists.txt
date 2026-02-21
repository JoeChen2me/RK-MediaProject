cmake_minimum_required(VERSION 3.16)

project(v4l2_basic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 生成 compile_commands.json 以供 clangd 使用

# Output layout: build/bin for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(app
    src/main.cpp
    src/rkmpp.cpp
    src/rkrga.cpp
    src/v4l2Camera.cpp
    # Examples: add more source files here
    # src/utils/log.cpp
)

# Use include/ for public headers + RK MPP/RGA headers
# RK 平台常见头文件路径：/usr/include/rockchip, /usr/include/rga
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    /usr/include/rockchip
    /usr/include/rga
)

# Example: add compile definitions (macros) if needed later
# target_compile_definitions(app PRIVATE USE_V4L2=1)

find_package(Threads REQUIRED)

# 显式查找 Rockchip MPP / RGA 动态库，便于后续接入解码与图像处理链路
find_path(RGA_INCLUDE_DIR
    NAMES im2d.h
    PATHS /usr/include/rga
    NO_DEFAULT_PATH
)
if (NOT RGA_INCLUDE_DIR)
    message(FATAL_ERROR "Failed to find RGA headers (im2d.h) in /usr/include/rga")
endif()

target_include_directories(app PRIVATE ${RGA_INCLUDE_DIR})

find_library(ROCKCHIP_MPP_LIB NAMES rockchip_mpp REQUIRED)
find_library(RGA_LIB NAMES rga REQUIRED)

target_link_libraries(app PRIVATE
    Threads::Threads
    v4l2
    ${ROCKCHIP_MPP_LIB}
    ${RGA_LIB}
)

# Keep warnings consistent with tasks.json
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic)
endif()
