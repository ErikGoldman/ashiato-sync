#include "server/packet.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("server packet id allocator wraps and repairs invalid next ids") {
    ashiato::sync::server_detail::ServerPacketIdAllocator allocator(
        ashiato::sync::protocol::default_max_pending_packet_acks_per_client);
    std::uint32_t next_packet_id = allocator.max_packet_id();

    REQUIRE(allocator.allocate(next_packet_id) == allocator.max_packet_id());
    REQUIRE(next_packet_id == 1);
    REQUIRE(allocator.allocate(next_packet_id) == 1);
    REQUIRE(next_packet_id == 2);

    next_packet_id = 0;
    REQUIRE(allocator.allocate(next_packet_id) == 1);

    next_packet_id = allocator.max_packet_id() + 1U;
    REQUIRE(allocator.allocate(next_packet_id) == 1);
}

TEST_CASE("server packet helper writes update header before record payload") {
    ashiato::sync::ReplicationServerOptions options;
    ashiato::BitBuffer records;
    records.push_bool(false);
    records.push_bits(0x5, 3U);

    ashiato::BitBuffer packet = ashiato::sync::server_detail::make_server_packet(
        options.mtu_bytes,
        ashiato::sync::server_detail::configured_packet_id_bits(options),
        42,
        7,
        11,
        3,
        records);

    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::server_update_message);
    REQUIRE(static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U)) == 42);
    REQUIRE(static_cast<std::uint32_t>(packet.read_bits(ashiato::sync::server_detail::configured_packet_id_bits(options))) == 7);
    REQUIRE(static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U)) == 11);
    REQUIRE(static_cast<std::uint16_t>(packet.read_bits(16U)) == 3);
    REQUIRE(packet.read_bool() == false);
    REQUIRE(packet.read_bits(3U) == 0x5);
}
