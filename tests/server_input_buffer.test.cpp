#include "server/input_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

kage::sync::SyncComponentOps byte_input_ops() {
    kage::sync::SyncComponentOps ops;
    ops.serialization.quantized_size = 1;
    ops.serialization.deserialize = [](
        ecs::BitBuffer& packet,
        const std::uint8_t*,
        std::uint8_t* out,
        ecs::ComponentSerializationContext*) {
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        out[0] = static_cast<std::uint8_t>(packet.read_bits(8U));
        return true;
    };
    return ops;
}

}  // namespace

TEST_CASE("ServerInputBuffer decodes full input frames and selects due inputs") {
    kage::sync::server_detail::ServerInputBuffer buffer;
    ecs::BitBuffer packet;
    packet.push_bits(0, 32U);
    packet.push_bits(2, 16U);
    packet.push_bool(true);
    packet.push_bits(3, 32U);
    packet.push_bits(10, 8U);
    packet.push_bits(11, 8U);

    kage::sync::server_detail::ServerInputPacketTrace trace;
    REQUIRE(buffer.process_packet_payload(packet, byte_input_ops(), 4, &trace));
    REQUIRE(buffer.ack_frame() == 4);
    REQUIRE(buffer.stats().latest_received_input_frame == 4);
    REQUIRE(trace.baseline_frame == 0);
    REQUIRE(trace.first_input_frame == 3);
    REQUIRE(trace.last_input_frame == 4);

    kage::sync::server_detail::ServerInputForFrame due = buffer.select_input_for_frame(3, 1);
    REQUIRE(due.bytes != nullptr);
    REQUIRE((*due.bytes)[0] == 10);
    REQUIRE(due.input_frame == 3);
    REQUIRE(buffer.stats().latest_applied_input_frame == 3);
    REQUIRE(buffer.stats().input_frames_applied == 1);
    REQUIRE(buffer.stats().input_starvation_frames == 0);

    due = buffer.select_input_for_frame(4, 1);
    REQUIRE(due.bytes != nullptr);
    REQUIRE((*due.bytes)[0] == 11);
    REQUIRE(due.input_frame == 4);
    REQUIRE(buffer.stats().input_frames_applied == 2);

    due = buffer.select_input_for_frame(5, 1);
    REQUIRE(due.bytes != nullptr);
    REQUIRE((*due.bytes)[0] == 11);
    REQUIRE(due.input_frame == 4);
    REQUIRE(buffer.stats().input_starvation_frames == 1);
    REQUIRE(buffer.stats().input_reused_frames == 1);
}

TEST_CASE("ServerInputBuffer accepts missing delta baseline without consuming input frames") {
    kage::sync::server_detail::ServerInputBuffer buffer;
    ecs::BitBuffer packet;
    packet.push_bits(7, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(false);
    packet.push_bits(99, 8U);

    kage::sync::server_detail::ServerInputPacketTrace trace;
    REQUIRE(buffer.process_packet_payload(packet, byte_input_ops(), 4, &trace));
    REQUIRE(buffer.ack_frame() == 7);
    REQUIRE(buffer.stats().latest_received_input_frame == 0);
    REQUIRE(trace.baseline_frame == 7);
    REQUIRE(trace.first_input_frame == 0);
    REQUIRE(trace.last_input_frame == 0);
}
