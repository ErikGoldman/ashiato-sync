#include "server/packet_ack_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

struct TestAckRecord {
    int value = 0;
};

struct TestPendingPacketAck {
    std::uint32_t packet_id = 0;
    kage::sync::SyncFrame sent_frame = 0;
    std::size_t charged_bytes = 0;
    std::vector<TestAckRecord> records;
};

}  // namespace

TEST_CASE("server packet ack tracker removes stale entries when packet ids wrap") {
    constexpr std::size_t max_pending_packet_acks = 1;
    std::uint32_t next_packet_id = 1;
    std::vector<TestPendingPacketAck> pending{
        TestPendingPacketAck{1, 0, 100, std::vector<TestAckRecord>{{10}}},
        TestPendingPacketAck{2, 0, 200, std::vector<TestAckRecord>{{20}}},
    };

    const std::uint32_t packet_id = kage::sync::server_detail::allocate_tracked_packet_id(
        next_packet_id,
        pending,
        max_pending_packet_acks);

    REQUIRE(packet_id == 1);
    REQUIRE(next_packet_id == 1);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].packet_id == 2);
}

TEST_CASE("server packet ack tracker stores only packets with ack records") {
    std::vector<TestPendingPacketAck> pending;

    kage::sync::server_detail::track_packet_ack(
        pending,
        1,
        10,
        100,
        std::vector<TestAckRecord>{});
    REQUIRE(pending.empty());

    kage::sync::server_detail::track_packet_ack(
        pending,
        2,
        11,
        120,
        std::vector<TestAckRecord>{{20}, {21}});
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].packet_id == 2);
    REQUIRE(pending[0].sent_frame == 11);
    REQUIRE(pending[0].charged_bytes == 120);
    REQUIRE(pending[0].records.size() == 2);
}

TEST_CASE("server packet ack tracker trims oldest pending packets to the configured limit") {
    std::vector<TestPendingPacketAck> pending{
        TestPendingPacketAck{1, 0, 100, std::vector<TestAckRecord>{{10}}},
        TestPendingPacketAck{2, 0, 200, std::vector<TestAckRecord>{{20}}},
        TestPendingPacketAck{3, 0, 300, std::vector<TestAckRecord>{{30}}},
        TestPendingPacketAck{4, 0, 400, std::vector<TestAckRecord>{{40}}},
    };

    kage::sync::server_detail::enforce_pending_packet_ack_limit(pending, 2);

    REQUIRE(pending.size() == 2);
    REQUIRE(pending[0].packet_id == 3);
    REQUIRE(pending[1].packet_id == 4);
}
