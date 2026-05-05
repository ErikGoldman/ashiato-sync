cmake_minimum_required(VERSION 3.16)

project(KageSync
    VERSION 0.1.0
    DESCRIPTION "Predictive networking layer built on the kagesoko ECS"
    LANGUAGES CXX
)

include(CTest)
include(cmake/KageSyncDependencies.cmake)

option(KAGE_SYNC_BUILD_BENCHMARKS "Build kage-sync Google Benchmark suites" OFF)
option(KAGE_SYNC_BUILD_EXAMPLES "Build kage-sync examples" OFF)
option(KAGE_SYNC_BUILD_TRACE_VIEWER "Build the Dear ImGui ktrace viewer" OFF)
option(KAGE_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION "Build trace viewer socket automation and PNG screenshots" ON)
option(KAGE_SYNC_ENABLE_COVERAGE "Build kage-sync targets with coverage instrumentation" OFF)
option(KAGE_SYNC_ENABLE_GPROF "Build kage-sync benchmark tools with gprof instrumentation" OFF)
option(KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS "Build kage-sync with buffered interpolation starvation diagnostics" OFF)
option(KAGE_SYNC_ENABLE_TRACING "Build kage-sync with client/server tracing hooks" OFF)
option(KAGE_SYNC_TRACE_COMPONENT_DATA "Build kage-sync tracing support for component data serialization" OFF)
option(KAGE_SYNC_TRACE_PACKET_LOGS "Build kage-sync tracing support for packet event logging" OFF)

set(KAGE_SYNC_BASELINE_FRAME_DELTA_BITS "5" CACHE STRING "Compile-time bit count for relative delta baseline frame numbers")
set(KAGE_SYNC_BALL_STRESS_RUN_MODE "none" CACHE STRING "How run_kage_sync_ball_stress invokes the stress tool: none, gprof, memcheck, or massif")
set_property(CACHE KAGE_SYNC_BALL_STRESS_RUN_MODE PROPERTY STRINGS none gprof memcheck massif)
set(KAGE_SYNC_BALL_STRESS_RUN_ARGS "--duration-seconds 10 --report text" CACHE STRING "Command-line arguments passed by run_kage_sync_ball_stress")
set(KAGE_SYNC_BALL_STRESS_GPROF_OUT "/tmp/kage_sync_ball_stress_gprof.txt" CACHE FILEPATH "gprof report path for run_kage_sync_ball_stress")
set(KAGE_SYNC_PREDICTION_STRESS_RUN_ARGS "--ticks 1800 --latency-frames 10 --misprediction-percent 5 --report text" CACHE STRING "Command-line arguments passed by run_kage_sync_prediction_stress")
set(KAGE_SYNC_TRACE_VIEWER_TRACE_DIR "/tmp/kage-balls-trace" CACHE PATH "Trace directory used by run_kage_sync_trace_viewer_gprof")
set(KAGE_SYNC_TRACE_VIEWER_BENCHMARK_FRAMES "1200" CACHE STRING "Frame count used by trace viewer benchmark and gprof runs")
set(KAGE_SYNC_TRACE_VIEWER_BENCHMARK_OUT "/tmp/kage_sync_trace_viewer_benchmark.json" CACHE FILEPATH "Trace viewer benchmark report path")
set(KAGE_SYNC_TRACE_VIEWER_GPROF_OUT "/tmp/kage_sync_trace_viewer_gprof.txt" CACHE FILEPATH "Trace viewer gprof report path")

set(KAGE_SYNC_BUILD_TESTING ${BUILD_TESTING})

if(KAGE_SYNC_ENABLE_COVERAGE AND NOT KAGE_SYNC_BUILD_TESTING)
    message(FATAL_ERROR "KAGE_SYNC_ENABLE_COVERAGE requires BUILD_TESTING=ON")
endif()

if(KAGE_SYNC_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "KAGE_SYNC_ENABLE_COVERAGE requires GCC or Clang")
    endif()

    function(kage_sync_enable_coverage target_name)
        target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
        target_link_options(${target_name} PRIVATE --coverage)
    endfunction()
endif()

kage_sync_add_ecs_dependency()

add_subdirectory(src)

if(KAGE_SYNC_BUILD_TESTING)
    add_subdirectory(tests)

    if(KAGE_SYNC_ENABLE_COVERAGE)
        find_program(KAGE_SYNC_GCOV_EXECUTABLE gcov REQUIRED)

        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
            COMMAND ${CMAKE_COMMAND}
                -DKAGE_SYNC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DKAGE_SYNC_BINARY_DIR=${CMAKE_BINARY_DIR}
                -DKAGE_SYNC_GCOV_EXECUTABLE=${KAGE_SYNC_GCOV_EXECUTABLE}
                -DKAGE_SYNC_COVERAGE_OUTPUT_DIR=${CMAKE_BINARY_DIR}/coverage
                -P ${CMAKE_SOURCE_DIR}/cmake/KageSyncCoverageReport.cmake
            DEPENDS kage_sync_tests
            USES_TERMINAL
            COMMENT "Running tests and generating gcov coverage stats"
        )
    endif()
endif()

if(KAGE_SYNC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(KAGE_SYNC_BUILD_TRACE_VIEWER)
    add_subdirectory(tools/trace_viewer)
endif()

if(KAGE_SYNC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
