if(NOT KAGE_SYNC_ENABLE_TRACING)
    message(FATAL_ERROR "KAGE_SYNC_BUILD_TRACE_VIEWER requires KAGE_SYNC_ENABLE_TRACING=ON")
endif()

kage_sync_fetch_glfw()
kage_sync_fetch_imgui()

find_package(OpenGL REQUIRED)

add_library(kage_sync_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(kage_sync_imgui
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(kage_sync_imgui
    PUBLIC
        glfw
        OpenGL::GL
)

target_compile_features(kage_sync_imgui PUBLIC cxx_std_17)

add_executable(kage_sync_trace_viewer
    main.cpp
)

target_link_libraries(kage_sync_trace_viewer
    PRIVATE
        kage::sync
        kage_sync_imgui
)

target_compile_features(kage_sync_trace_viewer PRIVATE cxx_std_17)

if(KAGE_SYNC_ENABLE_GPROF)
    if(MSVC)
        message(WARNING "KAGE_SYNC_ENABLE_GPROF is ignored for the trace viewer with MSVC")
    else()
        kage_sync_apply_gprof_to_targets(
            kage_sync
            kage_sync_imgui
            kage_sync_trace_viewer
            ecs
        )
    endif()

    find_program(GPROF_EXECUTABLE gprof)
    if(NOT GPROF_EXECUTABLE)
        message(FATAL_ERROR "KAGE_SYNC_ENABLE_GPROF requires gprof on PATH for run_kage_sync_trace_viewer_gprof")
    endif()

    add_custom_target(run_kage_sync_trace_viewer_gprof
        COMMAND ${CMAKE_COMMAND} -E remove -f gmon.out
        COMMAND $<TARGET_FILE:kage_sync_trace_viewer>
            --trace-dir ${KAGE_SYNC_TRACE_VIEWER_TRACE_DIR}
            --benchmark
            --benchmark-report ${KAGE_SYNC_TRACE_VIEWER_BENCHMARK_OUT}
            --benchmark-frames ${KAGE_SYNC_TRACE_VIEWER_BENCHMARK_FRAMES}
        COMMAND ${CMAKE_COMMAND}
            -DGPROF_EXECUTABLE=${GPROF_EXECUTABLE}
            -DSTRESS_EXECUTABLE=$<TARGET_FILE:kage_sync_trace_viewer>
            -DGPROF_OUTPUT=${KAGE_SYNC_TRACE_VIEWER_GPROF_OUT}
            -DPROFILE_DATA=${PROJECT_BINARY_DIR}/gmon.out
            -P ${PROJECT_SOURCE_DIR}/cmake/RunGprof.cmake
        DEPENDS kage_sync_trace_viewer
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        USES_TERMINAL
    )
endif()
