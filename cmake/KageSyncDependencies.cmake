include(FetchContent)

function(kage_sync_add_ecs_dependency)
    set(kage_sync_build_testing ${BUILD_TESTING})

    set(KAGE_SYNC_KAGESOKO_GIT_REPOSITORY
        "https://github.com/ErikGoldman/kagesoto.git"
        CACHE STRING "kagesoko ECS Git repository"
    )
    set(KAGE_SYNC_KAGESOKO_GIT_TAG
        "eef993892879a6fe6cb9a3ee5129d931f150871a"
        CACHE STRING "Pinned kagesoko ECS Git commit/tag"
    )
    set(KAGE_SYNC_KAGESOKO_SOURCE_DIR
        ""
        CACHE PATH "Optional local kagesoko checkout override"
    )

    set(ECS_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(ECS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(ECS_BUILD_PROFILING OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

    if(KAGE_SYNC_KAGESOKO_SOURCE_DIR)
        add_subdirectory("${KAGE_SYNC_KAGESOKO_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/ecs")
    else()
        FetchContent_Declare(
            kagesoko
            GIT_REPOSITORY "${KAGE_SYNC_KAGESOKO_GIT_REPOSITORY}"
            GIT_TAG "${KAGE_SYNC_KAGESOKO_GIT_TAG}"
            GIT_PROGRESS TRUE
        )
        FetchContent_MakeAvailable(kagesoko)
    endif()

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

    FetchContent_GetProperties(glfw)
    if(glfw_SOURCE_DIR)
        set(glfw_SOURCE_DIR "${glfw_SOURCE_DIR}" PARENT_SCOPE)
    elseif(GLFW_SOURCE_DIR)
        set(glfw_SOURCE_DIR "${GLFW_SOURCE_DIR}" PARENT_SCOPE)
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

function(kage_sync_fetch_spdlog)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.3
    )

    FetchContent_MakeAvailable(spdlog)
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
