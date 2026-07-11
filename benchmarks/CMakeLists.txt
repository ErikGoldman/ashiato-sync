ashiato_sync_fetch_google_benchmark()

add_executable(ashiato_sync_benchmark
    allocation_tracker.cpp
    client_benchmark.cpp
    server_benchmark.cpp
)

add_executable(ashiato_sync_ball_stress
    ball_stress.cpp
)

add_executable(ashiato_sync_prediction_stress
    prediction_stress.cpp
)

target_link_libraries(ashiato_sync_benchmark
    PRIVATE
        ashiato::sync
        benchmark::benchmark_main
)

target_link_libraries(ashiato_sync_ball_stress
    PRIVATE
        ashiato::sync
)

target_link_libraries(ashiato_sync_prediction_stress
    PRIVATE
        ashiato::sync
)

target_compile_features(ashiato_sync_benchmark PRIVATE cxx_std_17)
target_compile_features(ashiato_sync_ball_stress PRIVATE cxx_std_17)
target_compile_features(ashiato_sync_prediction_stress PRIVATE cxx_std_17)

ashiato_sync_apply_gprof_to_targets(
    ashiato_sync
    ashiato_sync_ball_stress
    ashiato_sync_prediction_stress
    ashiato
)

separate_arguments(ASHIATO_SYNC_BALL_STRESS_RUN_ARGS_LIST NATIVE_COMMAND "${ASHIATO_SYNC_BALL_STRESS_RUN_ARGS}")
separate_arguments(ASHIATO_SYNC_PREDICTION_STRESS_RUN_ARGS_LIST NATIVE_COMMAND "${ASHIATO_SYNC_PREDICTION_STRESS_RUN_ARGS}")

add_custom_target(run_ashiato_sync_prediction_stress
    COMMAND $<TARGET_FILE:ashiato_sync_prediction_stress> ${ASHIATO_SYNC_PREDICTION_STRESS_RUN_ARGS_LIST}
    DEPENDS ashiato_sync_prediction_stress
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    USES_TERMINAL
)

if(ASHIATO_SYNC_BALL_STRESS_RUN_MODE STREQUAL "none")
    add_custom_target(run_ashiato_sync_ball_stress
        COMMAND $<TARGET_FILE:ashiato_sync_ball_stress> ${ASHIATO_SYNC_BALL_STRESS_RUN_ARGS_LIST}
        DEPENDS ashiato_sync_ball_stress
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        USES_TERMINAL
    )
elseif(ASHIATO_SYNC_BALL_STRESS_RUN_MODE STREQUAL "gprof")
    if(NOT ASHIATO_SYNC_ENABLE_GPROF)
        message(FATAL_ERROR "ASHIATO_SYNC_BALL_STRESS_RUN_MODE=gprof requires -DASHIATO_SYNC_ENABLE_GPROF=ON")
    endif()
    if(MSVC)
        message(FATAL_ERROR "ASHIATO_SYNC_BALL_STRESS_RUN_MODE=gprof is not supported with MSVC")
    endif()
    find_program(GPROF_EXECUTABLE gprof)
    if(NOT GPROF_EXECUTABLE)
        message(FATAL_ERROR "ASHIATO_SYNC_BALL_STRESS_RUN_MODE=gprof requires gprof on PATH")
    endif()
    add_custom_target(run_ashiato_sync_ball_stress
        COMMAND ${CMAKE_COMMAND} -E remove -f gmon.out
        COMMAND $<TARGET_FILE:ashiato_sync_ball_stress> ${ASHIATO_SYNC_BALL_STRESS_RUN_ARGS_LIST}
        COMMAND ${CMAKE_COMMAND}
            -DGPROF_EXECUTABLE=${GPROF_EXECUTABLE}
            -DSTRESS_EXECUTABLE=$<TARGET_FILE:ashiato_sync_ball_stress>
            -DGPROF_OUTPUT=${ASHIATO_SYNC_BALL_STRESS_GPROF_OUT}
            -DPROFILE_DATA=${PROJECT_BINARY_DIR}/gmon.out
            -P ${PROJECT_SOURCE_DIR}/cmake/RunGprof.cmake
        DEPENDS ashiato_sync_ball_stress
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        USES_TERMINAL
    )
elseif(ASHIATO_SYNC_BALL_STRESS_RUN_MODE STREQUAL "memcheck")
    find_program(VALGRIND_EXECUTABLE valgrind)
    if(NOT VALGRIND_EXECUTABLE)
        message(FATAL_ERROR "ASHIATO_SYNC_BALL_STRESS_RUN_MODE=memcheck requires valgrind on PATH")
    endif()
    add_custom_target(run_ashiato_sync_ball_stress
        COMMAND ${VALGRIND_EXECUTABLE} --leak-check=full --track-origins=yes --error-exitcode=42 $<TARGET_FILE:ashiato_sync_ball_stress> ${ASHIATO_SYNC_BALL_STRESS_RUN_ARGS_LIST}
        DEPENDS ashiato_sync_ball_stress
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        USES_TERMINAL
    )
elseif(ASHIATO_SYNC_BALL_STRESS_RUN_MODE STREQUAL "massif")
    find_program(VALGRIND_EXECUTABLE valgrind)
    if(NOT VALGRIND_EXECUTABLE)
        message(FATAL_ERROR "ASHIATO_SYNC_BALL_STRESS_RUN_MODE=massif requires valgrind on PATH")
    endif()
    add_custom_target(run_ashiato_sync_ball_stress
        COMMAND ${VALGRIND_EXECUTABLE} --tool=massif $<TARGET_FILE:ashiato_sync_ball_stress> ${ASHIATO_SYNC_BALL_STRESS_RUN_ARGS_LIST}
        DEPENDS ashiato_sync_ball_stress
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        USES_TERMINAL
    )
else()
    message(FATAL_ERROR "ASHIATO_SYNC_BALL_STRESS_RUN_MODE must be one of: none, gprof, memcheck, massif")
endif()
