cmake_minimum_required(VERSION 3.16)

project(v4l2_basic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output layout: build/bin for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(app
    src/main.cpp
    src/v4l2Camera.cpp
    # Examples: add more source files here
    # src/utils/log.cpp
)

# Use include/ for public headers
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Example: add compile definitions (macros) if needed later
# target_compile_definitions(app PRIVATE USE_V4L2=1)

find_package(Threads REQUIRED)
target_link_libraries(app PRIVATE Threads::Threads v4l2)

# Keep warnings consistent with tasks.json
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic)
endif()
