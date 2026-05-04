kage_sync_fetch_raylib()

add_executable(kage_sync_balls_example
    balls.cpp
)

target_link_libraries(kage_sync_balls_example
    PRIVATE
        kage::sync
        raylib
)

target_compile_features(kage_sync_balls_example PRIVATE cxx_std_17)

if(WIN32)
    target_link_libraries(kage_sync_balls_example PRIVATE ws2_32)
endif()

add_subdirectory(fps)
