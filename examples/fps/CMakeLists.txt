add_executable(kage_sync_fps_example
    src/app.cpp
    src/client.cpp
    src/game/constants.cpp
    src/game/jobs.cpp
    src/game/math.cpp
    src/game/schema.cpp
    src/game/simulation.cpp
    src/main.cpp
    src/net.cpp
    src/rendering.cpp
    src/server.cpp
    src/trace.cpp
)

target_include_directories(kage_sync_fps_example
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(kage_sync_fps_example
    PRIVATE
        kage::sync
        raylib
)

target_compile_features(kage_sync_fps_example PRIVATE cxx_std_17)

if(WIN32)
    target_link_libraries(kage_sync_fps_example PRIVATE ws2_32)
endif()
