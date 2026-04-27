if(NOT DEFINED GPROF_EXECUTABLE)
    message(FATAL_ERROR "GPROF_EXECUTABLE is required")
endif()

if(NOT DEFINED STRESS_EXECUTABLE)
    message(FATAL_ERROR "STRESS_EXECUTABLE is required")
endif()

if(NOT DEFINED GPROF_OUTPUT)
    message(FATAL_ERROR "GPROF_OUTPUT is required")
endif()

if(NOT DEFINED PROFILE_DATA)
    set(PROFILE_DATA "gmon.out")
endif()

execute_process(
    COMMAND "${GPROF_EXECUTABLE}" "${STRESS_EXECUTABLE}" "${PROFILE_DATA}"
    OUTPUT_FILE "${GPROF_OUTPUT}"
    RESULT_VARIABLE gprof_result
)

if(NOT gprof_result EQUAL 0)
    message(FATAL_ERROR "gprof failed with exit code ${gprof_result}")
endif()

message(STATUS "Wrote gprof report to ${GPROF_OUTPUT}")

