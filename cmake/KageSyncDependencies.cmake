include(FetchContent)

function(kage_sync_add_ecs_dependency)
    set(kage_sync_build_testing ${BUILD_TESTING})

    set(ECS_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(ECS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(ECS_BUILD_PROFILING OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

    add_subdirectory("${PROJECT_SOURCE_DIR}/../main" "${PROJECT_BINARY_DIR}/ecs")

    set(BUILD_TESTING ${kage_sync_build_testing} CACHE BOOL "" FORCE)
endfunction()

function(kage_sync_fetch_catch2)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
    )

    FetchContent_MakeAvailable(Catch2)

    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
endfunction()

function(kage_sync_fetch_raylib)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RAYLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
    )

    FetchContent_MakeAvailable(raylib)
endfunction()

function(kage_sync_fetch_glfw)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)

    if(NOT TARGET glfw)
        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
        )
        FetchContent_MakeAvailable(glfw)
    endif()
endfunction()

function(kage_sync_fetch_imgui)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9b
    )
    FetchContent_MakeAvailable(imgui)

    set(imgui_SOURCE_DIR "${imgui_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(kage_sync_fetch_google_benchmark)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        google_benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
    )

    FetchContent_MakeAvailable(google_benchmark)
endfunction()

function(kage_sync_apply_gprof_to_targets)
    if(NOT KAGE_SYNC_ENABLE_GPROF)
        return()
    endif()

    if(MSVC)
        message(WARNING "KAGE_SYNC_ENABLE_GPROF is ignored for MSVC")
        return()
    endif()

    foreach(target_name IN LISTS ARGN)
        if(TARGET ${target_name})
            target_compile_options(${target_name} PRIVATE -pg -g -fno-omit-frame-pointer)
            target_link_options(${target_name} PRIVATE -pg)
        endif()
    endforeach()
endfunction()
