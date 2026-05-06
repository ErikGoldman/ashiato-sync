#include "kage/sync/display.hpp"

#include "server/state.hpp"

#include <algorithm>
#include <cmath>

namespace kage::sync {
namespace {

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

}  // namespace

FractionalTickSampler::FractionalTickSampler(ReplicationClient& client)
    : source_(Source::Client), client_(&client) {}

FractionalTickSampler::FractionalTickSampler(ReplicationServer& server)
    : source_(Source::Server), server_(&server) {}

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

void FractionalTickSampler::capture_server_frame(ecs::Registry& registry) {
    if (source_ != Source::Server || server_ == nullptr) {
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
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const ComponentReplication& replication = archetype.components[component_index];
            if (replication.audience == ReplicationAudience::Owner &&
                (owner == nullptr || owner->client != server_->local_client_)) {
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
            if (ops.quantize == nullptr || ops.quantized_size == 0U) {
                continue;
            }
            ReplicatedComponentUpdate update;
            update.component = replication.component;
            update.bytes.resize(ops.quantized_size);
            ops.quantize(value, update.bytes.data());
            entity.sample.components_.push_back(std::move(update));
        }
        next.entities.push_back(std::move(entity));
    }

    previous_ = std::move(current_);
    current_ = std::move(next);
}

void FractionalTickSampler::rebuild_from_client(const ecs::Registry& registry) {
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

void FractionalTickSampler::rebuild_from_server(const ecs::Registry& registry) {
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
    samples_.reserve(current_.entities.size());
    for (const FractionalTickSampler::SnapshotEntity& current : current_.entities) {
        FractionalTickSample display = current.sample;
        display.alpha = alpha;
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
            samples_.push_back(std::move(display));
            continue;
        }

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
        samples_.push_back(std::move(display));
    }
}

const std::vector<FractionalTickSample>& FractionalTickSampler::entities(const ecs::Registry& registry) {
    if (source_ == Source::Client) {
        rebuild_from_client(registry);
    } else {
        rebuild_from_server(registry);
    }
    return samples_;
}

}  // namespace kage::sync
