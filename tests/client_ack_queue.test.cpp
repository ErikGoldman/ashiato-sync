#include "client/store/ack_queue.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ClientAckQueue chunks pending acks by mtu") {
    kage::sync::client_detail::ClientAckQueue queue;
    for (std::uint32_t packet_id = 1; packet_id <= 5; ++packet_id) {
        queue.push(packet_id);
    }

    std::vector<ecs::BitBuffer> packets;
    std::vector<kage::sync::client_detail::ClientAckPacketTrace> traces;
    queue.drain_ack_packets(
        5,
        8,
        packets,
        &traces);

    REQUIRE(queue.size() == 0);
    REQUIRE(packets.size() == 3);
    REQUIRE(traces.size() == 3);
    REQUIRE(traces[0].acks == std::vector<std::uint32_t>{1, 2});
    REQUIRE(traces[1].acks == std::vector<std::uint32_t>{3, 4});
    REQUIRE(traces[2].acks == std::vector<std::uint32_t>{5});

    packets[0].reset_read();
    REQUIRE(static_cast<std::uint8_t>(packets[0].read_bits(8)) == kage::sync::protocol::client_ack_message);
    REQUIRE(static_cast<std::uint16_t>(packets[0].read_bits(16)) == 2);
    REQUIRE(static_cast<std::uint32_t>(packets[0].read_bits(8)) == 1);
    REQUIRE(static_cast<std::uint32_t>(packets[0].read_bits(8)) == 2);

    packets[1].reset_read();
    REQUIRE(static_cast<std::uint8_t>(packets[1].read_bits(8)) == kage::sync::protocol::client_ack_message);
    REQUIRE(static_cast<std::uint16_t>(packets[1].read_bits(16)) == 2);
    REQUIRE(static_cast<std::uint32_t>(packets[1].read_bits(8)) == 3);
    REQUIRE(static_cast<std::uint32_t>(packets[1].read_bits(8)) == 4);

    packets[2].reset_read();
    REQUIRE(static_cast<std::uint8_t>(packets[2].read_bits(8)) == kage::sync::protocol::client_ack_message);
    REQUIRE(static_cast<std::uint16_t>(packets[2].read_bits(16)) == 1);
    REQUIRE(static_cast<std::uint32_t>(packets[2].read_bits(8)) == 5);
}

TEST_CASE("ClientAckQueue leaves pending acks when mtu cannot fit one ack") {
    kage::sync::client_detail::ClientAckQueue queue;
    queue.push(7);

    std::vector<ecs::BitBuffer> packets;
    queue.drain_ack_packets(
        3,
        8,
        packets,
        nullptr);

    REQUIRE(packets.empty());
    REQUIRE(queue.size() == 1);
}
