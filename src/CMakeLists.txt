add_library(kage_sync
    client.cpp
    client/ack_queue.cpp
    client/input_buffer.cpp
    client/packet_window.cpp
    client_clock.cpp
    components.cpp
    detail/frame_data.cpp
    detail/options_validation.cpp
    display.cpp
    server.cpp
    server/client_update_scheduler.cpp
    server/server_client_replicator.cpp
    server/detail.cpp
    server/input_buffer.cpp
    server/packet.cpp
    server/packet_ack.cpp
    server/quantized_frame.cpp
    server/transport.cpp
    snapshot_writer.cpp
    tracing.cpp
)

add_library(kage::sync ALIAS kage_sync)

target_compile_features(kage_sync PUBLIC cxx_std_17)

target_include_directories(kage_sync
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(kage_sync
    PUBLIC
        ecs::ecs
        spdlog::spdlog
)

target_compile_definitions(kage_sync
    PUBLIC
        KAGE_SYNC_BASELINE_FRAME_DELTA_BITS=${KAGE_SYNC_BASELINE_FRAME_DELTA_BITS}
        $<$<BOOL:${KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS}>:KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS=1>
        $<$<BOOL:${KAGE_SYNC_ENABLE_TRACING}>:KAGE_SYNC_ENABLE_TRACING=1>
        $<$<BOOL:${KAGE_SYNC_TRACE_COMPONENT_DATA}>:KAGE_SYNC_TRACE_COMPONENT_DATA=1>
        $<$<BOOL:${KAGE_SYNC_TRACE_PACKET_LOGS}>:KAGE_SYNC_TRACE_PACKET_LOGS=1>
)

if(MSVC)
    target_compile_options(kage_sync PRIVATE /W4)
else()
    target_compile_options(kage_sync PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(KAGE_SYNC_ENABLE_COVERAGE)
    kage_sync_enable_coverage(kage_sync)
endif()
