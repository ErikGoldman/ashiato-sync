if(NOT ASHIATO_SYNC_ENABLE_TRACING)
    message(FATAL_ERROR "ASHIATO_SYNC_BUILD_TRACE_VIEWER requires ASHIATO_SYNC_ENABLE_TRACING=ON")
endif()

ashiato_sync_fetch_glfw()
ashiato_sync_fetch_imgui()

find_package(OpenGL REQUIRED)

add_library(ashiato_sync_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(ashiato_sync_imgui
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(ashiato_sync_imgui
    PUBLIC
        glfw
        OpenGL::GL
)

target_compile_features(ashiato_sync_imgui PUBLIC cxx_std_17)

add_executable(ashiato_sync_trace_viewer
    main.cpp
)

target_link_libraries(ashiato_sync_trace_viewer
    PRIVATE
        ashiato::sync
        ashiato_sync_imgui
)

target_compile_features(ashiato_sync_trace_viewer PRIVATE cxx_std_17)

if(ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION)
    set(ashiato_sync_trace_viewer_stb_include_dir "")
    foreach(candidate
        "${glfw_SOURCE_DIR}/deps"
        "${GLFW_SOURCE_DIR}/deps"
        "${raylib_SOURCE_DIR}/external"
        "${raylib_SOURCE_DIR}/src/external"
    )
        if(EXISTS "${candidate}/stb_image_write.h")
            set(ashiato_sync_trace_viewer_stb_include_dir "${candidate}")
            break()
        endif()
    endforeach()
    if(ashiato_sync_trace_viewer_stb_include_dir STREQUAL "")
        message(FATAL_ERROR "ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION requires stb_image_write.h from GLFW deps or raylib external headers")
    endif()

    target_compile_definitions(ashiato_sync_trace_viewer
        PRIVATE
            ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION=1
    )
    target_include_directories(ashiato_sync_trace_viewer
        PRIVATE
            ${ashiato_sync_trace_viewer_stb_include_dir}
    )
endif()

if(ASHIATO_SYNC_ENABLE_GPROF)
    if(MSVC)
        message(WARNING "ASHIATO_SYNC_ENABLE_GPROF is ignored for the trace viewer with MSVC")
    else()
        ashiato_sync_apply_gprof_to_targets(
            ashiato_sync
            ashiato_sync_imgui
            ashiato_sync_trace_viewer
            ashiato
        )
    endif()

    find_program(GPROF_EXECUTABLE gprof)
    if(NOT GPROF_EXECUTABLE)
        message(FATAL_ERROR "ASHIATO_SYNC_ENABLE_GPROF requires gprof on PATH for run_ashiato_sync_trace_viewer_gprof")
    endif()

    add_custom_target(run_ashiato_sync_trace_viewer_gprof
        COMMAND ${CMAKE_COMMAND} -E remove -f gmon.out
        COMMAND $<TARGET_FILE:ashiato_sync_trace_viewer>
            --trace-dir ${ASHIATO_SYNC_TRACE_VIEWER_TRACE_DIR}
            --benchmark
            --benchmark-report ${ASHIATO_SYNC_TRACE_VIEWER_BENCHMARK_OUT}
            --benchmark-frames ${ASHIATO_SYNC_TRACE_VIEWER_BENCHMARK_FRAMES}
        COMMAND ${CMAKE_COMMAND}
            -DGPROF_EXECUTABLE=${GPROF_EXECUTABLE}
            -DSTRESS_EXECUTABLE=$<TARGET_FILE:ashiato_sync_trace_viewer>
            -DGPROF_OUTPUT=${ASHIATO_SYNC_TRACE_VIEWER_GPROF_OUT}
            -DPROFILE_DATA=${PROJECT_BINARY_DIR}/gmon.out
            -P ${PROJECT_SOURCE_DIR}/cmake/RunGprof.cmake
        DEPENDS ashiato_sync_trace_viewer
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        USES_TERMINAL
    )
endif()
