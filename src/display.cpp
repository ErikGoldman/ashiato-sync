#include "ashiato/sync/display.hpp"

#include "server/state.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace ashiato::sync {
namespace {

const ReplicatedComponentUpdate* find_component(
    const std::vector<ReplicatedComponentUpdate>& components,
    ashiato::Entity component) {
    const auto found = std::find_if(
        components.begin(),
        components.end(),
        [component](const ReplicatedComponentUpdate& update) {
            return update.component == component;
        });
    return found == components.end() ? nullptr : &*found;
}

}  // namespace

FractionalTickSampler::FractionalTickSampler(ReplicationClient& client)
    : source_(Source::Client), client_(&client) {}

FractionalTickSampler::FractionalTickSampler(ReplicationServer& server)
    : source_(Source::Server),
      server_(&server),
      server_frame_subscription_(server.subscribe_registry_dirty_frame_listener(*this)) {}

ClientId FractionalTickSampler::local_client() const noexcept {
    return source_ == Source::Client
        ? (client_ != nullptr ? client_->client_id() : invalid_client_id)
        : (server_ != nullptr ? server_->local_client() : invalid_client_id);
}

double FractionalTickSampler::target_frame() const noexcept {
    if (source_ == Source::Client) {
        return client_ != nullptr ? client_->fractional_tick_target_frame() : 0.0;
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

void FractionalTickSampler::capture_server_frame(ashiato::Registry& registry) {
    if (source_ != Source::Server || server_ == nullptr) {
        return;
    }
    if (current_.valid && current_.frame == server_->frame_) {
        return;
    }

    server_->rediscover_all_replicated_entities(registry);

    Snapshot next;
    next.frame = server_->frame_;
    next.valid = true;

    const SyncSettings& settings = registry.get<SyncSettings>();
    next.entities.reserve(server_->active_replicated_count_);
    for (const ReplicationServer::ReplicatedSlot& slot : server_->replicated_) {
        if (!slot.active || slot.archetype.value >= settings.archetypes.size() || !registry.alive(slot.entity)) {
            continue;
        }
        const SyncArchetype& archetype = settings.archetypes[slot.archetype.value];
        FractionalTickSampler::SnapshotEntity entity;
        entity.archetype = slot.archetype;
        entity.sample.local_entity = slot.entity;
        entity.sample.frame = next.frame;
        entity.sample.components_.reserve(archetype.components.size());
        const NetworkOwner* owner = registry.try_get<NetworkOwner>(slot.entity);
        const ClientId owner_client = owner != nullptr ? owner->client : invalid_client_id;
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const ComponentReplication& replication = archetype.components[component_index];
            if (!replication_audience_matches(replication.audience, owner_client, server_->local_client_)) {
                continue;
            }
            if (!registry.has<FractionalTickSampled>(replication.component)) {
                continue;
            }
            const void* value = registry.get(slot.entity, replication.component);
            if (value == nullptr || component_index >= archetype.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            if (ops.serialization.quantize == nullptr || ops.serialization.quantized_size == 0U) {
                continue;
            }
            ReplicatedComponentUpdate update;
            update.component = replication.component;
            update.serializer = replication.serializer;
            update.bytes.resize(ops.serialization.quantized_size);
            ops.serialization.quantize(value, update.bytes.data());
            entity.sample.components_.push_back(std::move(update));
        }
        next.entities.push_back(std::move(entity));
    }

    previous_ = std::move(current_);
    current_ = std::move(next);
}

void FractionalTickSampler::on_server_registry_dirty_frame(const ServerRegistryDirtyFrame& frame) {
    if (source_ != Source::Server || server_ == nullptr || &frame.server != server_) {
        return;
    }
    capture_server_frame(frame.registry);
}

void FractionalTickSampler::rebuild_from_client(const ashiato::Registry& registry) {
    samples_.clear();
    if (client_ == nullptr) {
        return;
    }
    const FractionalTickSampleBuffer& samples = client_->fractional_tick_frame(registry);
    samples_.reserve(samples.entities.size());
    for (const FractionalTickSample& sample : samples.entities) {
        samples_.push_back(sample);
    }
}

void FractionalTickSampler::rebuild_from_server(const ashiato::Registry& registry) {
    samples_.clear();
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
    const double target = target_frame();
    const bool target_valid = target >= 0.0 &&
        std::isfinite(target) &&
        target <= static_cast<double>(std::numeric_limits<SyncFrame>::max());
    const double target_floor_value = target_valid ? std::floor(target) : 0.0;
    samples_.reserve(current_.entities.size());
    for (const FractionalTickSampler::SnapshotEntity& current : current_.entities) {
        FractionalTickSample display = current.sample;
        display.alpha = alpha;
        display.target_frame = target;
        display.target_floor_frame = target_valid ? static_cast<SyncFrame>(target_floor_value) : 0;
        display.target_alpha = target_valid ? static_cast<float>(target - target_floor_value) : 0.0f;
        display.target_valid = target_valid;
        const auto found_previous = previous_.valid
            ? std::find_if(
                  previous_.entities.begin(),
                  previous_.entities.end(),
                  [&](const FractionalTickSampler::SnapshotEntity& previous) {
                      return previous.sample.local_entity == current.sample.local_entity &&
                          previous.archetype == current.archetype;
                  })
            : previous_.entities.end();
        if (found_previous == previous_.entities.end()) {
            display.floor_frame_present = true;
            display.next_frame_present = false;
            samples_.push_back(std::move(display));
            continue;
        }

        display.floor_frame_present = true;
        display.next_frame_present = true;
        for (ReplicatedComponentUpdate& component : display.components_) {
            const ReplicatedComponentUpdate* previous_component =
                find_component(found_previous->sample.components_, component.component);
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
            const std::size_t component_index =
                static_cast<std::size_t>(std::distance(archetype.components.begin(), found_replication));
            if (component_index >= archetype.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            if (ops.interpolate == nullptr ||
                component.bytes.size() != ops.serialization.quantized_size ||
                previous_component->bytes.size() != ops.serialization.quantized_size) {
                continue;
            }
            SyncComponentOps::QuantizedBytes blended;
            blended.resize(ops.serialization.quantized_size);
            if (ops.interpolate(
                    previous_component->bytes.data(),
                    component.bytes.data(),
                    alpha,
                    blended.data())) {
                component.bytes = std::move(blended);
            }
        }
        samples_.push_back(std::move(display));
    }
}

const std::vector<FractionalTickSample>& FractionalTickSampler::entities(const ashiato::Registry& registry) {
    if (source_ == Source::Client) {
        rebuild_from_client(registry);
    } else {
        rebuild_from_server(registry);
    }
    return samples_;
}

}  // namespace ashiato::sync
