#include "kage/sync/client.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace kage::sync {
namespace {

constexpr std::size_t max_baseline_history_per_entity = 64;

}  // namespace

ReplicationClient::ReplicationClient(ReplicationClientOptions options)
    : options_(options) {}

bool ReplicationClient::receive(ecs::Registry& registry, BitBuffer packet) {
    try {
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message != protocol::server_update_message) {
            return false;
        }
        return apply_update(registry, packet);
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<BitBuffer> ReplicationClient::drain_ack_packets() {
    std::vector<BitBuffer> packets;
    if (pending_acks_.empty()) {
        return packets;
    }

    BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    std::uint16_t packet_acks = 0;
    std::vector<AckRecord> retained_acks;

    auto flush = [&]() {
        if (packet_acks == 0) {
            return;
        }
        packets.push_back(packet);
        packet.clear();
        packet.reserve_bytes(options_.mtu_bytes);
        packet_acks = 0;
    };

    for (const AckRecord& ack : pending_acks_) {
        if (protocol::bytes_for_bits(protocol::client_ack_header_bits + protocol::client_ack_record_bits) >
            options_.mtu_bytes) {
            retained_acks.push_back(ack);
            continue;
        }

        if (packet_acks == 0) {
            packet.push_bits(protocol::client_ack_message, 8U);
            packet.push_bits(0, 16U);
        }

        const std::size_t next_bits = packet.bit_size() + protocol::client_ack_record_bits;
        if (packet_acks != 0 && protocol::bytes_for_bits(next_bits) > options_.mtu_bytes) {
            flush();
            packet.push_bits(protocol::client_ack_message, 8U);
            packet.push_bits(0, 16U);
        }

        if (protocol::bytes_for_bits(packet.bit_size() + protocol::client_ack_record_bits) > options_.mtu_bytes) {
            retained_acks.push_back(ack);
            continue;
        }

        packet.push_bool(ack.destroy);
        packet.push_bits(ack.frame, 32U);
        packet.push_unsigned_bits(ack.entity.value, 64U);
        ++packet_acks;
    }
    flush();

    for (BitBuffer& ack_packet : packets) {
        BitBuffer rewritten;
        rewritten.reserve_bytes(ack_packet.byte_size());
        rewritten.push_bits(protocol::client_ack_message, 8U);

        BitBuffer read = ack_packet;
        read.read_bits(8U);
        read.read_bits(16U);
        const std::uint16_t count =
            static_cast<std::uint16_t>(read.remaining_bits() / protocol::client_ack_record_bits);
        rewritten.push_bits(count, 16U);
        while (read.remaining_bits() >= protocol::client_ack_record_bits) {
            rewritten.push_bool(read.read_bool());
            rewritten.push_bits(read.read_bits(32U), 32U);
            rewritten.push_unsigned_bits(read.read_unsigned_bits(64U), 64U);
        }
        ack_packet = std::move(rewritten);
    }

    pending_acks_ = std::move(retained_acks);
    return packets;
}

std::size_t ReplicationClient::pending_ack_count() const noexcept {
    return pending_acks_.size();
}

ecs::Entity ReplicationClient::local_entity(ecs::Entity server_entity) const {
    const auto found = entities_.find(server_entity.value);
    return found != entities_.end() ? found->second.local : ecs::Entity{};
}

bool ReplicationClient::apply_update(ecs::Registry& registry, BitBuffer& packet) {
    const auto frame = static_cast<SyncFrame>(packet.read_bits(32U));
    const auto record_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    const SyncSettings& settings = registry.get<SyncSettings>();
    bool applied_any = false;

    for (std::uint16_t record = 0; record < record_count; ++record) {
        const bool destroy = packet.read_bool();
        const ecs::Entity server_entity{packet.read_unsigned_bits(64U)};
        if (!server_entity) {
            return false;
        }

        const bool applied = destroy
            ? apply_destroy(registry, frame, server_entity)
            : apply_upsert(registry, settings, frame, server_entity, packet);
        if (!applied) {
            return false;
        }
        applied_any = true;
    }

    return applied_any;
}

bool ReplicationClient::apply_upsert(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ecs::Entity server_entity,
    BitBuffer& packet) {
    const bool full = packet.read_bool();
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    SyncFrame baseline_frame = 0;

    auto found_state = entities_.find(server_entity.value);
    if (full) {
        archetype = SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))};
        if (archetype.value >= settings.archetypes.size()) {
            return false;
        }
    } else {
        if (found_state == entities_.end()) {
            return false;
        }
        archetype = found_state->second.archetype;
        if (archetype.value >= settings.archetypes.size()) {
            return false;
        }
        if (frame <= found_state->second.frame) {
            return false;
        }
        baseline_frame = static_cast<SyncFrame>(packet.read_bits(32U));
    }

    const SyncArchetype& definition = settings.archetypes[archetype.value];
    const auto component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    std::vector<ComponentBaseline> decoded;
    decoded.reserve(component_count);

    EntityState* previous_state = found_state != entities_.end() ? &found_state->second : nullptr;
    const std::vector<ComponentBaseline>* previous_baselines = nullptr;
    if (!full && previous_state != nullptr) {
        const auto found_baseline = std::find_if(
            previous_state->history.begin(),
            previous_state->history.end(),
            [baseline_frame](const EntityState::FrameBaseline& candidate) {
                return candidate.frame == baseline_frame;
            });
        if (found_baseline == previous_state->history.end()) {
            return false;
        }
        previous_baselines = &found_baseline->baselines;
    }

    for (std::uint16_t component = 0; component < component_count; ++component) {
        const auto component_index = static_cast<std::uint16_t>(packet.read_bits(16U));
        const auto payload_bits = static_cast<std::uint32_t>(packet.read_bits(32U));
        if (component_index >= definition.components.size()) {
            return false;
        }

        BitBuffer payload;
        payload.reserve_bytes(protocol::bytes_for_bits(payload_bits));
        for (std::uint32_t bit = 0; bit < payload_bits; ++bit) {
            payload.push_bool(packet.read_bool());
        }

        const ecs::Entity component_entity = definition.components[component_index].component;
        const auto found_ops = settings.component_ops.find(component_entity.value);
        if (found_ops == settings.component_ops.end() || found_ops->second.deserialize == nullptr ||
            found_ops->second.apply == nullptr) {
            return false;
        }

        ComponentBaseline baseline;
        baseline.component = component_entity;
        const SyncComponentOps::QuantizedBytes* previous =
            previous_baselines != nullptr ? baseline_for(*previous_baselines, component_entity) : nullptr;
        if (!found_ops->second.deserialize(payload, previous, baseline.bytes)) {
            return false;
        }
        decoded.push_back(std::move(baseline));
    }

    if (previous_state != nullptr && frame <= previous_state->frame) {
        return false;
    }

    EntityState& state = entities_[server_entity.value];
    if (!state.local) {
        state.local = registry.create();
    }
    state.archetype = archetype;

    if (full && previous_state != nullptr) {
        for (const ComponentBaseline& existing : previous_state->baselines) {
            const auto still_visible = std::find_if(
                decoded.begin(),
                decoded.end(),
                [&](const ComponentBaseline& baseline) {
                    return baseline.component == existing.component;
                });
            if (still_visible != decoded.end()) {
                continue;
            }

            registry.remove(state.local, existing.component);
        }

        state.baselines.erase(
            std::remove_if(
                state.baselines.begin(),
                state.baselines.end(),
                [&](const ComponentBaseline& existing) {
                    return std::find_if(
                        decoded.begin(),
                        decoded.end(),
                        [&](const ComponentBaseline& baseline) {
                            return baseline.component == existing.component;
                        }) == decoded.end();
                }),
            state.baselines.end());
    }

    for (const ComponentBaseline& baseline : decoded) {
        const auto found_ops = settings.component_ops.find(baseline.component.value);
        if (found_ops == settings.component_ops.end() ||
            !found_ops->second.apply(registry, state.local, baseline.bytes)) {
            return false;
        }

        auto found_baseline = std::find_if(
            state.baselines.begin(),
            state.baselines.end(),
            [&](const ComponentBaseline& existing) {
                return existing.component == baseline.component;
            });
        if (found_baseline == state.baselines.end()) {
            state.baselines.push_back(baseline);
        } else {
            found_baseline->bytes = baseline.bytes;
        }
    }

    state.frame = frame;
    remember_baseline(state);
    queue_ack(server_entity, frame, false);
    return true;
}

bool ReplicationClient::apply_destroy(ecs::Registry& registry, SyncFrame frame, ecs::Entity server_entity) {
    const auto found = entities_.find(server_entity.value);
    if (found != entities_.end()) {
        if (frame <= found->second.frame) {
            return false;
        }
        if (found->second.local) {
            registry.destroy(found->second.local);
        }
        entities_.erase(found);
    }
    queue_ack(server_entity, frame, true);
    return true;
}

const SyncComponentOps::QuantizedBytes* ReplicationClient::baseline_for(
    const std::vector<ComponentBaseline>& baselines,
    ecs::Entity component) const {
    const auto found = std::find_if(
        baselines.begin(),
        baselines.end(),
        [component](const ComponentBaseline& baseline) {
            return baseline.component == component;
        });
    return found != baselines.end() ? &found->bytes : nullptr;
}

void ReplicationClient::remember_baseline(EntityState& state) {
    state.history.erase(
        std::remove_if(
            state.history.begin(),
            state.history.end(),
            [frame = state.frame](const EntityState::FrameBaseline& baseline) {
                return baseline.frame == frame;
            }),
        state.history.end());
    state.history.push_back(EntityState::FrameBaseline{state.frame, state.baselines});
    while (state.history.size() > max_baseline_history_per_entity) {
        state.history.erase(state.history.begin());
    }
}

void ReplicationClient::queue_ack(ecs::Entity entity, SyncFrame frame, bool destroy) {
    pending_acks_.erase(
        std::remove_if(
            pending_acks_.begin(),
            pending_acks_.end(),
            [&](const AckRecord& ack) {
                return ack.entity == entity && ack.destroy == destroy;
            }),
        pending_acks_.end());
    pending_acks_.push_back(AckRecord{entity, frame, destroy});
}

}  // namespace kage::sync
