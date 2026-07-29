cmake_minimum_required(VERSION 3.14)
project(TextureCompress LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libdeflate
    GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git
    GIT_TAG v1.25
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libdeflate)

find_package(Threads REQUIRED)

add_executable(encoder encoder.cpp)
target_link_libraries(
    encoder PRIVATE libdeflate::libdeflate_static Threads::Threads)

add_executable(decoder decoder.cpp)

add_executable(overhead_benchmark overhead_benchmark.cpp)
target_link_libraries(
    overhead_benchmark
    PRIVATE libdeflate::libdeflate_static Threads::Threads)

add_executable(
    sampling_comparison_benchmark sampling_comparison_benchmark.cpp)
target_link_libraries(
    sampling_comparison_benchmark
    PRIVATE libdeflate::libdeflate_static Threads::Threads)
