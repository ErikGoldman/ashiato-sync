#pragma once

#include "kage/sync/server.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kage::sync::server_detail {

struct ServerInputPacketTrace {
    SyncFrame baseline_frame = 0;
    SyncFrame first_input_frame = 0;
    SyncFrame last_input_frame = 0;
};

struct ServerDueInput {
    const std::vector<std::uint8_t>* bytes = nullptr;
    SyncFrame frame = 0;
    bool counted = false;
};

class ServerInputBuffer {
public:
    bool set_local_frame(
        SyncFrame frame,
        const std::uint8_t* bytes,
        std::size_t quantized_size,
        std::size_t capacity_frames);

    bool process_packet_payload(
        ecs::BitBuffer& packet,
        const SyncComponentOps& ops,
        std::size_t capacity_frames,
        ServerInputPacketTrace* trace);

    ServerDueInput select_due_input(SyncFrame due_frame, std::size_t quantized_size);

    SyncFrame ack_frame() const noexcept {
        return input_ack_frame_;
    }

    const ReplicationServer::ClientInputStats& stats() const noexcept {
        return stats_;
    }

private:
    struct InputFrame {
        SyncFrame frame = 0;
        bool valid = false;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<InputFrame> input_frames_;
    std::vector<std::uint8_t> latest_input_;
    std::vector<std::uint8_t> latest_applied_input_;
    SyncFrame input_ack_frame_ = 0;
    SyncFrame latest_applied_input_frame_ = 0;
    bool has_latest_input_ = false;
    bool has_latest_applied_input_ = false;
    ReplicationServer::ClientInputStats stats_;
};

}  // namespace kage::sync::server_detail
