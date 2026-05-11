set(ASHIATO_SYNC_LIBRARY_TYPE)
if(ASHIATO_SYNC_BUILD_STATIC_LIBRARY)
    set(ASHIATO_SYNC_LIBRARY_TYPE STATIC)
endif()

add_library(ashiato_sync ${ASHIATO_SYNC_LIBRARY_TYPE}
    client.cpp
    client/entity.cpp
    client/runtime/buffered_runtime.cpp
    client/runtime/cue_runtime.cpp
    client/runtime/display_sampler.cpp
    client/runtime/prediction_runtime.cpp
    client/runtime/update_runtime.cpp
    client/store/ack_queue.cpp
    client/store/cue_store.cpp
    client/store/entity_store.cpp
    client/store/frame_ring_store.cpp
    client/input.cpp
    client/store/input_buffer.cpp
    client/timing_stats.cpp
    client/tracing.cpp
    client/transport.cpp
    client_clock.cpp
    components.cpp
    detail/frame_data.cpp
    detail/logging.cpp
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
    replay_streamer.cpp
    replay_writer.cpp
    tracing.cpp
)

add_library(ashiato::sync ALIAS ashiato_sync)

target_compile_features(ashiato_sync PUBLIC cxx_std_17)

target_include_directories(ashiato_sync
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ashiato_sync
    PUBLIC
        ashiato::ashiato
        spdlog::spdlog
)

target_compile_definitions(ashiato_sync
    PUBLIC
        ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS=${ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS}
        $<$<BOOL:${ASHIATO_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS}>:ASHIATO_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS=1>
        $<$<BOOL:${ASHIATO_SYNC_ENABLE_TRACING}>:ASHIATO_SYNC_ENABLE_TRACING=1>
        $<$<BOOL:${ASHIATO_SYNC_TRACE_COMPONENT_DATA}>:ASHIATO_SYNC_TRACE_COMPONENT_DATA=1>
        $<$<BOOL:${ASHIATO_SYNC_TRACE_PACKET_LOGS}>:ASHIATO_SYNC_TRACE_PACKET_LOGS=1>
)

if(MSVC)
    target_compile_options(ashiato_sync PRIVATE /W4)
else()
    target_compile_options(ashiato_sync PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ASHIATO_SYNC_ENABLE_COVERAGE)
    ashiato_sync_enable_coverage(ashiato_sync)
endif()
