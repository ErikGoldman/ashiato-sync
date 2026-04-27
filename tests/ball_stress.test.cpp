#include "../benchmarks/ball_stress.hpp"

#include <catch2/catch_test_macros.hpp>

#include <random>
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
    REQUIRE(registry.get<stress::BallPoison>(balls[0].entity).remaining == 1);
    REQUIRE(report.poison_components_added == 1);
    REQUIRE(report.poison_ticks == 1);
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
    link.latency_ms = 100.0;
    link.loss_percent = 0.0;

    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ack_message, 8U);
    packet.push_bits(0, 16U);

    stress::enqueue_packet(link, stats, 1, packet, 1.0);
    REQUIRE(stats.packets == 1);
    REQUIRE(stats.client_ack_packets == 1);
    REQUIRE(stats.dropped_packets == 0);

    bool delivered = false;
    stress::deliver_ready(link, stats, 1.05, [&](kage::sync::ClientId, const kage::sync::BitBuffer&) {
        delivered = true;
    });
    REQUIRE_FALSE(delivered);

    stress::deliver_ready(link, stats, 1.10, [&](kage::sync::ClientId, const kage::sync::BitBuffer&) {
        delivered = true;
    });
    REQUIRE(delivered);
    REQUIRE(stats.delivered_packets == 1);

    stress::SimulatedLink lossy;
    lossy.loss_percent = 100.0;
    stress::DirectionStats loss_stats;
    stress::enqueue_packet(lossy, loss_stats, 1, packet, 0.0);
    REQUIRE(loss_stats.dropped_packets == 1);
    REQUIRE(lossy.queued.empty());
}

TEST_CASE("ball stress packet classifier counts update record kinds") {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(7, 32U);
    packet.push_bits(3, 16U);

    packet.push_bool(false);
    packet.push_unsigned_bits(100, 64U);
    packet.push_bool(true);
    packet.push_bits(1, 32U);
    packet.push_bits(0, 16U);

    packet.push_bool(false);
    packet.push_unsigned_bits(101, 64U);
    packet.push_bool(false);
    kage::sync::protocol::write_baseline_frame(packet, 7, 6);
    packet.push_bits(0, 16U);

    packet.push_bool(true);
    packet.push_unsigned_bits(102, 64U);

    const stress::PacketBreakdown breakdown = stress::classify_packet(packet);

    REQUIRE(breakdown.type == stress::PacketType::ServerUpdate);
    REQUIRE(breakdown.full_upserts == 1);
    REQUIRE(breakdown.delta_upserts == 1);
    REQUIRE(breakdown.destroys == 1);
}

TEST_CASE("ball stress shared latency and loss defaults apply to both directions") {
    stress::StressConfig config = test_config();
    config.duration_seconds = 0.1;
    config.tick_rate = 10.0;
    config.spawn_interval_ms = 1.0;
    config.max_balls = 2;
    config.latency_ms = 25.0;
    config.loss_percent = 100.0;

    const stress::StressReport report = stress::run_stress(config);

    REQUIRE(report.config.server_to_client_latency_ms == 25.0);
    REQUIRE(report.config.client_to_server_latency_ms == 25.0);
    REQUIRE(report.config.server_to_client_loss_percent == 100.0);
    REQUIRE(report.config.client_to_server_loss_percent == 100.0);
    REQUIRE(report.server_to_clients.dropped_packets == report.server_to_clients.packets);
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
