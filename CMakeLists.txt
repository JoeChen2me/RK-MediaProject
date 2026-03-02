cmake_minimum_required(VERSION 3.16)

project(v4l2_basic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 生成 compile_commands.json 以供 clangd 使用
set(TARGET_SOC "rk3588" CACHE STRING "Target Rockchip SoC")
set(TARGET_ARCH "aarch64" CACHE STRING "Target architecture")
option(USE_RGA "Enable RGA support" ON)
option(USE_LIBJPEG "Enable libjpeg-turbo support for model-zoo utils" ON)

set_property(CACHE TARGET_SOC PROPERTY STRINGS
    rk3588 rk3576 rk356x rv1106 rv1103 rv1126b rk1808 rv1109 rv1126
)
set_property(CACHE TARGET_ARCH PROPERTY STRINGS aarch64 armhf)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel
)

if (USE_RGA)
    set(DISABLE_RGA OFF CACHE BOOL "Disable RGA in model-zoo utils" FORCE)
else()
    set(DISABLE_RGA ON CACHE BOOL "Disable RGA in model-zoo utils" FORCE)
endif()

if (USE_LIBJPEG)
    set(DISABLE_LIBJPEG OFF CACHE BOOL "Disable libjpeg in model-zoo utils" FORCE)
else()
    set(DISABLE_LIBJPEG ON CACHE BOOL "Disable libjpeg in model-zoo utils" FORCE)
endif()

# Output layout: build/bin for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Reuse official model-zoo thirdparty definitions (RKNN/RGA/JPEG include+lib paths).
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/experiments/rknn_model_zoo/3rdparty
    ${CMAKE_BINARY_DIR}/mz_3rdparty
)

add_executable(app
    src/main.cpp
    src/rkmppdec.cpp
    src/rkmppenc.cpp
    src/zlm_publisher.cpp
    src/rkrga.cpp
    src/v4l2Camera.cpp
    src/rknnyolo.cpp
    # Examples: add more source files here
    # src/utils/log.cpp
)

# Use include/ for public headers + RK MPP/RGA headers
# RK 平台常见头文件路径：/usr/include/rockchip, /usr/include/rga
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ZLM
    ${LIBRKNNRT_INCLUDES}
    ${LIBRGA_INCLUDES}
    ${LIBJPEG_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/experiments/rknn_model_zoo/utils
    /usr/include/rockchip
)

# Example: add compile definitions (macros) if needed later
# target_compile_definitions(app PRIVATE USE_V4L2=1)

find_package(Threads REQUIRED)

find_library(ROCKCHIP_MPP_LIB NAMES rockchip_mpp REQUIRED)
if (NOT LIBRKNNRT)
    message(FATAL_ERROR "Failed to resolve RKNN runtime from model-zoo 3rdparty.")
endif()
if (NOT LIBRGA)
    message(FATAL_ERROR "Failed to resolve RGA library from model-zoo 3rdparty.")
endif()

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
    ${LIBRGA}
    ${LIBRKNNRT}
    ${ZLM_MK_API_LIB}
    ${ZLM_TOOLKIT_LIB}
)

get_filename_component(RKNNRT_LIB_DIR ${LIBRKNNRT} DIRECTORY)

# 运行时优先从项目根目录下的 lib/ 加载动态库
set_target_properties(app PROPERTIES
    BUILD_RPATH "${ZLM_LOCAL_LIB_DIR}:${RKNNRT_LIB_DIR}:/lib"
)

# Keep warnings consistent with tasks.json
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic)
endif()
