#include "client/store/input_buffer.hpp"
#include "server/input_buffer.hpp"
#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace {

struct ContextProbe {
    std::int32_t value = 0;
};

struct QuantizedContextProbe {
    std::int32_t value = 0;
};

struct ContextObservation {
    ashiato::sync::SyncFrame current_frame = 0;
    ashiato::sync::SyncFrame previous_frame = 0;
    bool previous = false;
};

std::vector<ContextObservation> component_serializes;
std::vector<ContextObservation> component_deserializes;
std::vector<ContextObservation> input_serializes;
std::vector<ContextObservation> input_deserializes;

void reset_observations() {
    component_serializes.clear();
    component_deserializes.clear();
    input_serializes.clear();
    input_deserializes.clear();
}

ashiato::sync::SyncComponentOps context_input_ops() {
    ashiato::sync::SyncComponentOps ops;
    ops.serialization.quantized_size = 1U;
    ops.serialization.quantize = [](const void* input, std::uint8_t* out) {
        out[0] = *static_cast<const std::uint8_t*>(input);
    };
    ops.serialization.serialize = [](
        const std::uint8_t* previous,
        const std::uint8_t* current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        input_serializes.push_back(ContextObservation{
            context.currentFrame,
            context.previousFrame,
            previous != nullptr,
        });
        out.write_bits(current[0], 8U);
    };
    ops.serialization.deserialize = [](
        ashiato::BitBuffer& packet,
        const std::uint8_t* previous,
        std::uint8_t* out,
        ashiato::ComponentSerializationContext& context) {
        input_deserializes.push_back(ContextObservation{
            context.currentFrame,
            context.previousFrame,
            previous != nullptr,
        });
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        out[0] = static_cast<std::uint8_t>(packet.read_bits(8U));
        return true;
    };
    ops.serialization.push_to_registry = [](ashiato::Registry&, ashiato::Entity, const std::uint8_t*) {
        return true;
    };
    return ops;
}

}  // namespace

namespace ashiato::sync {

template <>
struct SyncComponentTraits<ContextProbe> {
    using Quantized = QuantizedContextProbe;

    static Quantized quantize(const ContextProbe& value) {
        return Quantized{value.value};
    }

    static ContextProbe dequantize(const Quantized& value) {
        return ContextProbe{value.value};
    }

    static void serialize(
        const Quantized* previous,
        const Quantized& current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        component_serializes.push_back(ContextObservation{
            context.currentFrame,
            context.previousFrame,
            previous != nullptr,
        });
        out.write_bool(previous != nullptr);
        out.write_bits(previous != nullptr ? current.value - previous->value : current.value, 8U);
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        const Quantized* previous,
        Quantized& out,
        ashiato::ComponentSerializationContext& context) {
        component_deserializes.push_back(ContextObservation{
            context.currentFrame,
            context.previousFrame,
            previous != nullptr,
        });
        if (in.remaining_bits() < 9U) {
            return false;
        }
        const bool delta = in.read_bool();
        const auto value = static_cast<std::int32_t>(in.read_bits(8U));
        if (!delta) {
            out.value = value;
            return true;
        }
        if (previous == nullptr) {
            return false;
        }
        out.value = previous->value + value;
        return true;
    }
};

}  // namespace ashiato::sync

namespace {

ashiato::sync::SyncArchetypeId define_context_probe_archetype(ashiato::Registry& registry) {
    const ashiato::Entity component =
        ashiato::sync::register_sync_component<ContextProbe>(registry, "ContextProbe");
    return ashiato::sync::define_archetype(
        registry,
        "ContextProbeActor",
        {{component, ashiato::sync::ReplicationAudience::All}});
}

}  // namespace

TEST_CASE("component serialization context carries server and baseline frames") {
    reset_observations();

    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_context_probe_archetype(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<ContextProbe>(server_entity, ContextProbe{10}) != nullptr);
    REQUIRE(ashiato_sync_tests::start_sync(server_registry, server_entity, server_archetype));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_context_probe_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClient client(
        client_registry,
        ashiato_sync_tests::make_test_client_options(client_registry, {}));

    REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
    REQUIRE(packets.size() == 1);
    const ashiato_sync_tests::ServerUpdatePacket full_update =
        ashiato_sync_tests::read_server_update(packets.back(), 2U, 9U);
    REQUIRE(full_update.entities.size() == 1);
    REQUIRE(full_update.entities[0].full);
    REQUIRE(component_serializes.size() == 1);
    REQUIRE(component_serializes[0].current_frame == full_update.frame);
    REQUIRE(component_serializes[0].previous_frame == 0U);
    REQUIRE_FALSE(component_serializes[0].previous);

    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(component_deserializes.size() == 1);
    REQUIRE(component_deserializes[0].current_frame == full_update.frame);
    REQUIRE(component_deserializes[0].previous_frame == 0U);
    REQUIRE_FALSE(component_deserializes[0].previous);

    REQUIRE(server.process_packet(server_registry, 1, ashiato_sync_tests::write_ack_packet(full_update.packet_id)));
    packets.clear();
    server_registry.write<ContextProbe>(server_entity).value = 15;
    REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
    REQUIRE(packets.size() == 1);
    const ashiato_sync_tests::ServerUpdatePacket delta_update =
        ashiato_sync_tests::read_server_update(packets.back(), 2U, 9U);
    REQUIRE(delta_update.entities.size() == 1);
    REQUIRE_FALSE(delta_update.entities[0].full);
    REQUIRE(delta_update.entities[0].baseline_frame == full_update.frame);

    REQUIRE(component_serializes.size() == 2);
    REQUIRE(component_serializes[1].current_frame == delta_update.frame);
    REQUIRE(component_serializes[1].previous_frame == full_update.frame);
    REQUIRE(component_serializes[1].previous);

    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(component_deserializes.size() == 2);
    REQUIRE(component_deserializes[1].current_frame == delta_update.frame);
    REQUIRE(component_deserializes[1].previous_frame == full_update.frame);
    REQUIRE(component_deserializes[1].previous);
}

TEST_CASE("client and server input serialization contexts carry input frame numbers") {
    reset_observations();

    ashiato::sync::SyncSettings settings;
    settings.input_component = ashiato::Entity{11};
    settings.component_ops.emplace(settings.input_component.value, context_input_ops());

    ashiato::Registry registry;
    ashiato::sync::client_detail::ClientInputBuffer client_buffer;
    std::uint8_t input = 10U;
    REQUIRE(client_buffer.set_latest(registry, settings, settings.input_component, &input));

    ashiato::sync::client_detail::ClientInputRecord recorded;
    REQUIRE(client_buffer.record_frame(settings, 8U, 3U, &recorded));
    input = 11U;
    REQUIRE(client_buffer.set_latest(registry, settings, settings.input_component, &input));
    REQUIRE(client_buffer.record_frame(settings, 8U, 4U, &recorded));

    std::vector<std::uint32_t> pending_acks;
    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::client_detail::ClientInputPacketTrace client_trace;
    REQUIRE(client_buffer.drain_packet(
        1200U,
        ashiato::sync::protocol::server_packet_id_bits,
        pending_acks,
        packets,
        &client_trace));
    REQUIRE(packets.size() == 1);
    REQUIRE(input_serializes.size() == 3);
    REQUIRE(input_serializes[0].current_frame == 3U);
    REQUIRE(input_serializes[0].previous_frame == 0U);
    REQUIRE_FALSE(input_serializes[0].previous);
    REQUIRE(input_serializes[1].current_frame == 3U);
    REQUIRE(input_serializes[1].previous_frame == 0U);
    REQUIRE_FALSE(input_serializes[1].previous);
    REQUIRE(input_serializes[2].current_frame == 4U);
    REQUIRE(input_serializes[2].previous_frame == 3U);
    REQUIRE(input_serializes[2].previous);

    ashiato_sync_tests::ClientInputPacket header = ashiato_sync_tests::read_client_input_header(packets[0]);
    REQUIRE(header.first_input_frame == 3U);
    REQUIRE(header.input_count == 2U);

    packets[0].reset_read();
    REQUIRE(packets[0].read_bits(ashiato::sync::protocol::message_bits) ==
            ashiato::sync::protocol::client_input_message);
    const auto ack_count = static_cast<std::uint16_t>(
        packets[0].read_bits(ashiato::sync::protocol::ack_count_bits));
    for (std::uint16_t index = 0; index < ack_count; ++index) {
        (void)packets[0].read_bits(ashiato::sync::protocol::server_packet_id_bits);
    }

    ashiato::sync::server_detail::ServerInputBuffer server_buffer;
    ashiato::sync::server_detail::ServerInputPacketTrace server_trace;
    REQUIRE(server_buffer.process_packet_payload(packets[0], context_input_ops(), 8U, &server_trace));
    REQUIRE(input_deserializes.size() == 2);
    REQUIRE(input_deserializes[0].current_frame == 3U);
    REQUIRE(input_deserializes[0].previous_frame == 0U);
    REQUIRE_FALSE(input_deserializes[0].previous);
    REQUIRE(input_deserializes[1].current_frame == 4U);
    REQUIRE(input_deserializes[1].previous_frame == 3U);
    REQUIRE(input_deserializes[1].previous);
}
