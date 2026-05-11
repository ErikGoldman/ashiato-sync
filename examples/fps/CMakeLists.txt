add_executable(ashiato_sync_fps_example
    src/app.cpp
    src/game/constants.cpp
    src/game/jobs.cpp
    src/game/math.cpp
    src/game/schema.cpp
    src/game/simulation.cpp
    src/main.cpp
    src/net.cpp
    src/raylib_frontend/client.cpp
    src/raylib_frontend/listen_server_frontend.cpp
    src/raylib_frontend/rendering.cpp
    src/replay.cpp
    src/server.cpp
    src/trace.cpp
)

target_include_directories(ashiato_sync_fps_example
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ashiato_sync_fps_example
    PRIVATE
        ashiato::sync
        raylib
)

target_compile_features(ashiato_sync_fps_example PRIVATE cxx_std_17)

if(WIN32)
    target_link_libraries(ashiato_sync_fps_example PRIVATE ws2_32)
endif()
