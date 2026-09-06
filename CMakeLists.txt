cmake_minimum_required(VERSION 3.14)
project(TextureCompress LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(LZ4_BUNDLED_MODE ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(lz4)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

find_package(Threads REQUIRED)

add_executable(encoder encoder.cpp)
target_link_libraries(
    encoder PRIVATE lz4_static libzstd_static Threads::Threads)

add_executable(decoder decoder.cpp)
target_link_libraries(
    decoder PRIVATE lz4_static libzstd_static Threads::Threads)

add_executable(overhead_benchmark overhead_benchmark.cpp)
target_link_libraries(
    overhead_benchmark
    PRIVATE lz4_static libzstd_static Threads::Threads)
