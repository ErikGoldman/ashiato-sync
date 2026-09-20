#include "stress_fault_link.hpp"
#include "test_components.hpp"
#include "test_protocol.hpp"

#include "client/store/input_buffer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

namespace {

constexpr ashiato::sync::ClientId full_client = 1;
constexpr ashiato::sync::ClientId position_only_client = 2;
constexpr ashiato::sync::ClientId lossy_client = 3;
constexpr std::uint64_t position_component_mask = std::uint64_t{1U};

struct ServerEntity {
    ashiato::Entity entity;
    std::uint32_t serial = 0;
    std::uint32_t updates = 0;
};

struct StressClient {
    explicit StressClient(
        ashiato::sync::ClientId client_id,
        ashiato::sync::SyncArchetypeId expected_archetype)
        : id(client_id) {
        const ashiato::Entity position =
            ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
        const ashiato::Entity health = ashiato::sync::register_sync_component<Health>(registry, "Health");
        const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
            registry,
            "StatefulStressActor",
            {
                {position, ashiato::sync::ReplicationAudience::All},
                {health, ashiato::sync::ReplicationAudience::All},
            });
        REQUIRE(archetype == expected_archetype);
        REQUIRE(configure_test_client_registry(registry, id));
        client = std::make_unique<ashiato::sync::ReplicationClient>(
            registry,
            make_test_client_options(registry, {}));
    }

    ashiato::sync::ClientId id;
    ashiato::Registry registry;
    std::unique_ptr<ashiato::sync::ReplicationClient> client;
};

ashiato::sync::SyncComponentOps byte_input_component_ops() {
    ashiato::sync::SyncComponentOps ops;
    ops.serialization.quantized_size = 1U;
    ops.serialization.quantize = [](const void* input, std::uint8_t* out) {
        out[0] = *static_cast<const std::uint8_t*>(input);
    };
    ops.serialization.serialize = [](
        const std::uint8_t*,
        const std::uint8_t* current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext&) {
        out.write_bits(current[0], 8U);
    };
    ops.serialization.push_to_registry = [](ashiato::Registry&, ashiato::Entity, const std::uint8_t*) {
        return true;
    };
    return ops;
}

void record_byte_inputs(
    ashiato::sync::client_detail::ClientInputBuffer& buffer,
    ashiato::Registry& registry,
    const ashiato::sync::SyncSettings& settings,
    ashiato::sync::SyncFrame frame_count) {
    for (ashiato::sync::SyncFrame frame = 1U; frame <= frame_count; ++frame) {
        const std::uint8_t input = static_cast<std::uint8_t>(frame);
        REQUIRE(buffer.set_latest(registry, settings, settings.input_component, &input));
        REQUIRE(buffer.record_frame(settings, 64U, frame, nullptr));
    }
}

class ReplicationChaosHarness {
public:
    explicit ReplicationChaosHarness(std::uint32_t seed, bool use_all_clients = true)
        : rng_(seed) {
        const ashiato::Entity position =
            ashiato::sync::register_sync_component<NetworkedPosition>(server_registry_, "NetworkedPosition");
        const ashiato::Entity health =
            ashiato::sync::register_sync_component<Health>(server_registry_, "Health");
        archetype_ = ashiato::sync::define_archetype(
            server_registry_,
            "StatefulStressActor",
            {
                {position, ashiato::sync::ReplicationAudience::All},
                {health, ashiato::sync::ReplicationAudience::All},
            });
        REQUIRE(configure_test_server_registry(server_registry_));

        ashiato::sync::ReplicationServerOptions options;
        options.bandwidth_limit_bytes_per_tick = 72U;
        options.mtu_bytes = 72U;
        options.entity_replication_decision_interval_frames = 1U;
        options.entity_replication_decider = [](ashiato::sync::ClientId client, ashiato::sync::EntityReplicationDecisionContext) {
            ashiato::sync::EntityReplicationDecision decision;
            if (client == position_only_client) {
                decision.component_mask = position_component_mask;
            }
            return decision;
        };
        options.transport = [this](ashiato::sync::PeerId peer, const ashiato::BitBuffer& packet) {
            const auto client = static_cast<ashiato::sync::ClientId>(peer);
            downstream_.enqueue(client, packet, tick_, downstream_fault(client));
        };
        server_ = std::make_unique<ashiato::sync::ReplicationServer>(server_registry_, options);

        const std::vector<ashiato::sync::ClientId> client_ids = use_all_clients
            ? std::vector<ashiato::sync::ClientId>{full_client, position_only_client, lossy_client}
            : std::vector<ashiato::sync::ClientId>{full_client};
        for (const ashiato::sync::ClientId id : client_ids) {
            clients_.push_back(std::make_unique<StressClient>(id, archetype_));
            REQUIRE(server_->add_client(id));
        }
        spawn_until(12U);
    }

    void run_generated_tick() {
        deliver_downstream();
        tick_clients();
        deliver_upstream();
        mutate_world();
        REQUIRE(server_->tick(server_registry_, server_->options().fixed_dt_seconds));
        if (clients_.size() > 1U) {
            require_secret_never_leaks();
        }
        ++tick_;
    }

    void converge(std::size_t drain_ticks = 512U) {
        faults_enabled_ = false;
        drain_network(drain_ticks);
    }

    void mutate_all_then_converge(std::size_t drain_ticks = 512U) {
        for (ServerEntity& tracked : entities_) {
            NetworkedPosition& position = server_registry_.write<NetworkedPosition>(tracked.entity);
            position.x += 0.1f;
            position.y += 0.1f;
            server_registry_.write<Health>(tracked.entity).value += 1;
        }
        faults_enabled_ = false;
        drain_network(drain_ticks);
    }

    void require_converged() {
        for (const std::unique_ptr<StressClient>& node : clients_) {
            CAPTURE(node->id, tick_);
            for (const ServerEntity& tracked : entities_) {
                CAPTURE(tracked.serial);
                const ashiato::sync::ClientEntityNetworkId network_id =
                    server_->client_entity_network_id(node->id, tracked.entity);
                REQUIRE(network_id != ashiato::sync::invalid_client_entity_network_id);
                const ashiato::Entity local = node->client->local_entity(network_id);
                REQUIRE(local);
                const NetworkedPosition& expected = server_registry_.get<NetworkedPosition>(tracked.entity);
                const NetworkedPosition& actual = node->registry.get<NetworkedPosition>(local);
                CHECK(actual.x == Catch::Approx(expected.x).margin(0.11f));
                CHECK(actual.y == Catch::Approx(expected.y).margin(0.11f));
                if (node->id == position_only_client) {
                    CHECK_FALSE(node->registry.contains<Health>(local));
                } else {
                    REQUIRE(node->registry.contains<Health>(local));
                    CHECK(node->registry.get<Health>(local).value ==
                          server_registry_.get<Health>(tracked.entity).value);
                }
            }
            CHECK(node->client->pending_ack_count() <= ashiato::sync::protocol::baseline_retention_count);
        }
        CHECK(server_->retained_quantized_frame_count() <= entities_.size() * clients_.size() * 2U);
    }

    void require_health_converged(std::uint32_t serial) {
        const auto tracked = std::find_if(entities_.begin(), entities_.end(), [serial](const ServerEntity& entity) {
            return entity.serial == serial;
        });
        REQUIRE(tracked != entities_.end());
        StressClient& node = client(full_client);
        const ashiato::sync::ClientEntityNetworkId network_id =
            server_->client_entity_network_id(full_client, tracked->entity);
        REQUIRE(network_id != ashiato::sync::invalid_client_entity_network_id);
        const ashiato::Entity local = node.client->local_entity(network_id);
        REQUIRE(local);
        REQUIRE(node.registry.contains<Health>(local));
        CHECK(node.registry.get<Health>(local).value == server_registry_.get<Health>(tracked->entity).value);
    }

private:
    void drain_network(std::size_t drain_ticks) {
        for (std::size_t drain = 0; drain < drain_ticks; ++drain) {
            deliver_downstream();
            tick_clients();
            deliver_upstream();
            REQUIRE(server_->tick(server_registry_, server_->options().fixed_dt_seconds));
            ++tick_;
        }
        deliver_downstream();
        tick_clients();
        deliver_upstream();
    }

    PacketFault downstream_fault(ashiato::sync::ClientId client) {
        if (!faults_enabled_) {
            return {};
        }
        PacketFault fault;
        fault.delay_ticks = static_cast<std::uint32_t>((tick_ + client * 3U) % 9U);
        fault.drop = client == lossy_client && tick_ % 7U < 3U;
        fault.duplicate_count = tick_ % 29U == 0U ? 1U : 0U;
        return fault;
    }

    PacketFault upstream_fault(ashiato::sync::ClientId client) const {
        if (!faults_enabled_) {
            return {};
        }
        PacketFault fault;
        fault.delay_ticks = static_cast<std::uint32_t>((tick_ * 5U + client) % 11U);
        fault.drop = (client == full_client && tick_ % 13U < 5U) ||
            (client == lossy_client && tick_ % 5U == 0U);
        fault.duplicate_count = tick_ % 31U == 0U ? 1U : 0U;
        return fault;
    }

    void deliver_downstream() {
        downstream_.deliver_ready(tick_, [&](ashiato::sync::ClientId id, const ashiato::BitBuffer& packet, std::uint64_t) {
            StressClient& node = client(id);
            (void)node.client->receive(node.registry, packet);
        });
    }

    void tick_clients() {
        for (const std::unique_ptr<StressClient>& node : clients_) {
            REQUIRE(node->client->tick(node->registry, node->client->fixed_dt_seconds()));
            for (const ashiato::BitBuffer& packet : node->client->drain_packets()) {
                upstream_.enqueue(node->id, packet, tick_, upstream_fault(node->id));
            }
        }
    }

    void deliver_upstream() {
        upstream_.deliver_ready(tick_, [&](ashiato::sync::ClientId id, const ashiato::BitBuffer& packet, std::uint64_t) {
            (void)server_->process_packet(server_registry_, id, packet);
        });
    }

    StressClient& client(ashiato::sync::ClientId id) {
        const auto found = std::find_if(clients_.begin(), clients_.end(), [id](const std::unique_ptr<StressClient>& node) {
            return node->id == id;
        });
        REQUIRE(found != clients_.end());
        return **found;
    }

    void spawn_until(std::size_t count) {
        while (entities_.size() < count) {
            const std::uint32_t serial = next_serial_++;
            const ashiato::Entity entity = server_registry_.create();
            REQUIRE(server_registry_.add<NetworkedPosition>(
                        entity,
                        NetworkedPosition{}) != nullptr);
            REQUIRE(server_registry_.add<Health>(entity, Health{100 + static_cast<std::int32_t>(serial)}) != nullptr);
            REQUIRE(start_sync(server_registry_, entity, archetype_));
            entities_.push_back(ServerEntity{entity, serial, 0U});
        }
    }

    void mutate_world() {
        std::uniform_int_distribution<std::size_t> select(0U, entities_.size() - 1U);
        for (std::size_t change = 0; change < 4U; ++change) {
            ServerEntity& tracked = entities_[select(rng_)];
            NetworkedPosition& position = server_registry_.write<NetworkedPosition>(tracked.entity);
            ++tracked.updates;
            const float phase = static_cast<float>(tracked.updates) / 20.0f;
            position.x = phase;
            position.y = phase;
            if ((tick_ + change) % 3U == 0U) {
                server_registry_.write<Health>(tracked.entity).value -= 1;
            }
        }

        if (tick_ != 0U && tick_ % 37U == 0U) {
            const std::size_t index = select(rng_);
            REQUIRE(server_registry_.destroy(entities_[index].entity));
            entities_.erase(entities_.begin() + static_cast<std::ptrdiff_t>(index));
            spawn_until(12U);
        }
    }

    void require_secret_never_leaks() {
        StressClient& hidden = client(position_only_client);
        for (const ServerEntity& tracked : entities_) {
            const auto network_id = server_->client_entity_network_id(position_only_client, tracked.entity);
            if (network_id == ashiato::sync::invalid_client_entity_network_id) {
                continue;
            }
            const ashiato::Entity local = hidden.client->local_entity(network_id);
            if (local) {
                CHECK_FALSE(hidden.registry.contains<Health>(local));
            }
        }
    }

    std::mt19937 rng_;
    std::uint64_t tick_ = 0;
    std::uint32_t next_serial_ = 1;
    bool faults_enabled_ = true;
    ashiato::Registry server_registry_;
    ashiato::sync::SyncArchetypeId archetype_;
    std::unique_ptr<ashiato::sync::ReplicationServer> server_;
    std::vector<std::unique_ptr<StressClient>> clients_;
    std::vector<ServerEntity> entities_;
    PacketFaultLink<ashiato::sync::ClientId> downstream_;
    PacketFaultLink<ashiato::sync::ClientId> upstream_;
};

std::size_t stress_seed_count() {
    constexpr std::size_t default_seed_count = 4U;
    const char* configured = std::getenv("ASHIATO_SYNC_STRESS_SEED_COUNT");
    if (configured == nullptr) {
        return default_seed_count;
    }
    const unsigned long parsed = std::strtoul(configured, nullptr, 10);
    return parsed == 0U ? default_seed_count : static_cast<std::size_t>(parsed);
}

std::uint32_t stress_seed(std::size_t index) {
    constexpr std::array<std::uint32_t, 4> fixed{1U, 0xC0FFEEU, 0xA11CEU, 0xDEADBEEFU};
    if (index < fixed.size()) {
        return fixed[index];
    }
    return 0x9E3779B9U * static_cast<std::uint32_t>(index + 1U) + 0x7F4A7C15U;
}

}  // namespace

TEST_CASE("stateful multi-client replication converges after loss reordering duplication and lifecycle churn", "[.stress][stateful]") {
    for (std::size_t index = 0; index < stress_seed_count(); ++index) {
        const std::uint32_t seed = stress_seed(index);
        DYNAMIC_SECTION("seed=" << seed) {
            ReplicationChaosHarness harness(seed);
            for (std::size_t tick = 0; tick < 400U; ++tick) {
                harness.run_generated_tick();
            }
            harness.converge();
            harness.require_converged();
        }
    }
}

TEST_CASE("client eventually receives a budget-deferred component mutation after ACK loss", "[.stress][stateful]") {
    ReplicationChaosHarness harness(1U, false);
    for (std::size_t tick = 0; tick < 9U; ++tick) {
        harness.run_generated_tick();
    }
    harness.mutate_all_then_converge(64U);
    harness.require_health_converged(7U);
}

TEST_CASE("fault link scripts drop delay duplicate corrupt and replay", "[.stress][fault-link]") {
    PacketFaultLink<std::uint32_t> link;
    ashiato::BitBuffer packet;
    packet.write_bits(0b101101U, 6U);

    const std::uint64_t dropped = link.enqueue(1U, packet, 0U, PacketFault{0U, 0U, 0U, true});
    REQUIRE(link.queued_count() == 0U);
    REQUIRE(link.dropped_count() == 1U);
    REQUIRE(link.replay(dropped, 1U));

    const std::uint64_t duplicated = link.enqueue(2U, packet, 0U, PacketFault{3U, 1U, 2U, false});
    REQUIRE(duplicated != dropped);
    REQUIRE(link.queued_count() == 3U);

    std::vector<std::pair<std::uint32_t, std::uint64_t>> delivered;
    link.deliver_ready(1U, [&](std::uint32_t endpoint, ashiato::BitBuffer payload, std::uint64_t sequence) {
        delivered.push_back({endpoint, sequence});
        CHECK(payload.read_bits(6U) == 0b101101U);
    });
    REQUIRE(delivered.size() == 1U);

    link.deliver_ready(3U, [&](std::uint32_t endpoint, ashiato::BitBuffer payload, std::uint64_t sequence) {
        delivered.push_back({endpoint, sequence});
        CHECK(payload.read_bits(6U) == 0b101001U);
    });
    REQUIRE(delivered.size() == 3U);
    REQUIRE(link.delivered_count() == 3U);
}

TEST_CASE("reused entity identities reject every stale lifecycle packet", "[.stress][lifecycle]") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    std::vector<ashiato::BitBuffer> server_packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::PeerId, const ashiato::BitBuffer& packet) {
        server_packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1U));

    ashiato::Registry client_registry;
    REQUIRE(define_position_archetype(client_registry) == server_archetype);
    REQUIRE(configure_test_client_registry(client_registry, 1U));
    ashiato::sync::ReplicationClient client(
        client_registry,
        make_test_client_options(client_registry, {}));

    auto acknowledge = [&]() {
        for (const ashiato::BitBuffer& packet : client.drain_ack_packets()) {
            REQUIRE(server.process_packet(server_registry, 1U, packet));
        }
    };
    auto tick_one_packet = [&]() {
        server_packets.clear();
        REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
        REQUIRE(server_packets.size() == 1U);
        return server_packets.front();
    };

    std::vector<ashiato::BitBuffer> stale_packets;
    std::uint32_t reused_wire_id = 0U;
    for (std::uint32_t generation = 1U; generation <= 64U; ++generation) {
        CAPTURE(generation);
        const ashiato::Entity entity = server_registry.create();
        REQUIRE(server_registry.add<Position>(
                    entity,
                    Position{static_cast<float>(generation), static_cast<float>(generation)}) != nullptr);
        REQUIRE(start_sync(server_registry, entity, server_archetype));

        const ashiato::BitBuffer full = tick_one_packet();
        const ClientUpdatePacket update = read_update(full);
        REQUIRE(update.records.size() == 1U);
        if (reused_wire_id == 0U) {
            reused_wire_id = update.records.front().network_id;
        }
        REQUIRE(update.records.front().network_id == reused_wire_id);
        REQUIRE(client.receive(client_registry, full));
        acknowledge();

        const ashiato::sync::ClientEntityNetworkId current_id =
            server.client_entity_network_id(1U, entity);
        REQUIRE(ashiato::sync::client_entity_network_id_version(current_id) == generation);
        const ashiato::Entity local = client.local_entity(current_id);
        REQUIRE(local);
        for (const ashiato::BitBuffer& stale : stale_packets) {
            REQUIRE_FALSE(client.receive(client_registry, stale));
            REQUIRE(client.local_entity(current_id) == local);
            REQUIRE(client_registry.get<Position>(local).x == static_cast<float>(generation));
        }
        REQUIRE(client.pending_ack_count() == 0U);

        REQUIRE(server_registry.destroy(entity));
        const ashiato::BitBuffer destroy = tick_one_packet();
        REQUIRE(client.receive(client_registry, destroy));
        acknowledge();
        REQUIRE_FALSE(client_registry.alive(local));
        stale_packets.push_back(full);
        stale_packets.push_back(destroy);
    }
}

TEST_CASE("input freshness survives protocol and ring capacity boundaries", "[.stress][input]") {
    constexpr std::array<ashiato::sync::SyncFrame, 5> frame_counts{31U, 32U, 63U, 64U, 65U};
    for (const ashiato::sync::SyncFrame frame_count : frame_counts) {
        DYNAMIC_SECTION("unacknowledged_frames=" << frame_count) {
            ashiato::Registry registry;
            ashiato::sync::SyncSettings settings;
            settings.input_component = ashiato::Entity{11U};
            settings.component_ops.emplace(settings.input_component.value, byte_input_component_ops());
            ashiato::sync::client_detail::ClientInputBuffer buffer;
            record_byte_inputs(buffer, registry, settings, frame_count);
            std::vector<std::uint32_t> pending_acks;
            std::vector<ashiato::BitBuffer> packets;
            REQUIRE(buffer.append_input_packet(
                24U,
                ashiato::sync::protocol::server_packet_id_bits,
                pending_acks,
                packets,
                nullptr));
            REQUIRE_FALSE(packets.empty());
            const auto input_packet = std::find_if(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
                return static_cast<std::uint8_t>(
                           packet.read_bits(ashiato::sync::protocol::message_bits)) ==
                    ashiato::sync::protocol::client_input_message;
            });
            REQUIRE(input_packet != packets.end());
            const ClientInputPacket decoded = read_client_input_header(*input_packet);
            REQUIRE(decoded.input_count > 0U);
            CHECK(decoded.first_input_frame + decoded.input_count - 1U == frame_count);
            for (const ashiato::BitBuffer& packet : packets) {
                CHECK(ashiato::sync::protocol::bytes_for_bits(packet.bit_size()) <= 24U);
            }
        }
    }
}
