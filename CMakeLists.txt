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
    src/rkmppdec.cpp
    src/rkmppenc.cpp
    src/zlm_publisher.cpp
    src/rkrga.cpp
    src/v4l2Camera.cpp
    # Examples: add more source files here
    # src/utils/log.cpp
)

# Use include/ for public headers + RK MPP/RGA headers
# RK 平台常见头文件路径：/usr/include/rockchip, /usr/include/rga
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ZLM
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

# 本地 ZLMediaKit C API 动态库及头文件
set(ZLM_LOCAL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ZLM)
set(ZLM_LOCAL_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

if (NOT EXISTS ${ZLM_LOCAL_INCLUDE_DIR}/mk_mediakit.h)
    message(FATAL_ERROR "Failed to find ZLM C API headers in ${ZLM_LOCAL_INCLUDE_DIR}")
endif()

find_library(ZLM_MK_API_LIB
    NAMES mk_api
    PATHS ${ZLM_LOCAL_LIB_DIR}
    NO_DEFAULT_PATH
)
if (NOT ZLM_MK_API_LIB)
    message(FATAL_ERROR "Failed to find libmk_api.so in ${ZLM_LOCAL_LIB_DIR}")
endif()

find_library(ZLM_TOOLKIT_LIB
    NAMES ZLToolKit
    PATHS ${ZLM_LOCAL_LIB_DIR}
    NO_DEFAULT_PATH
)
if (NOT ZLM_TOOLKIT_LIB)
    message(FATAL_ERROR "Failed to find libZLToolKit.so in ${ZLM_LOCAL_LIB_DIR}")
endif()

target_link_libraries(app PRIVATE
    Threads::Threads
    v4l2
    ${ROCKCHIP_MPP_LIB}
    ${RGA_LIB}
    ${ZLM_MK_API_LIB}
    ${ZLM_TOOLKIT_LIB}
)

# 运行时优先从项目根目录下的 lib/ 加载动态库
set_target_properties(app PROPERTIES
    BUILD_RPATH ${ZLM_LOCAL_LIB_DIR}
)

# Keep warnings consistent with tasks.json
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic)
endif()
