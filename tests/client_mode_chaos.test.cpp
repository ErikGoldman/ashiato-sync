#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

namespace {

constexpr ashiato::sync::ClientId test_client_id = 1;

ashiato::sync::ReplicationClientOptions mode_options(
    ashiato::sync::ReplicationClientMode mode,
    ashiato::sync::SyncFrame buffered_lag = 3U) {
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = mode;
    options.buffered.auto_buffered_frame_lag = false;
    options.buffered.buffered_frame_lag = buffered_lag;
    options.prediction.auto_lead_frames = false;
    options.prediction.lead_frames = 2U;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    return options;
}

ashiato::sync::SyncArchetypeId prepare_registry(ashiato::Registry& registry) {
    const ashiato::sync::SyncArchetypeId archetype = define_predicted_archetype(registry);
    REQUIRE(configure_test_client_registry(registry, test_client_id));
    return archetype;
}

ashiato::BitBuffer make_multi_entity_packet(
    ashiato::sync::SyncFrame frame,
    const std::vector<std::pair<ashiato::Entity, PredictedPosition>>& records) {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::server_update_message, ashiato::sync::protocol::message_bits);
    packet.write_bits(frame, 32U);
    packet.write_bits(frame, ashiato::sync::protocol::server_packet_id_bits);
    packet.write_bits(0U, 32U);
    packet.write_bits(static_cast<std::uint16_t>(records.size()), 16U);
    for (const auto& record : records) {
        packet.write_bool(false);
        ashiato::sync::protocol::write_network_entity_id(packet, test_network_id(record.first));
        packet.write_bool(true);
        packet.write_bits(0U, 32U);
        packet.write_bool(false);
        packet.write_bits(1U, 16U);
        packet.write_bits(1U, ashiato::sync::protocol::bits_for_range(2U));
        packet.write_bits(static_cast<std::int32_t>(record.second.x * 10.0f), 16U);
        packet.write_bits(static_cast<std::int32_t>(record.second.y * 10.0f), 16U);
        packet.write_bool(false);
    }
    return packet;
}

}  // namespace

TEST_CASE(
    "mixed client modes preserve identity under sustained loss reordering and transitions",
    "[.stress][mode-transition]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    ashiato::Registry registry;
    REQUIRE(prepare_registry(registry).value == 0U);
    ashiato::sync::ReplicationClient client(
        registry,
        make_test_client_options(registry, mode_options(Mode::Snap)));
    client.simulation_job<PredictedPosition>(registry, 0).each(
        [](ashiato::Entity, PredictedPosition& position) {
            position.x += 1.0f;
        });

    const std::array<ashiato::Entity, 4> server_entities{
        ashiato::Entity{40U},
        ashiato::Entity{41U},
        ashiato::Entity{42U},
        ashiato::Entity{43U}};
    const auto make_records = [&](ashiato::sync::SyncFrame frame) {
        std::vector<std::pair<ashiato::Entity, PredictedPosition>> records;
        for (std::size_t index = 0; index < server_entities.size(); ++index) {
            records.push_back({
                server_entities[index],
                PredictedPosition{static_cast<float>(frame * 10U + index), static_cast<float>(index)}});
        }
        return records;
    };

    REQUIRE(client.receive(registry, make_multi_entity_packet(1U, make_records(1U))));
    std::array<ashiato::Entity, 4> local_entities{};
    for (std::size_t index = 0; index < server_entities.size(); ++index) {
        local_entities[index] = client.local_entity(
            test_client_entity_network_id(test_client_id, server_entities[index]));
        REQUIRE(local_entities[index]);
    }

    std::vector<ashiato::BitBuffer> delayed;
    for (ashiato::sync::SyncFrame frame = 2U; frame <= 180U; ++frame) {
        ashiato::BitBuffer packet = make_multi_entity_packet(frame, make_records(frame));
        if (frame % 11U != 0U) {
            if (frame % 7U == 0U) {
                delayed.push_back(std::move(packet));
            } else {
                REQUIRE(client.receive(registry, std::move(packet)));
            }
        }
        if (frame % 5U == 0U && !delayed.empty()) {
            (void)client.receive(registry, std::move(delayed.back()));
            delayed.pop_back();
        }

        const std::size_t selected = static_cast<std::size_t>(frame % server_entities.size());
        const Mode mode = static_cast<Mode>(frame % 3U);
        client.set_entity_mode(
            registry,
            test_client_entity_network_id(test_client_id, server_entities[selected]),
            mode);
        REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

        for (std::size_t index = 0; index < server_entities.size(); ++index) {
            CAPTURE(frame, index, selected, mode);
            const auto network_id = test_client_entity_network_id(test_client_id, server_entities[index]);
            REQUIRE(client.local_entity(network_id) == local_entities[index]);
            REQUIRE(registry.alive(local_entities[index]));
            REQUIRE(registry.contains<PredictedPosition>(local_entities[index]));
            REQUIRE(registry.get<PredictedPosition>(local_entities[index]).y ==
                    Catch::Approx(static_cast<float>(index)));
        }
    }

    for (ashiato::BitBuffer& packet : delayed) {
        (void)client.receive(registry, std::move(packet));
    }
    constexpr ashiato::sync::SyncFrame recovery_frame = 200U;
    REQUIRE(client.receive(registry, make_multi_entity_packet(recovery_frame, make_records(recovery_frame))));
    for (std::size_t index = 0; index < server_entities.size(); ++index) {
        const auto network_id = test_client_entity_network_id(test_client_id, server_entities[index]);
        client.set_entity_mode(registry, network_id, Mode::Snap);
        REQUIRE(client.local_entity(network_id) == local_entities[index]);
        REQUIRE(registry.get<PredictedPosition>(local_entities[index]).x ==
                Catch::Approx(static_cast<float>(recovery_frame * 10U + index)));
    }
}
