#include "kage/sync/display.hpp"

#include "server/state.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace kage::sync {
namespace {

bool tag_bit_set(std::uint64_t tag_mask, std::size_t tag_index) noexcept {
    return tag_index < 64U && (tag_mask & (std::uint64_t{1} << tag_index)) != 0U;
}

const ReplicatedComponentUpdate* find_component(
    const std::vector<ReplicatedComponentUpdate>& components,
    ecs::Entity component) {
    const auto found = std::find_if(
        components.begin(),
        components.end(),
        [component](const ReplicatedComponentUpdate& update) {
            return update.component == component;
        });
    return found == components.end() ? nullptr : &*found;
}

std::uint64_t visible_tag_mask_for_display(
    const ecs::Registry& registry,
    const SyncArchetype& archetype,
    ecs::Entity entity,
    ClientId local_client) {
    std::uint64_t mask = 0;
    const NetworkOwner* owner = registry.try_get<NetworkOwner>(entity);
    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        const SyncTagReplication& replication = archetype.tags[tag_index];
        if (replication.audience == ReplicationAudience::Owner &&
            (owner == nullptr || owner->client != local_client)) {
            continue;
        }
        if (registry.has(entity, replication.tag)) {
            mask |= std::uint64_t{1} << tag_index;
        }
    }
    return mask;
}

}  // namespace

bool DisplayFrameEntity::try_get_display_value(
    const ecs::Registry& registry,
    ecs::Entity component,
    void* out) const {
    if (out == nullptr) {
        return false;
    }
    if (!registry.has<DisplayInterpolated>(component)) {
        throw std::logic_error("display frame component is not marked display-interpolated");
    }
    const ReplicatedComponentUpdate* update = find_component(components, component);
    if (update == nullptr) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end() || found_ops->second.dequantize == nullptr) {
        return false;
    }
    if (update->bytes.size() != found_ops->second.quantized_size) {
        return false;
    }
    found_ops->second.dequantize(update->bytes.data(), out);
    return true;
}

bool DisplayFrameEntity::has_tag(const ecs::Registry& registry, ecs::Entity tag) const {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (archetype.value >= settings.archetypes.size()) {
        return false;
    }
    const SyncArchetype& definition = settings.archetypes[archetype.value];
    for (std::size_t tag_index = 0; tag_index < definition.tags.size(); ++tag_index) {
        if (definition.tags[tag_index].tag == tag) {
            return tag_bit_set(tag_mask, tag_index);
        }
    }
    return false;
}

DisplayFrameInterpolation::DisplayFrameInterpolation(ReplicationClient& client)
    : source_(Source::Client), client_(&client) {}

DisplayFrameInterpolation::DisplayFrameInterpolation(ReplicationServer& server)
    : source_(Source::Server), server_(&server) {}

ClientId DisplayFrameInterpolation::local_client() const noexcept {
    return source_ == Source::Client
        ? (client_ != nullptr ? client_->client_id() : invalid_client_id)
        : (server_ != nullptr ? server_->local_client() : invalid_client_id);
}

double DisplayFrameInterpolation::target_frame() const noexcept {
    if (source_ == Source::Client) {
        return client_ != nullptr ? client_->display_target_frame() : 0.0;
    }
    if (server_ == nullptr) {
        return 0.0;
    }
    const double alpha = server_->options_.fixed_dt_seconds > 0.0
        ? server_->tick_accumulator_seconds_ / server_->options_.fixed_dt_seconds
        : 0.0;
    if (server_->frame_ == 0U) {
        return alpha;
    }
    return static_cast<double>(server_->frame_ - 1U) + std::clamp(alpha, 0.0, 1.0);
}

void DisplayFrameInterpolation::capture_server_frame(ecs::Registry& registry) {
    if (source_ != Source::Server || server_ == nullptr) {
        return;
    }
    server_->refresh_replicated(registry);

    Snapshot next;
    next.frame = server_->frame_;
    next.valid = true;

    const SyncSettings& settings = registry.get<SyncSettings>();
    const ClientId local = server_->local_client_;
    next.entities.reserve(server_->active_replicated_count_);
    for (const ReplicationServer::ReplicatedSlot& slot : server_->replicated_) {
        if (!slot.active || slot.archetype.value >= settings.archetypes.size() || !registry.alive(slot.entity)) {
            continue;
        }
        const SyncArchetype& archetype = settings.archetypes[slot.archetype.value];
        DisplayFrameEntity entity;
        entity.local_entity = slot.entity;
        entity.archetype = slot.archetype;
        entity.frame = next.frame;
        entity.tag_mask = visible_tag_mask_for_display(registry, archetype, slot.entity, local);
        entity.components.reserve(archetype.components.size());
        const NetworkOwner* owner = registry.try_get<NetworkOwner>(slot.entity);
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const ComponentReplication& replication = archetype.components[component_index];
            if (replication.audience == ReplicationAudience::Owner &&
                (owner == nullptr || owner->client != local)) {
                continue;
            }
            if (!registry.has<DisplayInterpolated>(replication.component)) {
                continue;
            }
            const void* value = registry.get(slot.entity, replication.component);
            if (value == nullptr || component_index >= archetype.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            if (ops.quantize == nullptr || ops.quantized_size == 0U) {
                continue;
            }
            ReplicatedComponentUpdate update;
            update.component = replication.component;
            update.bytes.resize(ops.quantized_size);
            ops.quantize(value, update.bytes.data());
            entity.components.push_back(std::move(update));
        }
        next.entities.push_back(std::move(entity));
    }

    previous_ = std::move(current_);
    current_ = std::move(next);
}

void DisplayFrameInterpolation::rebuild_from_client(const ecs::Registry& registry) {
    display_.clear();
    if (client_ == nullptr) {
        return;
    }
    const DisplayInterpolationSampleBuffer& samples = client_->display_interpolation_frame(registry);
    display_.reserve(samples.entities.size());
    for (const DisplayInterpolationSample& sample : samples.entities) {
        DisplayFrameEntity entity;
        entity.client_entity_network_id = sample.client_entity_network_id;
        entity.local_entity = sample.local_entity;
        entity.archetype = sample.archetype;
        entity.frame = sample.frame;
        entity.alpha = sample.alpha;
        entity.tag_mask = sample.tag_mask;
        entity.components = sample.components;
        display_.push_back(std::move(entity));
    }
}

void DisplayFrameInterpolation::rebuild_from_server(const ecs::Registry& registry) {
    display_.clear();
    if (!current_.valid) {
        return;
    }
    const SyncSettings& settings = registry.get<SyncSettings>();
    const float alpha = server_ != nullptr && server_->options_.fixed_dt_seconds > 0.0
        ? static_cast<float>(std::clamp(
              server_->tick_accumulator_seconds_ / server_->options_.fixed_dt_seconds,
              0.0,
              1.0))
        : 0.0f;
    display_.reserve(current_.entities.size());
    for (const DisplayFrameEntity& current : current_.entities) {
        DisplayFrameEntity display = current;
        display.alpha = alpha;
        const auto found_previous = previous_.valid
            ? std::find_if(
                  previous_.entities.begin(),
                  previous_.entities.end(),
                  [&](const DisplayFrameEntity& previous) {
                      return previous.local_entity == current.local_entity &&
                          previous.archetype == current.archetype;
                  })
            : previous_.entities.end();
        if (found_previous == previous_.entities.end()) {
            display_.push_back(std::move(display));
            continue;
        }

        for (ReplicatedComponentUpdate& component : display.components) {
            const ReplicatedComponentUpdate* previous_component =
                find_component(found_previous->components, component.component);
            if (previous_component == nullptr) {
                continue;
            }
            if (current.archetype.value >= settings.archetypes.size()) {
                continue;
            }
            const SyncArchetype& archetype = settings.archetypes[current.archetype.value];
            const auto found_replication = std::find_if(
                archetype.components.begin(),
                archetype.components.end(),
                [&](const ComponentReplication& replication) {
                    return replication.component == component.component;
                });
            if (found_replication == archetype.components.end() ||
                found_replication->interpolation != ComponentInterpolation::Interpolate) {
                continue;
            }
            const auto found_ops = settings.component_ops.find(component.component.value);
            if (found_ops == settings.component_ops.end() || found_ops->second.interpolate == nullptr ||
                component.bytes.size() != found_ops->second.quantized_size ||
                previous_component->bytes.size() != found_ops->second.quantized_size) {
                continue;
            }
            SyncComponentOps::QuantizedBytes blended;
            blended.resize(found_ops->second.quantized_size);
            if (found_ops->second.interpolate(
                    previous_component->bytes.data(),
                    component.bytes.data(),
                    alpha,
                    blended.data())) {
                component.bytes = std::move(blended);
            }
        }
        display_.push_back(std::move(display));
    }
}

const std::vector<DisplayFrameEntity>& DisplayFrameInterpolation::entities(const ecs::Registry& registry) {
    if (source_ == Source::Client) {
        rebuild_from_client(registry);
    } else {
        rebuild_from_server(registry);
    }
    return display_;
}

}  // namespace kage::sync
