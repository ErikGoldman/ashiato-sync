kage_sync_fetch_google_benchmark()

add_executable(kage_sync_benchmark
    client_benchmark.cpp
    server_benchmark.cpp
)

add_executable(kage_sync_ball_stress
    ball_stress.cpp
)

add_executable(kage_sync_prediction_stress
    prediction_stress.cpp
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

target_link_libraries(kage_sync_prediction_stress
    PRIVATE
        kage::sync
)

target_compile_features(kage_sync_benchmark PRIVATE cxx_std_17)
target_compile_features(kage_sync_ball_stress PRIVATE cxx_std_17)
target_compile_features(kage_sync_prediction_stress PRIVATE cxx_std_17)

kage_sync_apply_gprof_to_targets(
    kage_sync
    kage_sync_ball_stress
    kage_sync_prediction_stress
    ecs
)

separate_arguments(KAGE_SYNC_BALL_STRESS_RUN_ARGS_LIST NATIVE_COMMAND "${KAGE_SYNC_BALL_STRESS_RUN_ARGS}")
separate_arguments(KAGE_SYNC_PREDICTION_STRESS_RUN_ARGS_LIST NATIVE_COMMAND "${KAGE_SYNC_PREDICTION_STRESS_RUN_ARGS}")

add_custom_target(run_kage_sync_prediction_stress
    COMMAND $<TARGET_FILE:kage_sync_prediction_stress> ${KAGE_SYNC_PREDICTION_STRESS_RUN_ARGS_LIST}
    DEPENDS kage_sync_prediction_stress
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    USES_TERMINAL
)

if(KAGE_SYNC_BALL_STRESS_RUN_MODE STREQUAL "none")
    add_custom_target(run_kage_sync_ball_stress
        COMMAND $<TARGET_FILE:kage_sync_ball_stress> ${KAGE_SYNC_BALL_STRESS_RUN_ARGS_LIST}
        DEPENDS kage_sync_ball_stress
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
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
            -DPROFILE_DATA=${PROJECT_BINARY_DIR}/gmon.out
            -P ${PROJECT_SOURCE_DIR}/cmake/RunGprof.cmake
        DEPENDS kage_sync_ball_stress
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
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
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
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
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        USES_TERMINAL
    )
else()
    message(FATAL_ERROR "KAGE_SYNC_BALL_STRESS_RUN_MODE must be one of: none, gprof, memcheck, massif")
endif()
