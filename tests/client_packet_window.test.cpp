#include "client/packet_window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("client packet window records received reordered and duplicate packets") {
    kage::sync::client_detail::ClientPacketWindow window(
        kage::sync::protocol::default_max_pending_packet_acks_per_client);
    kage::sync::ReplicationClientTimingStats stats;

    window.record(10, stats);
    window.record(10, stats);
    window.record(9, stats);

    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 2);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packet_loss == 0.0f);
}

TEST_CASE("client packet window counts gaps after they leave the receive window") {
    kage::sync::client_detail::ClientPacketWindow window(
        kage::sync::protocol::default_max_pending_packet_acks_per_client);
    kage::sync::ReplicationClientTimingStats stats;

    window.record(1, stats);
    window.record(3, stats);
    for (std::uint32_t packet = 4; packet <= 67; ++packet) {
        window.record(packet, stats);
    }

    REQUIRE(stats.server_update_packets_received == 66);
    REQUIRE(stats.server_update_packets_missing == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(1.0f / 67.0f));
}

TEST_CASE("client packet window handles wrapped packet ids") {
    constexpr std::size_t max_pending_packet_acks = kage::sync::protocol::default_max_pending_packet_acks_per_client;
    kage::sync::client_detail::ClientPacketWindow window(max_pending_packet_acks);
    kage::sync::ReplicationClientTimingStats stats;
    const std::uint32_t max_packet_id =
        kage::sync::protocol::packet_id_mask(kage::sync::protocol::packet_id_bits_for_max_pending(max_pending_packet_acks));

    window.record(max_packet_id - 1U, stats);
    window.record(max_packet_id, stats);
    window.record(1, stats);

    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 0);
    REQUIRE(stats.server_update_packets_missing == 0);
}
