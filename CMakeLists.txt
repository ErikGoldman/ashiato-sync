cmake_minimum_required(VERSION 3.16)

project(AshiatoSync
    VERSION 0.1.0
    DESCRIPTION "Predictive networking layer built on the Ashiato ECS"
    LANGUAGES CXX
)

include(CTest)
include(cmake/AshiatoSyncDependencies.cmake)

option(ASHIATO_SYNC_BUILD_BENCHMARKS "Build ashiato-sync Google Benchmark suites" OFF)
option(ASHIATO_SYNC_BUILD_EXAMPLES "Build ashiato-sync examples" OFF)
option(ASHIATO_SYNC_BUILD_FUZZERS "Build ashiato-sync libFuzzer protocol fuzzers" OFF)
option(ASHIATO_SYNC_BUILD_STATIC_LIBRARY "Build ashiato-sync as a static library" OFF)
option(ASHIATO_SYNC_BUILD_TRACE_VIEWER "Build the Dear ImGui ktrace viewer" OFF)
option(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION "Build trace viewer socket automation and PNG screenshots" ON)
option(ASHIATO_SYNC_ENABLE_COVERAGE "Build ashiato-sync targets with coverage instrumentation" OFF)
option(ASHIATO_SYNC_ENABLE_GPROF "Build ashiato-sync benchmark tools with gprof instrumentation" OFF)
option(ASHIATO_SYNC_ENABLE_PROFILING "Build ashiato-sync profiling callback scopes" OFF)
option(ASHIATO_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS "Build ashiato-sync with buffered interpolation starvation diagnostics" OFF)
option(ASHIATO_SYNC_ENABLE_CLANG_TIDY "Run clang-tidy while compiling ashiato-sync targets" OFF)
option(ASHIATO_SYNC_ENABLE_CPPCHECK "Run cppcheck while compiling ashiato-sync targets" OFF)
option(ASHIATO_SYNC_ENABLE_TRACING "Build ashiato-sync tracing support" OFF)
option(ASHIATO_SYNC_TRACE_COMPONENT_DATA "Build ashiato-sync tracing support for component data serialization" OFF)
option(ASHIATO_SYNC_TRACE_PACKET_LOGS "Build ashiato-sync tracing support for packet event logging" OFF)

if(ASHIATO_SYNC_ENABLE_TRACING)
    set(ASHIATO_ENABLE_SERIALIZATION_TRACING ON CACHE BOOL "Build serialization payload trace capture helpers" FORCE)
endif()

set(ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS "5" CACHE STRING "Compile-time bit count for relative delta baseline frame numbers")
set(ASHIATO_SYNC_BALL_STRESS_RUN_MODE "none" CACHE STRING "How run_ashiato_sync_ball_stress invokes the stress tool: none, gprof, memcheck, or massif")
set_property(CACHE ASHIATO_SYNC_BALL_STRESS_RUN_MODE PROPERTY STRINGS none gprof memcheck massif)
set(ASHIATO_SYNC_BALL_STRESS_RUN_ARGS "--duration-seconds 10 --report text" CACHE STRING "Command-line arguments passed by run_ashiato_sync_ball_stress")
set(ASHIATO_SYNC_BALL_STRESS_GPROF_OUT "/tmp/ashiato_sync_ball_stress_gprof.txt" CACHE FILEPATH "gprof report path for run_ashiato_sync_ball_stress")
set(ASHIATO_SYNC_PREDICTION_STRESS_RUN_ARGS "--ticks 1800 --latency-frames 10 --misprediction-percent 5 --report text" CACHE STRING "Command-line arguments passed by run_ashiato_sync_prediction_stress")
set(ASHIATO_SYNC_TRACE_VIEWER_TRACE_DIR "/tmp/ashiato-balls-trace" CACHE PATH "Trace directory used by run_ashiato_sync_trace_viewer_gprof")
set(ASHIATO_SYNC_TRACE_VIEWER_BENCHMARK_FRAMES "1200" CACHE STRING "Frame count used by trace viewer benchmark and gprof runs")
set(ASHIATO_SYNC_TRACE_VIEWER_BENCHMARK_OUT "/tmp/ashiato_sync_trace_viewer_benchmark.json" CACHE FILEPATH "Trace viewer benchmark report path")
set(ASHIATO_SYNC_TRACE_VIEWER_GPROF_OUT "/tmp/ashiato_sync_trace_viewer_gprof.txt" CACHE FILEPATH "Trace viewer gprof report path")

set(ASHIATO_SYNC_BUILD_TESTING ${BUILD_TESTING})

if(ASHIATO_SYNC_ENABLE_COVERAGE AND NOT ASHIATO_SYNC_BUILD_TESTING)
    message(FATAL_ERROR "ASHIATO_SYNC_ENABLE_COVERAGE requires BUILD_TESTING=ON")
endif()

if(ASHIATO_SYNC_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ASHIATO_SYNC_ENABLE_COVERAGE requires GCC or Clang")
    endif()

    function(ashiato_sync_enable_coverage target_name)
        target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
        target_link_options(${target_name} PRIVATE --coverage)
    endfunction()
endif()

function(ashiato_sync_apply_strict_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Werror
        )
    endif()
endfunction()

function(ashiato_sync_apply_static_analysis target_name)
    if(ASHIATO_SYNC_ENABLE_CLANG_TIDY)
        find_program(ASHIATO_SYNC_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
        set_target_properties(${target_name} PROPERTIES
            CXX_CLANG_TIDY "${ASHIATO_SYNC_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
        )
    endif()

    if(ASHIATO_SYNC_ENABLE_CPPCHECK)
        find_program(ASHIATO_SYNC_CPPCHECK_EXECUTABLE NAMES cppcheck REQUIRED)
        set_target_properties(${target_name} PROPERTIES
            CXX_CPPCHECK "${ASHIATO_SYNC_CPPCHECK_EXECUTABLE};--enable=warning,performance,portability;--error-exitcode=1;--inline-suppr;--std=c++17;--suppress=missingIncludeSystem"
        )
    endif()
endfunction()

if(ASHIATO_SYNC_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ASHIATO_SYNC_BUILD_FUZZERS requires Clang/libFuzzer")
    endif()

    function(ashiato_sync_enable_fuzzer_instrumentation target_name)
        target_compile_options(${target_name} PRIVATE
            -g
            -O1
            -fno-omit-frame-pointer
            -fsanitize=fuzzer-no-link,address,undefined
        )
    endfunction()
endif()

ashiato_sync_add_ashiato_dependency()
ashiato_sync_fetch_spdlog()

add_subdirectory(src)

if(ASHIATO_SYNC_BUILD_FUZZERS)
    ashiato_sync_enable_fuzzer_instrumentation(ashiato)
    ashiato_sync_enable_fuzzer_instrumentation(ashiato_sync)
    add_subdirectory(fuzzers)
endif()

if(ASHIATO_SYNC_BUILD_TESTING)
    add_subdirectory(tests)

    if(ASHIATO_SYNC_ENABLE_COVERAGE)
        find_program(ASHIATO_SYNC_GCOV_EXECUTABLE gcov REQUIRED)

        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
            COMMAND ${CMAKE_COMMAND}
                -DASHIATO_SYNC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DASHIATO_SYNC_BINARY_DIR=${CMAKE_BINARY_DIR}
                -DASHIATO_SYNC_GCOV_EXECUTABLE=${ASHIATO_SYNC_GCOV_EXECUTABLE}
                -DASHIATO_SYNC_COVERAGE_OUTPUT_DIR=${CMAKE_BINARY_DIR}/coverage
                -P ${CMAKE_SOURCE_DIR}/cmake/AshiatoSyncCoverageReport.cmake
            DEPENDS ashiato_sync_tests
            USES_TERMINAL
            COMMENT "Running tests and generating gcov coverage stats"
        )
    endif()
endif()

if(ASHIATO_SYNC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ASHIATO_SYNC_BUILD_TRACE_VIEWER)
    add_subdirectory(tools/trace_viewer)
endif()

if(ASHIATO_SYNC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
