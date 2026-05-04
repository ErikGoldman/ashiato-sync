#include "../benchmarks/ball_stress.hpp"
#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace stress = kage::sync::stress;

namespace {

stress::StressConfig test_config() {
    stress::StressConfig config;
    config.clients = 1;
    config.max_balls = 8;
    config.spawn_interval_ms = 100000.0;
    config.poison_min = 2;
    config.poison_max = 2;
    config.health_min = 10;
    config.health_max = 10;
    config.tick_rate = 4.0;
    config.duration_seconds = 1.0;
    config.loss_percent = 0.0;
    config.latency_ms = 0.0;
    return config;
}

stress::ServerBall make_ball(
    ecs::Registry& registry,
    kage::sync::SyncArchetypeId archetype,
    stress::BallPosition position,
    stress::BallHealth health,
    float vx,
    float vy,
    float vz) {
    const ecs::Entity entity = registry.create();
    registry.add<stress::BallPosition>(entity, position);
    registry.add<stress::BallVisual>(entity, stress::BallVisual{});
    registry.add<stress::BallHealth>(entity, health);
    registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype});
    return stress::ServerBall{entity, vx, vy, vz};
}

}  // namespace

TEST_CASE("ball stress bounce adds poison in configured range") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const stress::SyncSchema schema = stress::define_schema(registry);
    stress::StressConfig config = test_config();
    std::mt19937 rng(7);
    stress::StressReport report;
    double spawn_accumulator = 0.0;

    std::vector<stress::ServerBall> balls;
    balls.push_back(make_ball(
        registry,
        schema.ball,
        stress::BallPosition{9.95f, 0.0f, 0.0f},
        stress::BallHealth{10},
        4.0f,
        0.0f,
        0.0f));

    stress::update_server_world(registry, balls, schema, config, 0.25, spawn_accumulator, rng, report);

    REQUIRE(balls.size() == 1);
    REQUIRE(registry.contains<stress::BallPoison>(balls[0].entity));
    REQUIRE(registry.has(balls[0].entity, schema.bounced));
    REQUIRE(registry.get<stress::BallPoison>(balls[0].entity).remaining == 1);
    REQUIRE(report.poison_components_added == 1);
    REQUIRE(report.poison_ticks == 1);
    REQUIRE(report.bounce_tags_added == 1);
}

TEST_CASE("ball stress spawn randomly assigns synced tags") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const stress::SyncSchema schema = stress::define_schema(registry);
    stress::StressConfig config = test_config();
    config.max_balls = 32;
    config.spawn_interval_ms = 1.0;
    std::mt19937 rng(13);
    stress::StressReport report;
    double spawn_accumulator = 1.0;
    std::vector<stress::ServerBall> balls;

    stress::update_server_world(registry, balls, schema, config, 0.1, spawn_accumulator, rng, report);

    std::size_t tagged = 0;
    for (const stress::ServerBall& ball : balls) {
        if (registry.has(ball.entity, schema.spawn_tagged)) {
            ++tagged;
        }
    }
    REQUIRE(tagged > 0);
    REQUIRE(report.spawn_tags_added == tagged);
}

TEST_CASE("ball stress schema syncs multiple tags to clients") {
    ecs::Registry server_registry;
    kage::sync::configure_server(server_registry);
    const stress::SyncSchema server_schema = stress::define_schema(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<stress::BallPosition>(
                server_entity, stress::BallPosition{1.0f, 2.0f, 3.0f}) != nullptr);
    REQUIRE(server_registry.add<stress::BallVisual>(server_entity, stress::BallVisual{}) != nullptr);
    REQUIRE(server_registry.add<stress::BallHealth>(server_entity, stress::BallHealth{10}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_schema.spawn_tagged));
    REQUIRE(server_registry.add_tag(server_entity, server_schema.bounced));
    REQUIRE(
        server_registry.add<kage::sync::Replicated>(
            server_entity, kage::sync::Replicated{server_schema.ball})
        != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    kage::sync::configure_client(client_registry, 1);
    const stress::SyncSchema client_schema = stress::define_schema(client_registry);
    REQUIRE(client_schema.ball == server_schema.ball);

    kage::sync::ReplicationClient client;
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local = client.local_entity(kage_sync_tests::first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.has(local, client_schema.spawn_tagged));
    REQUIRE(client_registry.has(local, client_schema.bounced));
}

TEST_CASE("ball stress poison ticks health and removes component") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const stress::SyncSchema schema = stress::define_schema(registry);
    stress::StressConfig config = test_config();
    std::mt19937 rng(9);
    stress::StressReport report;
    double spawn_accumulator = 0.0;

    std::vector<stress::ServerBall> balls;
    balls.push_back(make_ball(
        registry,
        schema.ball,
        stress::BallPosition{0.0f, 0.0f, 0.0f},
        stress::BallHealth{3},
        0.0f,
        0.0f,
        0.0f));
    registry.add<stress::BallPoison>(balls[0].entity, stress::BallPoison{1, 0.0f});

    stress::update_server_world(registry, balls, schema, config, 0.25, spawn_accumulator, rng, report);

    REQUIRE(balls.size() == 1);
    REQUIRE(registry.get<stress::BallHealth>(balls[0].entity).value == 2);
    REQUIRE_FALSE(registry.contains<stress::BallPoison>(balls[0].entity));
    REQUIRE(report.poison_components_removed == 1);
    REQUIRE(report.poison_ticks == 1);
}

TEST_CASE("ball stress despawns balls at zero health") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const stress::SyncSchema schema = stress::define_schema(registry);
    stress::StressConfig config = test_config();
    std::mt19937 rng(11);
    stress::StressReport report;
    double spawn_accumulator = 0.0;

    std::vector<stress::ServerBall> balls;
    balls.push_back(make_ball(
        registry,
        schema.ball,
        stress::BallPosition{0.0f, 0.0f, 0.0f},
        stress::BallHealth{1},
        0.0f,
        0.0f,
        0.0f));
    const ecs::Entity entity = balls[0].entity;
    registry.add<stress::BallPoison>(entity, stress::BallPoison{1, 0.0f});

    stress::update_server_world(registry, balls, schema, config, 0.25, spawn_accumulator, rng, report);

    REQUIRE(balls.empty());
    REQUIRE_FALSE(registry.alive(entity));
    REQUIRE(report.despawned == 1);
}

TEST_CASE("ball stress spawn cap is respected") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const stress::SyncSchema schema = stress::define_schema(registry);
    stress::StressConfig config = test_config();
    config.max_balls = 3;
    config.spawn_interval_ms = 1.0;
    std::mt19937 rng(13);
    stress::StressReport report;
    double spawn_accumulator = 1.0;
    std::vector<stress::ServerBall> balls;

    stress::update_server_world(registry, balls, schema, config, 0.1, spawn_accumulator, rng, report);

    REQUIRE(balls.size() == 3);
    REQUIRE(report.spawned == 3);
}

TEST_CASE("ball stress simulated transport applies latency and loss") {
    stress::SimulatedLink link;
    stress::DirectionStats stats;
    link.settings.latency_ms = 100.0;
    link.settings.loss_percent = 0.0;

    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ack_message, 8U);
    packet.push_bits(0, 16U);

    stress::enqueue_packet(link, stats, 1, packet, 1.0);
    REQUIRE(stats.packets == 1);
    REQUIRE(stats.client_ack_packets == 1);
    REQUIRE(stats.dropped_packets == 0);

    bool delivered = false;
    stress::deliver_ready(link, stats, 1.05, [&](kage::sync::ClientId, const ecs::BitBuffer&) {
        delivered = true;
    });
    REQUIRE_FALSE(delivered);

    stress::deliver_ready(link, stats, 1.10, [&](kage::sync::ClientId, const ecs::BitBuffer&) {
        delivered = true;
    });
    REQUIRE(delivered);
    REQUIRE(stats.delivered_packets == 1);

    stress::SimulatedLink lossy;
    lossy.settings.loss_percent = 100.0;
    stress::DirectionStats loss_stats;
    stress::enqueue_packet(lossy, loss_stats, 1, packet, 0.0);
    REQUIRE(loss_stats.dropped_packets == 1);
    REQUIRE(lossy.empty());
}

TEST_CASE("ball stress simulated transport applies bounded uniform jitter") {
    stress::SimulatedLink link;
    stress::DirectionStats stats;
    link.settings.latency_ms = 100.0;
    link.settings.jitter_ms = 25.0;
    link.settings.loss_percent = 0.0;
    link.random_engine().seed(123);

    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ack_message, 8U);
    packet.push_bits(0, 16U);

    stress::enqueue_packet(link, stats, 1, packet, 1.0);
    REQUIRE(link.size() == 1);
    REQUIRE(link.queued_packets().front().deliver_at >= 1.075);
    REQUIRE(link.queued_packets().front().deliver_at <= 1.125);

    stress::SimulatedLink clamped;
    clamped.settings.latency_ms = 5.0;
    clamped.settings.jitter_ms = 25.0;
    clamped.random_engine().seed(456);
    stress::DirectionStats clamped_stats;
    stress::enqueue_packet(clamped, clamped_stats, 1, packet, 2.0);
    REQUIRE(clamped.size() == 1);
    REQUIRE(clamped.queued_packets().front().deliver_at >= 2.0);
    REQUIRE(clamped.queued_packets().front().deliver_at <= 2.03);
}

TEST_CASE("ball stress simulated transport delivers by scheduled time under jitter") {
    stress::SimulatedLink link;
    stress::DirectionStats stats;
    link.settings.latency_ms = 50.0;
    link.settings.jitter_ms = 50.0;
    link.random_engine().seed(2);

    ecs::BitBuffer first;
    first.push_bits(kage::sync::protocol::client_ack_message, 8U);
    first.push_bits(1, 16U);
    ecs::BitBuffer second;
    second.push_bits(kage::sync::protocol::client_ack_message, 8U);
    second.push_bits(2, 16U);

    stress::enqueue_packet(link, stats, 1, first, 1.0);
    stress::enqueue_packet(link, stats, 2, second, 1.0);

    REQUIRE(link.size() == 2);
    REQUIRE(link.queued_packets()[0].deliver_at <= link.queued_packets()[1].deliver_at);
}

TEST_CASE("ball stress packet classifier counts update record kinds") {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(7, 32U);
    packet.push_bits(99, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(3, 16U);

    packet.push_bool(false);
    kage::sync::protocol::write_network_entity_id(packet, 100);
    packet.push_bool(true);
    packet.push_bits(1, 32U);
    packet.push_bool(false);
    packet.push_bits(1, 16U);
    packet.push_bits(0, kage::sync::protocol::bits_for_range(stress::WireFormatStats::slot_count));
    packet.push_unsigned_bits(3U, 2U);
    packet.push_bool(false);

    packet.push_bool(false);
    kage::sync::protocol::write_network_entity_id(packet, 101);
    packet.push_bool(false);
    kage::sync::protocol::write_baseline_frame(packet, 7, 6);
    packet.push_bool(true);
    packet.push_bool(false);
    packet.push_bool(false);
    packet.push_bool(false);
    packet.push_bool(false);
    packet.push_unsigned_bits(1U, 2U);
    packet.push_bool(false);

    packet.push_bool(true);
    kage::sync::protocol::write_network_entity_id(packet, 102);

    const stress::PacketBreakdown breakdown = stress::classify_packet(packet);

    REQUIRE(breakdown.type == stress::PacketType::ServerUpdate);
    REQUIRE(breakdown.full_upserts == 1);
    REQUIRE(breakdown.delta_upserts == 1);
    REQUIRE(breakdown.destroys == 1);

    stress::WireFormatStats wire;
    const stress::PacketBreakdown diagnostic_breakdown = stress::classify_packet(packet, &wire);

    REQUIRE(diagnostic_breakdown.type == stress::PacketType::ServerUpdate);
    REQUIRE(wire.packet_bits == 206);
    REQUIRE(wire.padding_bits == 2);
    REQUIRE(wire.server_update_header_bits == kage::sync::protocol::server_update_header_bits);
    REQUIRE(wire.server_update_entities == 3);
    REQUIRE(wire.max_server_update_entities_per_packet == 3);
    REQUIRE(wire.full_upsert_bits == 69);
    REQUIRE(wire.full_upsert_payload_bits == 2);
    REQUIRE(wire.full_upsert_slot_list_records == 1);
    REQUIRE(wire.full_upsert_presence_mask_records == 0);
    REQUIRE(wire.delta_upsert_bits == 28);
    REQUIRE(wire.delta_upsert_payload_bits == 2);
    REQUIRE(wire.delta_baseline_bits == 1 + kage::sync::protocol::baseline_frame_delta_bits);
    REQUIRE(wire.delta_baseline_relative == 1);
    REQUIRE(wire.delta_baseline_absolute == 0);
    REQUIRE(wire.delta_change_mask_bits == stress::WireFormatStats::slot_count);
    REQUIRE(wire.destroy_record_bits == 13);
    REQUIRE(wire.slots[0].records == 2);
    REQUIRE(wire.slots[0].index_bits == 3);
    REQUIRE(wire.slots[0].payload_bits == 4);
}

TEST_CASE("ball stress packet classifier records ACK wire diagnostics") {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ack_message, 8U);
    packet.push_bits(2, 16U);
    packet.push_bits(7, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(8, kage::sync::protocol::server_packet_id_bits);

    stress::WireFormatStats wire;
    const stress::PacketBreakdown breakdown = stress::classify_packet(packet, &wire);

    REQUIRE(breakdown.type == stress::PacketType::ClientAck);
    REQUIRE(wire.packet_bits == kage::sync::protocol::client_ack_header_bits +
            2U * kage::sync::protocol::client_ack_record_bits);
    REQUIRE(wire.padding_bits == 0);
    REQUIRE(wire.ack_header_bits == kage::sync::protocol::client_ack_header_bits);
    REQUIRE(wire.ack_records == 2);
    REQUIRE(wire.ack_record_bits == 2U * kage::sync::protocol::client_ack_record_bits);
}

TEST_CASE("ball stress packet classifier honors custom network id tier width") {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(1, 32U);
    packet.push_bits(1, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(true);
    kage::sync::protocol::write_network_entity_id(packet, 255U, 8U);

    stress::WireFormatStats wire;
    const stress::PacketBreakdown breakdown = stress::classify_packet(packet, &wire, 8U);

    REQUIRE(breakdown.type == stress::PacketType::ServerUpdate);
    REQUIRE(breakdown.destroys == 1);
    REQUIRE(wire.destroy_record_bits == 10U);
}

TEST_CASE("ball stress shared latency and loss defaults apply to both directions") {
    stress::StressConfig config = test_config();
    config.duration_seconds = 0.1;
    config.tick_rate = 10.0;
    config.spawn_interval_ms = 1.0;
    config.max_balls = 2;
    config.latency_ms = 25.0;
    config.jitter_ms = 5.0;
    config.loss_percent = 100.0;

    const stress::StressReport report = stress::run_stress(config);

    REQUIRE(report.config.server_to_client_latency_ms == 25.0);
    REQUIRE(report.config.client_to_server_latency_ms == 25.0);
    REQUIRE(report.config.server_to_client_jitter_ms == 5.0);
    REQUIRE(report.config.client_to_server_jitter_ms == 5.0);
    REQUIRE(report.config.server_to_client_loss_percent == 100.0);
    REQUIRE(report.config.client_to_server_loss_percent == 100.0);
    REQUIRE(report.server_to_clients.dropped_packets == report.server_to_clients.packets);
}

TEST_CASE("ball stress directional jitter overrides shared jitter") {
    stress::StressConfig config = test_config();
    config.duration_seconds = 0.1;
    config.tick_rate = 10.0;
    config.jitter_ms = 5.0;
    config.server_to_client_jitter_ms = 10.0;
    config.client_to_server_jitter_ms = 2.0;

    const stress::StressReport report = stress::run_stress(config);

    REQUIRE(report.config.server_to_client_jitter_ms == 10.0);
    REQUIRE(report.config.client_to_server_jitter_ms == 2.0);
}

TEST_CASE("ball stress validates time dilation config") {
    stress::StressConfig config = test_config();
    config.time_dilation_min = 0.0;
    REQUIRE_THROWS_AS(stress::run_stress(config), std::invalid_argument);

    config = test_config();
    config.time_dilation_max = 0.5;
    REQUIRE_THROWS_AS(stress::run_stress(config), std::invalid_argument);

    config = test_config();
    config.time_dilation_gain = -0.1;
    REQUIRE_THROWS_AS(stress::run_stress(config), std::invalid_argument);
}

TEST_CASE("ball stress runs multiple clients through replication and ACKs") {
    stress::StressConfig config = test_config();
    config.clients = 3;
    config.duration_seconds = 0.5;
    config.tick_rate = 10.0;
    config.spawn_interval_ms = 1.0;
    config.max_balls = 4;
    config.latency_ms = 0.0;
    config.loss_percent = 0.0;

    const stress::StressReport report = stress::run_stress(config);

    REQUIRE(report.config.clients == 3);
    REQUIRE(report.spawned > 0);
    REQUIRE(report.server_to_clients.server_update_packets >= report.config.clients);
    REQUIRE(report.clients_to_server.client_ack_packets >= report.config.clients);
    REQUIRE(report.memory.client_pending_acks == 0);
    REQUIRE(report.memory.client_local_entities >= report.config.clients);
}

TEST_CASE("ball stress wire diagnostics are opt-in report fields") {
    stress::StressConfig config = test_config();
    config.clients = 2;
    config.duration_seconds = 0.5;
    config.tick_rate = 10.0;
    config.spawn_interval_ms = 1.0;
    config.max_balls = 4;
    config.wire_diagnostics = true;

    const stress::StressReport report = stress::run_stress(config);

    REQUIRE(report.config.wire_diagnostics);
    REQUIRE(report.server_to_clients.wire.packet_bits > 0);
    REQUIRE(report.server_to_clients.wire.server_update_header_bits > 0);
    REQUIRE(report.server_to_clients.wire.full_upsert_bits > 0);
    REQUIRE(report.clients_to_server.wire.ack_records > 0);
    REQUIRE(report.clients_to_server.wire.ack_record_bits > 0);

    std::ostringstream json;
    stress::write_report_json(json, report);
    REQUIRE(json.str().find("\"wire_format\"") != std::string::npos);

    std::ostringstream text;
    stress::write_report_text(text, report);
    REQUIRE(text.str().find("wire_format server_to_clients") != std::string::npos);

    config.wire_diagnostics = false;
    const stress::StressReport default_report = stress::run_stress(config);
    std::ostringstream default_json;
    stress::write_report_json(default_json, default_report);
    REQUIRE(default_json.str().find("\"wire_format\"") == std::string::npos);
}

TEST_CASE("ball stress runs with buffered interpolation clients") {
    stress::StressConfig config = test_config();
    config.client_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    config.interpolation_buffer_frames = 2;
    config.interpolation_buffer_capacity_frames = 8;

    const stress::StressReport report = stress::run_stress(config);

    REQUIRE(report.config.client_mode == kage::sync::ReplicationClientMode::BufferedInterpolation);
    REQUIRE(report.memory.client_pending_acks == 0);
    REQUIRE(report.server_to_clients.server_update_packets >= 1);
    REQUIRE(report.client_timing.sample_count >= 1);
    REQUIRE(report.client_timing.max_current_interpolation_buffer_frames < config.interpolation_buffer_capacity_frames);
    REQUIRE(report.client_timing.average_time_dilation >= config.time_dilation_min);
    REQUIRE(report.client_timing.average_time_dilation <= config.time_dilation_max);
}

TEST_CASE("ball stress buffered clients report accumulator timing under jitter") {
    stress::StressConfig config = test_config();
    config.client_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    config.duration_seconds = 2.0;
    config.tick_rate = 30.0;
    config.spawn_interval_ms = 2.0;
    config.max_balls = 64;
    config.latency_ms = 50.0;
    config.jitter_ms = 25.0;
    config.interpolation_buffer_frames = 1;
    config.interpolation_buffer_capacity_frames = 16;
    config.time_dilation_min = 0.50;
    config.time_dilation_max = 1.50;
    config.time_dilation_gain = 0.50;

    const stress::StressReport report = stress::run_stress(config);

    REQUIRE(report.client_timing.sample_count > 0);
    REQUIRE(report.client_timing.max_desired_interpolation_buffer_frames > 0);
    REQUIRE(report.client_timing.max_current_interpolation_buffer_frames < config.interpolation_buffer_capacity_frames);
    REQUIRE(report.client_timing.average_time_dilation >= config.time_dilation_min);
    REQUIRE(report.client_timing.average_time_dilation <= config.time_dilation_max);
    REQUIRE(report.client_timing.average_measured_interpolation_buffer_frames >= 0.0);
    REQUIRE(report.client_timing.average_jitter_frames >= 0.0);
}
