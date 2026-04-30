cmake_minimum_required(VERSION 3.16)

project(KageSync
    VERSION 0.1.0
    DESCRIPTION "Predictive networking layer built on the kagesoko ECS"
    LANGUAGES CXX
)

include(CTest)

option(KAGE_SYNC_BUILD_BENCHMARKS "Build kage-sync Google Benchmark suites" OFF)
option(KAGE_SYNC_BUILD_EXAMPLES "Build kage-sync examples" OFF)
option(KAGE_SYNC_ENABLE_GPROF "Build kage-sync benchmark tools with gprof instrumentation" OFF)
option(KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS "Build kage-sync with buffered interpolation starvation diagnostics" OFF)
set(KAGE_SYNC_BASELINE_FRAME_DELTA_BITS "5" CACHE STRING "Compile-time bit count for relative delta baseline frame numbers")
set(KAGE_SYNC_BALL_STRESS_RUN_MODE "none" CACHE STRING "How run_kage_sync_ball_stress invokes the stress tool: none, gprof, memcheck, or massif")
set_property(CACHE KAGE_SYNC_BALL_STRESS_RUN_MODE PROPERTY STRINGS none gprof memcheck massif)
set(KAGE_SYNC_BALL_STRESS_RUN_ARGS "--duration-seconds 10 --report text" CACHE STRING "Command-line arguments passed by run_kage_sync_ball_stress")
set(KAGE_SYNC_BALL_STRESS_GPROF_OUT "/tmp/kage_sync_ball_stress_gprof.txt" CACHE FILEPATH "gprof report path for run_kage_sync_ball_stress")

set(KAGE_SYNC_BUILD_TESTING ${BUILD_TESTING})

set(ECS_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(ECS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ECS_BUILD_PROFILING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../main" "${CMAKE_CURRENT_BINARY_DIR}/ecs")
set(BUILD_TESTING ${KAGE_SYNC_BUILD_TESTING} CACHE BOOL "" FORCE)

add_library(kage_sync
    src/client.cpp
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

target_compile_definitions(kage_sync
    PUBLIC
        KAGE_SYNC_BASELINE_FRAME_DELTA_BITS=${KAGE_SYNC_BASELINE_FRAME_DELTA_BITS}
        $<$<BOOL:${KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS}>:KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS=1>
)

if(MSVC)
    target_compile_options(kage_sync PRIVATE /W4)
else()
    target_compile_options(kage_sync PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(KAGE_SYNC_BUILD_TESTING OR KAGE_SYNC_BUILD_BENCHMARKS OR KAGE_SYNC_BUILD_EXAMPLES)
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
        tests/ball_stress.test.cpp
        tests/bit_buffer.test.cpp
        tests/client.test.cpp
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

if(KAGE_SYNC_BUILD_EXAMPLES)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RAYLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
    )
    FetchContent_MakeAvailable(raylib)

    add_executable(kage_sync_balls_example
        examples/balls.cpp
    )

    target_link_libraries(kage_sync_balls_example
        PRIVATE
            kage::sync
            raylib
    )

    target_compile_features(kage_sync_balls_example PRIVATE cxx_std_17)

    if(WIN32)
        target_link_libraries(kage_sync_balls_example PRIVATE ws2_32)
    endif()
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
        benchmarks/client_benchmark.cpp
        benchmarks/server_benchmark.cpp
    )

    add_executable(kage_sync_ball_stress
        benchmarks/ball_stress.cpp
    )

    target_link_libraries(kage_sync_benchmark
        PRIVATE
            kage::sync
            benchmark::benchmark_main
    )

    target_link_libraries(kage_sync_ball_stress
        PRIVATE
            kage::sync
    )

    target_compile_features(kage_sync_benchmark PRIVATE cxx_std_17)
    target_compile_features(kage_sync_ball_stress PRIVATE cxx_std_17)

    if(KAGE_SYNC_ENABLE_GPROF)
        if(MSVC)
            message(WARNING "KAGE_SYNC_ENABLE_GPROF is ignored for MSVC")
        else()
            target_compile_options(kage_sync PRIVATE -pg -g -fno-omit-frame-pointer)
            target_link_options(kage_sync PRIVATE -pg)
            target_compile_options(kage_sync_ball_stress PRIVATE -pg -g -fno-omit-frame-pointer)
            target_link_options(kage_sync_ball_stress PRIVATE -pg)
            target_compile_options(ecs PRIVATE -pg -g -fno-omit-frame-pointer)
            target_link_options(ecs PRIVATE -pg)
        endif()
    endif()

    separate_arguments(KAGE_SYNC_BALL_STRESS_RUN_ARGS_LIST NATIVE_COMMAND "${KAGE_SYNC_BALL_STRESS_RUN_ARGS}")

    if(KAGE_SYNC_BALL_STRESS_RUN_MODE STREQUAL "none")
        add_custom_target(run_kage_sync_ball_stress
            COMMAND $<TARGET_FILE:kage_sync_ball_stress> ${KAGE_SYNC_BALL_STRESS_RUN_ARGS_LIST}
            DEPENDS kage_sync_ball_stress
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            USES_TERMINAL
        )
    elseif(KAGE_SYNC_BALL_STRESS_RUN_MODE STREQUAL "gprof")
        if(NOT KAGE_SYNC_ENABLE_GPROF)
            message(FATAL_ERROR "KAGE_SYNC_BALL_STRESS_RUN_MODE=gprof requires -DKAGE_SYNC_ENABLE_GPROF=ON")
        endif()
        if(MSVC)
            message(FATAL_ERROR "KAGE_SYNC_BALL_STRESS_RUN_MODE=gprof is not supported with MSVC")
        endif()
        find_program(GPROF_EXECUTABLE gprof)
        if(NOT GPROF_EXECUTABLE)
            message(FATAL_ERROR "KAGE_SYNC_BALL_STRESS_RUN_MODE=gprof requires gprof on PATH")
        endif()
        add_custom_target(run_kage_sync_ball_stress
            COMMAND ${CMAKE_COMMAND} -E remove -f gmon.out
            COMMAND $<TARGET_FILE:kage_sync_ball_stress> ${KAGE_SYNC_BALL_STRESS_RUN_ARGS_LIST}
            COMMAND ${CMAKE_COMMAND}
                -DGPROF_EXECUTABLE=${GPROF_EXECUTABLE}
                -DSTRESS_EXECUTABLE=$<TARGET_FILE:kage_sync_ball_stress>
                -DGPROF_OUTPUT=${KAGE_SYNC_BALL_STRESS_GPROF_OUT}
                -DPROFILE_DATA=${CMAKE_CURRENT_BINARY_DIR}/gmon.out
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunGprof.cmake
            DEPENDS kage_sync_ball_stress
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            USES_TERMINAL
        )
    elseif(KAGE_SYNC_BALL_STRESS_RUN_MODE STREQUAL "memcheck")
        find_program(VALGRIND_EXECUTABLE valgrind)
        if(NOT VALGRIND_EXECUTABLE)
            message(FATAL_ERROR "KAGE_SYNC_BALL_STRESS_RUN_MODE=memcheck requires valgrind on PATH")
        endif()
        add_custom_target(run_kage_sync_ball_stress
            COMMAND ${VALGRIND_EXECUTABLE} --leak-check=full --track-origins=yes --error-exitcode=42 $<TARGET_FILE:kage_sync_ball_stress> ${KAGE_SYNC_BALL_STRESS_RUN_ARGS_LIST}
            DEPENDS kage_sync_ball_stress
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            USES_TERMINAL
        )
    elseif(KAGE_SYNC_BALL_STRESS_RUN_MODE STREQUAL "massif")
        find_program(VALGRIND_EXECUTABLE valgrind)
        if(NOT VALGRIND_EXECUTABLE)
            message(FATAL_ERROR "KAGE_SYNC_BALL_STRESS_RUN_MODE=massif requires valgrind on PATH")
        endif()
        add_custom_target(run_kage_sync_ball_stress
            COMMAND ${VALGRIND_EXECUTABLE} --tool=massif $<TARGET_FILE:kage_sync_ball_stress> ${KAGE_SYNC_BALL_STRESS_RUN_ARGS_LIST}
            DEPENDS kage_sync_ball_stress
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            USES_TERMINAL
        )
    else()
        message(FATAL_ERROR "KAGE_SYNC_BALL_STRESS_RUN_MODE must be one of: none, gprof, memcheck, massif")
    endif()
endif()
