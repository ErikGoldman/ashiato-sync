// An entity that changed once, on a link whose ACKs arrive a few ticks late.
//
// After sending a record the scheduler moved the entity's dirty frame to the current frame, and an
// ACK retires the entity only once the ACKed frame reaches its dirty frame. Every ACK that arrives
// over a link with latency is for an older send, so it never did, and the entity was sent a record
// every tick for as long as it lived. Found by the cockpit game: a parked aircraft nobody had
// touched for minutes cost each client 4 bytes a tick at a 5-tick link.

#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace ashiato_sync_tests;

TEST_CASE("an entity written once is not sent for ever when its ACKs arrive a few ticks late") {
    // THE LOOP. On sending a record the scheduler sets the entity's dirty frame to the current frame, and an ACK only
    // retires the entity when the ACKed frame reaches its dirty frame. Over a link with any latency the ACK that arrives
    // is always for an older send, so an entity that was dirty once was sent a record -- with nothing in it -- every tick
    // for as long as it lived, whether or not anything wrote it again. Found in cockpit at a 5-tick link: a parked
    // aircraft nobody had touched for minutes cost each client 4 bytes a tick.
    constexpr std::size_t ack_delay_ticks = 5;
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry, "LateAckProbe", {{position_component, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 1.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<std::vector<std::uint32_t>> acks_due(200);
    std::size_t records_after_change = 0;
    std::size_t records_late = 0;
    for (std::size_t tick = 0; tick < 120; ++tick) {
        if (tick == 10) {
            // ONE REAL CHANGE, after the first record has been ACKed; nothing writes the entity again.
            registry.write<NetworkedPosition>(entity) = NetworkedPosition{2.0f, 1.0f};
        }
        for (const std::uint32_t packet_id : acks_due[tick]) {
            (void)server.process_packet(registry, 1, write_ack_packet(packet_id));
        }
        payloads.clear();
        server.tick(registry, server.options().fixed_dt_seconds);
        for (const ashiato::BitBuffer& payload : payloads) {
            const ServerUpdatePacket update = read_server_update(payload);
            if (update.message != ashiato::sync::protocol::server_update_message) {
                continue;
            }
            acks_due[tick + ack_delay_ticks].push_back(update.packet_id);
            if (tick >= 10 && tick < 20) {
                records_after_change += update.entities.size();
            }
            if (tick >= 60) {
                records_late += update.entities.size();
            }
        }
    }
    // THE CONTROL: the change itself was sent.
    REQUIRE(records_after_change >= 1);
    // Fifty ticks after the change, with every record ACKed five ticks late, nothing is owed.
    REQUIRE(records_late == 0);
}
