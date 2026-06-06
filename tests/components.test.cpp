#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <type_traits>

using ashiato_sync_tests::Health;
using ashiato_sync_tests::NetworkedPayload;
using ashiato_sync_tests::NetworkedPosition;
using ashiato_sync_tests::Position;
using ashiato_sync_tests::Secret;
using ashiato_sync_tests::Visible;
using ashiato_sync_tests::read_networked_payload;

namespace {

struct ProfiledTransform {
    int x = 0;
    int y = 0;
};

struct ProfiledTransformXOnlySettings {
    static constexpr bool x_only = true;
};

}  // namespace

namespace ashiato::sync {

template <>
struct SyncComponentTraits<ProfiledTransform> {
    struct Settings {
        static constexpr bool x_only = false;
    };

    struct Quantized {
        std::int32_t x = 0;
        std::int32_t y = 0;
    };

	template <typename SettingsT = Settings>
	struct Serializer {
		static void quantize(const ProfiledTransform& value, Quantized& out) {
			out = Quantized{value.x, value.y};
		}

        static ProfiledTransform dequantize(const Quantized& value) {
            return ProfiledTransform{value.x, value.y};
        }

        static void serialize(
            const Quantized*,
            const Quantized& current,
            ashiato::BitBuffer& out,
            ashiato::ComponentSerializationContext&) {
            out.write_bits(current.x, 8U);
            if constexpr (!SettingsT::x_only) {
                out.write_bits(current.y, 8U);
            }
        }

        static bool deserialize(
            ashiato::BitBuffer& in,
            const Quantized*,
            Quantized& out,
            ashiato::ComponentSerializationContext&) {
            if (in.remaining_bits() < 8U) {
                return false;
            }
            out.x = static_cast<std::int32_t>(in.read_bits(8U));
            if constexpr (SettingsT::x_only) {
                out.y = 0;
                return true;
            } else {
                if (in.remaining_bits() < 8U) {
                    return false;
                }
                out.y = static_cast<std::int32_t>(in.read_bits(8U));
                return true;
            }
        }
    };
};

}  // namespace ashiato::sync

static_assert(
    std::is_nothrow_move_constructible<ashiato::sync::SyncSettings>::value,
    "SyncSettings must satisfy Ashiato component storage requirements");

TEST_CASE("sync components register into the Ashiato registry") {
    ashiato::Registry registry;

    ashiato::sync::register_components(registry);

    REQUIRE(registry.component<ashiato::sync::SyncSettings>());
    REQUIRE(registry.component<ashiato::sync::SyncAuthority>());
    REQUIRE(registry.component<ashiato::sync::Replicated>());
    REQUIRE(registry.component<ashiato::sync::NetworkOwner>());
    REQUIRE(registry.component<ashiato::sync::FractionalTickSampled>());

    const ashiato::sync::SyncSettings& settings = registry.get<ashiato::sync::SyncSettings>();
    REQUIRE(settings.role == ashiato::sync::SyncRole::Server);
    REQUIRE(settings.local_client == ashiato::sync::invalid_client_id);
    REQUIRE(settings.archetypes.empty());
    REQUIRE(registry.get<ashiato::sync::SyncAuthority>().is_authoritative());
}

TEST_CASE("sync singleton components expose debug fields") {
    ashiato::Registry registry;

    ashiato::sync::register_components(registry);

    const std::vector<ashiato::ComponentField>* settings_fields =
        registry.component_fields(registry.component<ashiato::sync::SyncSettings>());
    REQUIRE(settings_fields != nullptr);
    REQUIRE(settings_fields->size() == 4U);
    CHECK((*settings_fields)[0].name == "role");
    CHECK((*settings_fields)[0].type == registry.primitive_type(ashiato::PrimitiveType::I32));
    CHECK((*settings_fields)[1].name == "local_client");
    CHECK((*settings_fields)[1].type == registry.primitive_type(ashiato::PrimitiveType::U8));
    CHECK((*settings_fields)[2].name == "input_component.value");
    CHECK((*settings_fields)[2].type == registry.primitive_type(ashiato::PrimitiveType::U64));
    CHECK((*settings_fields)[3].name == "fixed_dt_seconds");
    CHECK((*settings_fields)[3].type == registry.primitive_type(ashiato::PrimitiveType::F64));

    const std::vector<ashiato::ComponentField>* frame_fields =
        registry.component_fields(registry.component<ashiato::sync::FrameInfo>());
    REQUIRE(frame_fields != nullptr);
    REQUIRE(frame_fields->size() == 1U);
    CHECK((*frame_fields)[0].name == "frame");
    CHECK((*frame_fields)[0].type == registry.primitive_type(ashiato::PrimitiveType::U32));

    const std::vector<ashiato::ComponentField>* authority_fields =
        registry.component_fields(registry.component<ashiato::sync::SyncAuthority>());
    REQUIRE(authority_fields != nullptr);
    REQUIRE(authority_fields->size() == 1U);
    CHECK((*authority_fields)[0].name == "authoritative");
    CHECK((*authority_fields)[0].type == registry.primitive_type(ashiato::PrimitiveType::Bool));
}

TEST_CASE("fractional tick sample marker is stored as a component entity tag") {
    ashiato::Registry registry;
    const ashiato::Entity smooth_component =
        ashiato::sync::register_sync_component<ashiato_sync_tests::SmoothPosition>(registry, "SmoothPosition");
    const ashiato::Entity position_component = ashiato::sync::register_sync_component<Position>(registry, "Position");

    REQUIRE(ashiato::sync::set_fractional_tick_sampled(registry, smooth_component));
    REQUIRE(ashiato::sync::is_fractional_tick_sampled(registry, smooth_component));
    REQUIRE(registry.has<ashiato::sync::FractionalTickSampled>(smooth_component));

    REQUIRE_FALSE(ashiato::sync::set_fractional_tick_sampled(registry, position_component));
    REQUIRE_FALSE(ashiato::sync::is_fractional_tick_sampled(registry, position_component));

    REQUIRE(ashiato::sync::set_fractional_tick_sampled<ashiato_sync_tests::SmoothPosition>(registry, false));
    REQUIRE_FALSE(ashiato::sync::is_fractional_tick_sampled<ashiato_sync_tests::SmoothPosition>(registry));
}

TEST_CASE("server and client configuration update singleton settings") {
    ashiato::Registry registry;

    ashiato_sync_tests::configure_test_client_registry(registry, 42);

    REQUIRE(registry.get<ashiato::sync::SyncSettings>().role == ashiato::sync::SyncRole::Client);
    REQUIRE(registry.get<ashiato::sync::SyncSettings>().local_client == 42);
    REQUIRE_FALSE(registry.get<ashiato::sync::SyncAuthority>().is_authoritative());

    ashiato_sync_tests::configure_test_server_registry(registry);

    REQUIRE(registry.get<ashiato::sync::SyncSettings>().role == ashiato::sync::SyncRole::Server);
    REQUIRE(registry.get<ashiato::sync::SyncSettings>().local_client == ashiato::sync::invalid_client_id);
    REQUIRE(registry.get<ashiato::sync::SyncAuthority>().is_authoritative());
}

TEST_CASE("sync archetypes store component replication settings in the singleton") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = ashiato::sync::register_sync_component<Position>(registry, "Position");
    const ashiato::Entity health_component = ashiato::sync::register_sync_component<Health>(registry, "Health");

    const ashiato::sync::SyncArchetypeId actor = ashiato::sync::define_archetype(
        registry,
        "Actor",
        {
            {position_component, ashiato::sync::ReplicationAudience::All},
            {health_component, ashiato::sync::ReplicationAudience::Owner},
        });

    const ashiato::sync::SyncArchetypeId projectile = ashiato::sync::define_archetype(
        registry,
        "Projectile",
        {{position_component, ashiato::sync::ReplicationAudience::All}});

    REQUIRE(actor.value == 0);
    REQUIRE(projectile.value == 1);

    const ashiato::sync::SyncArchetype* found_actor = ashiato::sync::find_archetype(registry, actor);
    REQUIRE(found_actor != nullptr);
    REQUIRE(found_actor->name == "Actor");
    REQUIRE(found_actor->components.size() == 2);
    REQUIRE(found_actor->components[0].component == position_component);
    REQUIRE(found_actor->components[0].audience == ashiato::sync::ReplicationAudience::All);
    REQUIRE(found_actor->components[1].component == health_component);
    REQUIRE(found_actor->components[1].audience == ashiato::sync::ReplicationAudience::Owner);

    REQUIRE(ashiato::sync::find_archetype(registry, ashiato::sync::SyncArchetypeId{99}) == nullptr);
}

TEST_CASE("sync archetypes store tag replication settings in reserved slot zero") {
    ashiato::Registry registry;
    const ashiato::Entity visible = registry.register_component<Visible>("Visible");
    const ashiato::Entity secret = registry.register_component<Secret>("Secret");
    const ashiato::Entity position_component = ashiato::sync::register_sync_component<Position>(registry, "Position");

    const ashiato::sync::SyncArchetypeId actor = ashiato::sync::define_archetype(
        registry,
        ashiato::sync::SyncArchetypeDesc{
            "TaggedActor",
            {
                {visible, ashiato::sync::ReplicationAudience::All},
                {secret, ashiato::sync::ReplicationAudience::Owner},
            },
            {{position_component, ashiato::sync::ReplicationAudience::All}},
        });

    const ashiato::sync::SyncArchetype* found = ashiato::sync::find_archetype(registry, actor);
    REQUIRE(found != nullptr);
    REQUIRE(found->tags.size() == 2);
    REQUIRE(found->tags[0].tag == visible);
    REQUIRE(found->tags[0].audience == ashiato::sync::ReplicationAudience::All);
    REQUIRE(found->tags[1].tag == secret);
    REQUIRE(found->tags[1].audience == ashiato::sync::ReplicationAudience::Owner);
    REQUIRE(found->components.size() == 1);
}

TEST_CASE("sync archetypes reject invalid tag declarations") {
    ashiato::Registry registry;
    const ashiato::Entity visible = registry.register_component<Visible>("Visible");
    const ashiato::Entity position = registry.register_component<Position>("Position");

    REQUIRE_THROWS_AS(
        ashiato::sync::define_archetype(
            registry,
            ashiato::sync::SyncArchetypeDesc{"Invalid", {{position, ashiato::sync::ReplicationAudience::All}}, {}}),
        std::invalid_argument);

    REQUIRE_THROWS_AS(
        ashiato::sync::define_archetype(
            registry,
            ashiato::sync::SyncArchetypeDesc{
                "Duplicate",
                {
                    {visible, ashiato::sync::ReplicationAudience::All},
                    {visible, ashiato::sync::ReplicationAudience::Owner},
                },
                {}}),
        std::invalid_argument);
}

TEST_CASE("sync archetypes reject unregistered component ids") {
    ashiato::Registry registry;

    REQUIRE_THROWS_AS(
        ashiato::sync::define_archetype(
            registry,
            "Invalid",
            {{ashiato::Entity{123}, ashiato::sync::ReplicationAudience::All}}),
        std::invalid_argument);
}

TEST_CASE("sync archetypes reject duplicate component declarations") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = ashiato::sync::register_sync_component<Position>(registry, "Position");

    REQUIRE_THROWS_AS(
        ashiato::sync::define_archetype(
            registry,
            "Duplicate",
            {
                {position_component, ashiato::sync::ReplicationAudience::All},
                {position_component, ashiato::sync::ReplicationAudience::Owner},
            }),
        std::invalid_argument);
}

TEST_CASE("sync archetypes reject components without sync traits") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = registry.register_component<Position>("Position");

    REQUIRE_THROWS_AS(
        ashiato::sync::define_archetype(
            registry,
            "Invalid",
            {{position_component, ashiato::sync::ReplicationAudience::All}}),
        std::invalid_argument);
}

TEST_CASE("sync component traits provide type-erased quantization and serialization ops") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncComponentOps* ops = ashiato::sync::find_component_ops(registry, position_component);
    REQUIRE(ops != nullptr);

    const NetworkedPosition position{1.0f, 2.0f};
    std::array<std::uint8_t, sizeof(ashiato_sync_tests::QuantizedNetworkedPosition)> quantized{};
    ops->serialization.quantize(&position, quantized.data());

    ashiato::BitBuffer payload;
    ashiato::ComponentSerializationContext serialization_context;
    ops->serialization.serialize(nullptr, quantized.data(), payload, serialization_context);
    const NetworkedPayload fields = read_networked_payload(payload);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 20);

    payload.reset_read();
    std::array<std::uint8_t, sizeof(ashiato_sync_tests::QuantizedNetworkedPosition)> decoded{};
    REQUIRE(ops->serialization.deserialize(payload, nullptr, decoded.data(), serialization_context));

    NetworkedPosition dequantized;
    ops->serialization.dequantize(decoded.data(), &dequantized);
    REQUIRE(dequantized.x == 1.0f);
    REQUIRE(dequantized.y == 2.0f);

    const ashiato::Entity entity = registry.create();
    REQUIRE(ops->serialization.push_to_registry(registry, entity, decoded.data()));
    REQUIRE(registry.contains<NetworkedPosition>(entity));
    REQUIRE(registry.remove(entity, position_component));
    REQUIRE_FALSE(registry.contains<NetworkedPosition>(entity));
}

TEST_CASE("sync component serializers support compile-time settings profiles per archetype") {
    ashiato::Registry registry;
    const ashiato::Entity transform =
        ashiato::sync::register_sync_component<ProfiledTransform>(registry, "ProfiledTransform");
    using TransformSync = ashiato::sync::SyncComponentTraits<ProfiledTransform>;
    using XOnlySerializer = TransformSync::Serializer<ProfiledTransformXOnlySettings>;
    const ashiato::sync::SyncComponentSerializerId x_only =
        ashiato::sync::register_sync_component_serializer<ProfiledTransform, XOnlySerializer>(
            registry,
            "ProfiledTransform.XOnly");

    const ashiato::sync::SyncArchetypeId full_archetype = ashiato::sync::define_archetype(
        registry,
        "Full",
        {ashiato::sync::replicate<ProfiledTransform>(registry)});
    const ashiato::sync::SyncArchetypeId x_only_archetype = ashiato::sync::define_archetype(
        registry,
        "XOnly",
        {ashiato::sync::replicate<ProfiledTransform>(registry, x_only)});

    const ashiato::sync::SyncSettings& settings = registry.get<ashiato::sync::SyncSettings>();
    REQUIRE(settings.archetypes[full_archetype.value].components[0].component == transform);
    REQUIRE(settings.archetypes[x_only_archetype.value].components[0].component == transform);
    REQUIRE(settings.archetypes[x_only_archetype.value].components[0].serializer == x_only);

    const ProfiledTransform value{3, 9};
    std::array<std::uint8_t, sizeof(TransformSync::Quantized)> quantized{};
    const ashiato::sync::SyncComponentOps& full_ops =
        settings.archetypes[full_archetype.value].component_ops[0];
    const ashiato::sync::SyncComponentOps& x_only_ops =
        settings.archetypes[x_only_archetype.value].component_ops[0];
    full_ops.serialization.quantize(&value, quantized.data());

    ashiato::ComponentSerializationContext context;
    ashiato::BitBuffer full_payload;
    full_ops.serialization.serialize(nullptr, quantized.data(), full_payload, context);
    REQUIRE(full_payload.bit_size() == 16U);

    ashiato::BitBuffer x_only_payload;
    x_only_ops.serialization.serialize(nullptr, quantized.data(), x_only_payload, context);
    REQUIRE(x_only_payload.bit_size() == 8U);

    x_only_payload.reset_read();
    std::array<std::uint8_t, sizeof(TransformSync::Quantized)> decoded{};
    REQUIRE(x_only_ops.serialization.deserialize(x_only_payload, nullptr, decoded.data(), context));
    ProfiledTransform dequantized;
    x_only_ops.serialization.dequantize(decoded.data(), &dequantized);
    REQUIRE(dequantized.x == 3);
    REQUIRE(dequantized.y == 0);
}

TEST_CASE("replication configuration is a direct Ashiato component") {
    ashiato::Registry registry;
    const ashiato::Entity position_component = ashiato::sync::register_sync_component<Position>(registry, "Position");
    const ashiato::sync::SyncArchetypeId actor = ashiato::sync::define_archetype(
        registry,
        "Actor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity entity = registry.create();

    REQUIRE(registry.add<ashiato::sync::Replicated>(ashiato::Entity{}, ashiato::sync::Replicated{actor}) == nullptr);

    REQUIRE(registry.add<ashiato::sync::Replicated>(entity, ashiato::sync::Replicated{actor}) != nullptr);
    REQUIRE(registry.contains<ashiato::sync::Replicated>(entity));
    REQUIRE(registry.get<ashiato::sync::Replicated>(entity).archetype == actor);

    REQUIRE(registry.add<ashiato::sync::Replicated>(
                entity,
                ashiato::sync::Replicated{ashiato::sync::SyncArchetypeId{22}}) != nullptr);
    REQUIRE(registry.get<ashiato::sync::Replicated>(entity).archetype == ashiato::sync::SyncArchetypeId{22});

    REQUIRE(registry.remove<ashiato::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.contains<ashiato::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.remove<ashiato::sync::Replicated>(entity));
}

TEST_CASE("owners can be assigned and replaced independently of replication marker") {
    ashiato::Registry registry;
    const ashiato::Entity entity = registry.create();

    REQUIRE_FALSE(ashiato::sync::set_owner(registry, ashiato::Entity{}, 7));

    REQUIRE(ashiato::sync::set_owner(registry, entity, 7));
    REQUIRE(registry.contains<ashiato::sync::NetworkOwner>(entity));
    REQUIRE(registry.get<ashiato::sync::NetworkOwner>(entity).client == 7);
    REQUIRE_FALSE(registry.contains<ashiato::sync::Replicated>(entity));

    REQUIRE(ashiato::sync::set_owner(registry, entity, 11));
    REQUIRE(registry.get<ashiato::sync::NetworkOwner>(entity).client == 11);
}
