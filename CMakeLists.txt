cmake_minimum_required(VERSION 3.16)

project(KageSync
    VERSION 0.1.0
    DESCRIPTION "Predictive networking layer built on the kagesoko ECS"
    LANGUAGES CXX
)

include(CTest)

option(KAGE_SYNC_BUILD_BENCHMARKS "Build kage-sync Google Benchmark suites" OFF)

set(KAGE_SYNC_BUILD_TESTING ${BUILD_TESTING})

set(ECS_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(ECS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ECS_BUILD_PROFILING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../main" "${CMAKE_CURRENT_BINARY_DIR}/ecs")
set(BUILD_TESTING ${KAGE_SYNC_BUILD_TESTING} CACHE BOOL "" FORCE)

add_library(kage_sync
    src/components.cpp
    src/server.cpp
)

add_library(kage::sync ALIAS kage_sync)

target_compile_features(kage_sync PUBLIC cxx_std_17)

target_include_directories(kage_sync
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(kage_sync
    PUBLIC
        ecs::ecs
)

if(MSVC)
    target_compile_options(kage_sync PRIVATE /W4)
else()
    target_compile_options(kage_sync PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(KAGE_SYNC_BUILD_TESTING OR KAGE_SYNC_BUILD_BENCHMARKS)
    include(FetchContent)
endif()

if(KAGE_SYNC_BUILD_TESTING)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
    )

    FetchContent_MakeAvailable(Catch2)

    add_executable(kage_sync_tests
        tests/bit_buffer.test.cpp
        tests/components.test.cpp
        tests/server.test.cpp
    )

    target_link_libraries(kage_sync_tests
        PRIVATE
            kage::sync
            Catch2::Catch2WithMain
    )

    include(Catch)
    catch_discover_tests(kage_sync_tests)
endif()

if(KAGE_SYNC_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        google_benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
    )

    FetchContent_MakeAvailable(google_benchmark)

    add_executable(kage_sync_benchmark
        benchmarks/server_benchmark.cpp
    )

    target_link_libraries(kage_sync_benchmark
        PRIVATE
            kage::sync
            benchmark::benchmark_main
    )

    target_compile_features(kage_sync_benchmark PRIVATE cxx_std_17)
endif()
