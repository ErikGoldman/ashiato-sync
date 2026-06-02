#pragma once

#include "client/store/frame_payload_ring.hpp"
#include "client/state.hpp"

#include "ashiato/bit_buffer.hpp"
#include "ashiato/sync/components.hpp"
#ifdef ASHIATO_SYNC_ENABLE_TRACING
#include "ashiato/sync/tracing.hpp"
#endif

#include <cstdint>
#include <vector>

namespace ashiato::sync::client_detail {

struct ClientInputRecord {
    SyncFrame frame = 0;
    ashiato::Entity component;
    const std::uint8_t* bytes = nullptr;
};

struct ClientInputPacketTrace {
    std::vector<std::uint32_t> acks;
    SyncFrame baseline_frame = 0;
    SyncFrame first_input_frame = 0;
    SyncFrame last_input_frame = 0;
    bool sent = false;
};

class ClientInputBuffer {
public:
    bool set_latest(
        ashiato::Registry& registry,
        const SyncSettings& settings,
        ashiato::Entity component,
        const void* input);

    bool record_frame(
        const SyncSettings& settings,
        std::size_t capacity_frames,
        SyncFrame frame,
        ClientInputRecord* recorded);

    bool fill_frames_through(
        const SyncSettings& settings,
        std::size_t capacity_frames,
        SyncFrame frame,
        std::vector<ClientInputRecord>& recorded);

    bool apply_frame(ashiato::Registry& registry, const SyncSettings& settings, SyncFrame frame) const;
    void acknowledge_frame(SyncFrame frame);
    void retire_transmit_frames_through(SyncFrame frame) noexcept;
    void apply_latest_to_owned_entities(ashiato::Registry& registry, const SyncSettings& settings) const;

    bool drain_packet(
        std::size_t mtu_bytes,
        std::size_t packet_id_bits,
        std::vector<std::uint32_t>& pending_acks,
        std::vector<ashiato::BitBuffer>& packets,
        ClientInputPacketTrace* trace
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        ,
        const SyncTracer* serialization_tracer = nullptr,
        ClientId client = invalid_client_id,
        SyncFrame trace_frame = 0
#endif
    );

    SyncFrame last_recorded_frame() const noexcept {
        return last_recorded_frame_;
    }

    SyncFrame acked_frame() const noexcept {
        return acked_frame_;
    }

private:
    bool ready_for(const SyncSettings& settings) const noexcept;
    void apply_quantized_to_owned_entities(
        ashiato::Registry& registry,
        const SyncSettings& settings,
        const std::uint8_t* quantized) const;
    void ensure_capacity(std::size_t capacity_frames);
    std::uint8_t* frame_bytes(std::size_t slot) noexcept;
    const std::uint8_t* frame_bytes(std::size_t slot) const noexcept;

    struct InputFrameSlot {
        SyncFrame frame = 0;
        bool valid = false;
    };

    FramePayloadRing<InputFrameSlot> frames_;
    std::vector<std::uint8_t> latest_;
    std::vector<std::uint8_t> acked_baseline_;
    SyncComponentOps ops_;
    ashiato::Entity component_;
    SyncFrame last_recorded_frame_ = 0;
    SyncFrame acked_frame_ = 0;
    SyncFrame retired_transmit_frame_ = 0;
    bool has_latest_ = false;
    bool has_ops_ = false;
    bool has_acked_baseline_ = true;
    bool history_discontinuous_ = false;
};

}  // namespace ashiato::sync::client_detail
