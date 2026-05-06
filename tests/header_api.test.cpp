#include "test_components.hpp"

#include "../examples/network_simulator.hpp"

#include "kage/sync/detail/type_name.hpp"
#include "kage/sync/simulated_link.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

namespace {

struct HeaderApiDefaultNamedComponent {
    float value = 0.0f;
};

struct HeaderApiDefaultNamedCue {
    int value = 0;
};

}  // namespace

namespace kage::sync {

template <>
struct SyncComponentTraits<HeaderApiDefaultNamedComponent> {
    using Quantized = HeaderApiDefaultNamedComponent;

    static Quantized quantize(const HeaderApiDefaultNamedComponent& value) {
        return value;
    }

    static HeaderApiDefaultNamedComponent dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(current));
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized*, Quantized& out) {
        if (in.remaining_bits() < sizeof(out) * 8U) {
            return false;
        }
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(out));
        return true;
    }
};

template <>
struct SyncCueTraits<HeaderApiDefaultNamedCue> {
    static void serialize(const HeaderApiDefaultNamedCue& cue, ecs::BitBuffer& out) {
        out.push_bits(cue.value, 8U);
    }

    static bool deserialize(ecs::BitBuffer& in, HeaderApiDefaultNamedCue& out) {
        out.value = static_cast<int>(in.read_bits(8U));
        return true;
    }

    static bool play(ecs::Registry&, ecs::Entity, const HeaderApiDefaultNamedCue&, float) {
        return true;
    }

    static bool rollback(ecs::Registry&, ecs::Entity, const HeaderApiDefaultNamedCue&) {
        return true;
    }

    static bool equals_cue(const HeaderApiDefaultNamedCue& lhs, const HeaderApiDefaultNamedCue& rhs) {
        return lhs.value == rhs.value;
    }
};

}  // namespace kage::sync

TEST_CASE("default type names strip compiler scopes and anonymous namespace wrappers") {
    REQUIRE(kage::sync::detail::short_type_name("struct outer::Inner") == "Inner");
    REQUIRE(kage::sync::detail::short_type_name("class Namespace::Widget") == "Widget");
    REQUIRE(kage::sync::detail::short_type_name("(anonymous namespace)::Hidden") == "Hidden");
}

TEST_CASE("header registration APIs assign default names for components and cues") {
    ecs::Registry registry;
    const ecs::Entity component = kage::sync::register_sync_component<HeaderApiDefaultNamedComponent>(registry);
    const kage::sync::SyncCueTypeId cue = kage::sync::register_sync_cue<HeaderApiDefaultNamedCue>(registry);

    const kage::sync::SyncSettings& settings = registry.get<kage::sync::SyncSettings>();
    REQUIRE(settings.component_ops.at(component.value).name.empty());
    REQUIRE(settings.cue_ops.at(cue).name == "HeaderApiDefaultNamedCue");
}

TEST_CASE("simulated link orders delayed packets and preserves FIFO ties") {
    kage::sync::SimulatedLink<std::string, int> link;
    link.settings.latency_ms = 0.0;
    link.settings.jitter_ms = 0.0;

    REQUIRE(link.enqueue(1, std::string{"second"}, 2.0));
    REQUIRE(link.enqueue(1, std::string{"first"}, 1.0));
    REQUIRE(link.enqueue(1, std::string{"third"}, 2.0));

    std::vector<std::string> delivered;
    REQUIRE(link.deliver_ready(1.5, [&](int endpoint, const std::string& payload) {
        REQUIRE(endpoint == 1);
        delivered.push_back(payload);
    }) == 1U);
    REQUIRE(delivered == std::vector<std::string>{"first"});

    REQUIRE(link.deliver_ready(2.0, [&](int endpoint, const std::string& payload) {
        REQUIRE(endpoint == 1);
        delivered.push_back(payload);
    }) == 2U);
    REQUIRE((delivered == std::vector<std::string>{"first", "second", "third"}));
    REQUIRE(link.empty());
}

TEST_CASE("simulated link loss and clamped negative latency are deterministic at extremes") {
    kage::sync::SimulatedLink<std::string, int> lossy({0.0, 0.0, 100.0}, 17);
    REQUIRE_FALSE(lossy.enqueue(1, std::string{"drop"}, 4.0));
    REQUIRE(lossy.empty());

    kage::sync::SimulatedLink<std::string, int> clamped({-10.0, 0.0, 0.0}, 17);
    REQUIRE(clamped.enqueue(2, std::string{"ready"}, 4.0));
    REQUIRE(clamped.size() == 1U);
    REQUIRE(clamped.queued_packets().front().deliver_at == 4.0);
}

TEST_CASE("example network simulator drops queued packets without preserving bandwidth delay") {
    kage::sync::examples::NetworkSimulator<int> link;
    link.settings.latency_ms = 100.0;
    link.settings.bandwidth_kbps = 1.0;

    ecs::BitBuffer packet;
    packet.push_bits(1, 8U);
    REQUIRE(link.enqueue(1, packet, 1.0));
    REQUIRE(link.size() == 1U);

    REQUIRE(link.drop_queued(1.5) == 1U);
    REQUIRE(link.empty());
    REQUIRE(link.queued_bytes() == 0U);

    REQUIRE(link.enqueue(1, packet, 1.5));
    REQUIRE(link.size() == 1U);
    REQUIRE(link.queued_packets().front().deliver_at >= 1.5);
    REQUIRE(link.queued_packets().front().deliver_at < 2.0);
}
