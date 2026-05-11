ashiato_sync_fetch_raylib()

add_executable(ashiato_sync_balls_example
    balls.cpp
)

target_link_libraries(ashiato_sync_balls_example
    PRIVATE
        ashiato::sync
        raylib
)

target_compile_features(ashiato_sync_balls_example PRIVATE cxx_std_17)

if(WIN32)
    target_link_libraries(ashiato_sync_balls_example PRIVATE ws2_32)
endif()

add_subdirectory(fps)
