cmake_minimum_required(VERSION 3.20)
project(TextureCompress LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(encoder encoder.cpp)
target_link_libraries(encoder PRIVATE lz4_static)

add_executable(decoder decoder.cpp)

add_executable(overhead_benchmark overhead_benchmark.cpp)
target_link_libraries(overhead_benchmark PRIVATE lz4_static)

if(MSVC)
    target_compile_options(encoder PRIVATE /O2 /EHsc)
    target_compile_options(decoder PRIVATE /O2 /EHsc)
    target_compile_options(overhead_benchmark PRIVATE /O2 /EHsc)
endif()
