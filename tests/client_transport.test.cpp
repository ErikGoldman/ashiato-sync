#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

namespace {

std::shared_ptr<spdlog::logger> make_test_logger(std::ostringstream& out) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
    auto logger = std::make_shared<spdlog::logger>("test.ashiato.sync.client", sink);
    logger->set_pattern("%l %v");
    return logger;
}

}  // namespace

TEST_CASE("replication client tick emits packets through configured sender") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    std::vector<ashiato::BitBuffer> sent;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    client.set_packet_sender([&](const ashiato::BitBuffer& packet) {
        sent.push_back(packet);
    });

    REQUIRE(client.tick(registry, 0.0));
    REQUIRE_FALSE(sent.empty());
}

TEST_CASE("replication client logs malformed server packets as warnings") {
    std::ostringstream logs;

    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.logging.level = ashiato::sync::LogLevel::Warning;
    options.logging.logger = make_test_logger(logs);
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_update_message, 8U);
    packet.push_bits(1U, 32U);

    REQUIRE_FALSE(client.receive(registry, packet));

    const std::string output = logs.str();
    REQUIRE(output.find("warning event=server_packet_rejected") != std::string::npos);
    REQUIRE(output.find("reason=malformed_server_update") != std::string::npos);
    REQUIRE(output.find("error event=transport_error") == std::string::npos);

    const ashiato::sync::ReplicationClient::ObservabilityStats stats = client.observability_stats();
    REQUIRE(stats.server_packet_warnings == 1);
    REQUIRE(stats.client_errors == 0);
}

TEST_CASE("replication client can suppress repeated server packet warning logs") {
    std::ostringstream logs;

    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.logging.level = ashiato::sync::LogLevel::Warning;
    options.logging.max_warning_logs_per_source = 1;
    options.logging.logger = make_test_logger(logs);
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_update_message, 8U);
    packet.push_bits(1U, 32U);

    REQUIRE_FALSE(client.receive(registry, packet));
    REQUIRE_FALSE(client.receive(registry, packet));
    REQUIRE_FALSE(client.receive(registry, packet));

    const std::string output = logs.str();
    REQUIRE(output.find("event=server_packet_rejected") != std::string::npos);
    REQUIRE(output.find("event=server_packet_warnings_suppressed") != std::string::npos);

    const ashiato::sync::ReplicationClient::ObservabilityStats stats = client.observability_stats();
    REQUIRE(stats.server_packet_warnings == 3);
    REQUIRE(stats.suppressed_server_packet_warnings == 2);
}

TEST_CASE("replication client logs packet sender failures as errors") {
    std::ostringstream logs;

    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.logging.level = ashiato::sync::LogLevel::Warning;
    options.logging.logger = make_test_logger(logs);
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.set_packet_sender([](const ashiato::BitBuffer&) {
        throw std::runtime_error("send failed");
    });

    REQUIRE_THROWS_AS(client.tick(registry, 0.0), std::runtime_error);

    const std::string output = logs.str();
    REQUIRE(output.find("error event=transport_error_client_send") != std::string::npos);
    REQUIRE(output.find("reason=send_failed") != std::string::npos);
    REQUIRE(output.find("warning event=server_packet_rejected") == std::string::npos);

    const ashiato::sync::ReplicationClient::ObservabilityStats stats = client.observability_stats();
    REQUIRE(stats.client_errors == 1);
    REQUIRE(stats.server_packet_warnings == 0);
}

TEST_CASE("replication client queued receive packets are processed during tick") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    ashiato::BitBuffer response;
    response.push_bits(ashiato::sync::protocol::server_connect_response_message, 8U);
    response.push_bits(1U, 1U);
    response.push_unsigned_bits(1U, 64U);
    client.receive_packet(response);

    REQUIRE(client.tick(registry, client.fixed_dt_seconds() * 0.5));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(client.local_time_seconds() == Catch::Approx(client.fixed_dt_seconds() * 0.5));
}

TEST_CASE("replication client rejects malformed connect responses") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.session.connect_token = "token";
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Connecting);

    ashiato::BitBuffer truncated_accept;
    truncated_accept.push_bits(ashiato::sync::protocol::server_connect_response_message, 8U);
    truncated_accept.push_bool(true);
    truncated_accept.push_bits(1, 8U);
    REQUIRE_FALSE(client.receive(registry, truncated_accept));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Connecting);

    ashiato::BitBuffer truncated_reject;
    truncated_reject.push_bits(ashiato::sync::protocol::server_connect_response_message, 8U);
    truncated_reject.push_bool(false);
    truncated_reject.push_bits(5, 16U);
    REQUIRE_FALSE(client.receive(registry, truncated_reject));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Connecting);

    ashiato::BitBuffer invalid_client_id;
    invalid_client_id.push_bits(ashiato::sync::protocol::server_connect_response_message, 8U);
    invalid_client_id.push_bool(true);
    invalid_client_id.push_unsigned_bits(ashiato::sync::max_client_entity_network_id_client + 1U, 64U);
    REQUIRE_FALSE(client.receive(registry, invalid_client_id));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Connecting);

    std::vector<std::uint8_t> fuzz_crash_packet{
        0xffU,
        0xffU,
        0xffU,
        0xffU,
        0xffU,
        0xffU,
        0x00U,
        0x00U,
        0x00U,
        0x05U,
        0x2dU,
    };
    fuzz_crash_packet[0] = ashiato::sync::protocol::server_connect_response_message;
    ashiato::BitBuffer fuzz_crash;
    fuzz_crash.assign_bytes(std::move(fuzz_crash_packet), 11U * 8U);
    REQUIRE_FALSE(client.receive(registry, fuzz_crash));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Connecting);
}

TEST_CASE("replication client stores rejected connect response errors") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.session.connect_token = "token";
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    ashiato::BitBuffer rejected;
    rejected.push_bits(ashiato::sync::protocol::server_connect_response_message, 8U);
    rejected.push_bool(false);
    ashiato::sync::protocol::write_string(rejected, "bad token");

    REQUIRE(client.receive(registry, rejected));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Rejected);
    REQUIRE(client.connect_error() == "bad token");
}

TEST_CASE("replication client queued stale receive packets do not fail tick") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    const ashiato::Entity server_entity{42};
    const ashiato::BitBuffer packet =
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}});
    REQUIRE(receive_at_local_frame(client, registry, packet, 1));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(receive_at_local_frame(client, registry, packet, 1));

    client.receive_packet(packet);
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.pending_ack_count() == 0);
}

TEST_CASE("replication client delays server update packet loss until gaps leave the receive window") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    const ashiato::Entity server_entity{42};

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}, 2U, 1U), 1));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}, 2U, 3U), 2));

    const ashiato::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 2);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 0);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client fills packet loss gaps from reordered server update packets") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    const ashiato::Entity first_entity{42};
    const ashiato::Entity second_entity{43};
    const ashiato::Entity reordered_entity{44};

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U), 1));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(3, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 3U), 3));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(4, {{reordered_entity, Position{2.0f, 3.0f}}}, 2U, 2U), 4));

    const ashiato::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client counts packet loss when gaps leave the receive window") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    const ashiato::Entity server_entity{42};

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}, 2U, 1U), 1));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(3, {{server_entity, Position{3.0f, 4.0f}}}, 2U, 3U), 3));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(66, {{server_entity, Position{6.0f, 6.0f}}}, 2U, 66U), 66));

    const ashiato::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 1);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 0);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(1.0f / 4.0f));
}

TEST_CASE("replication client treats duplicate server update packet ids as duplicate without loss") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    const ashiato::Entity first_entity{42};
    const ashiato::Entity duplicate_packet_id_entity{43};

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U), 1));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(2, {{duplicate_packet_id_entity, Position{1.0f, 2.0f}}}, 2U, 1U), 2));

    const ashiato::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 2);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client fills packet loss gaps from reordered wrapped packet ids") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    const ashiato::Entity first_entity{42};
    const ashiato::Entity second_entity{43};
    const ashiato::Entity reordered_entity{44};

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 254U), 1));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(2, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 1U), 2));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(3, {{reordered_entity, Position{2.0f, 3.0f}}}, 2U, 255U), 3));

    const ashiato::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client does not undo confirmed loss for packets older than the receive window") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    const ashiato::Entity first_entity{42};
    const ashiato::Entity second_entity{43};
    const ashiato::Entity far_entity{44};
    const ashiato::Entity late_entity{45};

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U), 1));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(3, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 3U), 3));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(66, {{far_entity, Position{6.0f, 6.0f}}}, 2U, 66U), 66));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(67, {{late_entity, Position{2.0f, 3.0f}}}, 2U, 2U), 67));

    const ashiato::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 4);
    REQUIRE(stats.server_update_packets_missing == 1);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(1.0f / 5.0f));
}

TEST_CASE("client connect handshake ACKs accepted id until first update") {
    ashiato::Registry client_registry;
    ashiato_sync_tests::define_position_archetype(client_registry);

    ashiato::sync::ReplicationClientOptions options;
    options.session.connect_token = "token";
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    std::vector<ashiato::BitBuffer> packets = client.drain_packets();
    REQUIRE(packets.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(packets[0].read_bits(8U)) ==
            ashiato::sync::protocol::client_connect_request_message);
    std::string token;
    REQUIRE(ashiato::sync::protocol::read_string(packets[0], token));
    REQUIRE(token == "token");

    ashiato::BitBuffer accepted;
    accepted.push_bits(ashiato::sync::protocol::server_connect_response_message, 8U);
    accepted.push_bool(true);
    accepted.push_unsigned_bits(7, 64U);
    REQUIRE(client.receive(client_registry, accepted));
    REQUIRE(client.client_id() == 7);
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(client_registry.get<ashiato::sync::SyncSettings>().local_client == 7);

    packets = client.drain_packets();
    REQUIRE_FALSE(packets.empty());
    bool saw_connect_ack = false;
    for (ashiato::BitBuffer packet : packets) {
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message == ashiato::sync::protocol::client_connect_ack_message) {
            saw_connect_ack = true;
            REQUIRE(packet.read_unsigned_bits(64U) == 7);
        }
    }
    REQUIRE(saw_connect_ack);

    const ashiato::Entity server_entity{42};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready);

    REQUIRE(client.tick(client_registry, client.options().session.connect_resend_interval_seconds));
    packets = client.drain_packets();
    for (ashiato::BitBuffer packet : packets) {
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) !=
                ashiato::sync::protocol::client_connect_ack_message);
    }
}
