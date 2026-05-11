#pragma once

#include "client/store/frame_payload_ring.hpp"
#include "client/state.hpp"

#include "ecs/bit_buffer.hpp"
#include "kage/sync/components.hpp"

#include <cstdint>
#include <vector>

namespace kage::sync::client_detail {

struct ClientInputRecord {
    SyncFrame frame = 0;
    ecs::Entity component;
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
        ecs::Registry& registry,
        const SyncSettings& settings,
        ecs::Entity component,
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

    bool apply_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame) const;
    void acknowledge_frame(SyncFrame frame);
    void retire_transmit_frames_through(SyncFrame frame) noexcept;
    void apply_latest_to_owned_entities(ecs::Registry& registry, const SyncSettings& settings) const;

    bool drain_packet(
        std::size_t mtu_bytes,
        std::size_t packet_id_bits,
        std::vector<std::uint32_t>& pending_acks,
        std::vector<ecs::BitBuffer>& packets,
        ClientInputPacketTrace* trace);

    SyncFrame last_recorded_frame() const noexcept {
        return last_recorded_frame_;
    }

    SyncFrame acked_frame() const noexcept {
        return acked_frame_;
    }

private:
    bool ready_for(const SyncSettings& settings) const noexcept;
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
    ecs::Entity component_;
    SyncFrame last_recorded_frame_ = 0;
    SyncFrame acked_frame_ = 0;
    SyncFrame retired_transmit_frame_ = 0;
    bool has_latest_ = false;
    bool has_ops_ = false;
    bool has_acked_baseline_ = true;
    bool history_discontinuous_ = false;
};

}  // namespace kage::sync::client_detail
