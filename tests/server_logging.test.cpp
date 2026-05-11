#include "ashiato/sync/server.hpp"
#include "test_setup.hpp"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>

#include <sstream>
#include <stdexcept>

namespace {

std::shared_ptr<spdlog::logger> make_test_logger(std::ostringstream& out) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
    auto logger = std::make_shared<spdlog::logger>("test.ashiato.sync.server", sink);
    logger->set_pattern("%l %v");
    return logger;
}

}  // namespace

TEST_CASE("replication server logs malformed client packets as warnings") {
    std::ostringstream logs;

    ashiato::Registry registry;
    ashiato::sync::ReplicationServerOptions options;
    options.logging.level = ashiato::sync::LogLevel::Warning;
    options.logging.logger = make_test_logger(logs);
    ashiato::sync::ReplicationServer server(registry, options);

    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_ack_message, 8U);
    packet.push_bits(1, 16U);

    REQUIRE_FALSE(server.process_packet(99, packet));

    const std::string output = logs.str();
    REQUIRE(output.find("warning event=client_packet_rejected") != std::string::npos);
    REQUIRE(output.find("reason=unknown_peer") != std::string::npos);
    REQUIRE(output.find("event=transport_error") == std::string::npos);
    REQUIRE(output.find("err ") == std::string::npos);

    const ashiato::sync::ReplicationServer::ObservabilityStats stats = server.observability_stats();
    REQUIRE(stats.client_packet_warnings == 1);
    REQUIRE(stats.server_errors == 0);
}

TEST_CASE("replication server logs server callback failures as errors") {
    std::ostringstream logs;

    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServerOptions options;
    options.logging.level = ashiato::sync::LogLevel::Warning;
    options.logging.logger = make_test_logger(logs);
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {
        throw std::runtime_error("send failed");
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    ashiato::BitBuffer ping;
    ping.push_bits(ashiato::sync::protocol::client_ping_message, 8U);
    ping.push_bits(7, 32U);

    REQUIRE_FALSE(server.process_packet(registry, 1, ping));

    const std::string output = logs.str();
    REQUIRE(output.find("error event=transport_error_pong") != std::string::npos);
    REQUIRE(output.find("reason=send_failed") != std::string::npos);
    REQUIRE(output.find("warning event=client_packet_rejected") == std::string::npos);

    const ashiato::sync::ReplicationServer::ObservabilityStats stats = server.observability_stats();
    REQUIRE(stats.server_errors == 1);
    REQUIRE(stats.client_packet_warnings == 0);
}

TEST_CASE("replication server can suppress repeated per-peer warning logs") {
    std::ostringstream logs;

    ashiato::Registry registry;
    ashiato::sync::ReplicationServerOptions options;
    options.logging.level = ashiato::sync::LogLevel::Warning;
    options.logging.max_warning_logs_per_source = 1;
    options.logging.logger = make_test_logger(logs);
    ashiato::sync::ReplicationServer server(registry, options);

    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_ack_message, 8U);
    packet.push_bits(1, 16U);

    REQUIRE_FALSE(server.process_packet(99, packet));
    REQUIRE_FALSE(server.process_packet(99, packet));
    REQUIRE_FALSE(server.process_packet(99, packet));

    const std::string output = logs.str();
    REQUIRE(output.find("event=client_packet_rejected") != std::string::npos);
    REQUIRE(output.find("event=client_packet_warnings_suppressed") != std::string::npos);

    const ashiato::sync::ReplicationServer::ObservabilityStats stats = server.observability_stats();
    REQUIRE(stats.client_packet_warnings == 3);
    REQUIRE(stats.suppressed_client_packet_warnings == 2);
}

TEST_CASE("replication server records connection lifecycle observability") {
    std::ostringstream logs;

    ashiato::Registry registry;
    ashiato::sync::ReplicationServerOptions options;
    options.logging.level = ashiato::sync::LogLevel::Info;
    options.logging.logger = make_test_logger(logs);
    ashiato::sync::ReplicationServer server(registry, options);

    REQUIRE(server.add_client(7));

    const std::string output = logs.str();
    REQUIRE(output.find("info event=client_connected") != std::string::npos);
    REQUIRE(output.find("client=7") != std::string::npos);

    const ashiato::sync::ReplicationServer::ObservabilityStats stats = server.observability_stats();
    REQUIRE(stats.client_connects_accepted == 1);
}
