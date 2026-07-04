#include "client/runtime/display_sampler.hpp"

#include "client/store/entity_store.hpp"
#include "client/frame_data.hpp"
#include "client/store/frame_ring_store.hpp"
#include "client/runtime/prediction_runtime.hpp"

#include "ashiato/sync/components.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ashiato::sync::client_detail {
namespace {

std::uint64_t hash_bytes(const std::uint8_t* bytes, std::size_t size) noexcept {
    if (bytes == nullptr || size == 0U) {
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t combine_hash(std::uint64_t seed, std::uint64_t value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

std::uint64_t frame_payload_hash(const detail::FrameDataView& frame) noexcept {
    std::uint64_t hash = combine_hash(0, frame.tag_mask);
    hash = combine_hash(hash, frame.present_mask);
    return combine_hash(hash, hash_bytes(frame.bytes, frame.byte_count));
}

void clear_diagnostics(FractionalTickSample& display) noexcept {
    display.sampled_component_count = 0;
    display.interpolated_component_count = 0;
    display.stepped_component_count = 0;
    display.missing_next_component_count = 0;
    display.missing_interpolate_op_count = 0;
    display.interpolated_equals_floor = false;
    display.interpolated_equals_next = false;
    display.source_floor_frame = 0;
    display.source_next_frame = 0;
    display.source_floor_generation = 0;
    display.source_next_generation = 0;
    display.source_floor_write_source = 0;
    display.source_next_write_source = 0;
    display.source_floor_presentation_cache_hit = false;
    display.source_next_presentation_cache_hit = false;
    display.source_floor_presentation_origin_valid = false;
    display.source_next_presentation_origin_valid = false;
    display.source_floor_presentation_origin_generation = 0;
    display.source_next_presentation_origin_generation = 0;
    display.source_floor_presentation_origin_write_source = 0;
    display.source_next_presentation_origin_write_source = 0;
    display.source_floor_presentation_origin_payload_hash = 0;
    display.source_next_presentation_origin_payload_hash = 0;
    display.floor_component_hash = 0;
    display.next_component_hash = 0;
    display.result_component_hash = 0;
    display.prediction_last_frame = 0;
    display.prediction_accumulator_seconds = 0.0;
    display.prediction_run_count = 0;
    display.prediction_rollback_queued_count = 0;
    display.prediction_rollback_applied_count = 0;
    display.prediction_resimulated_frame_count = 0;
    display.prediction_seed_count = 0;
    display.prediction_pending_rollback_frame = 0;
    display.prediction_has_pending_rollback = false;
}

void write_prediction_diagnostics(
    FractionalTickSample& display,
    const ReplicationClientClock& clock,
    const ClientPredictionRuntime& prediction) noexcept {
    display.prediction_last_frame = prediction.last_predicted_frame();
    display.prediction_accumulator_seconds = clock.predicted_accumulator_seconds();
    display.prediction_run_count = prediction.predicted_frame_run_count();
    display.prediction_rollback_queued_count = prediction.rollback_queued_count();
    display.prediction_rollback_applied_count = prediction.rollback_applied_count();
    display.prediction_resimulated_frame_count = prediction.resimulated_frame_count();
    display.prediction_seed_count = prediction.prediction_seed_count();
    display.prediction_pending_rollback_frame = prediction.pending_rollback_frame();
    display.prediction_has_pending_rollback = prediction.has_pending_rollback();
}

}  // namespace

ClientDisplaySampler::ClientDisplaySampler(
    const ReplicationClientClock& clock,
    const ClientEntityStore& entity_store,
    ClientPredictionRuntime& prediction,
    const ClientFrameRingStore& buffered_frames,
    const ClientFrameRingStore& predicted_frames,
    ClientFrameRingStore& predicted_presentation_frames,
    const FractionalTickSampleBuffer& previous_frame,
    double fixed_dt_seconds) noexcept
    : clock_(clock),
      entity_store_(entity_store),
      prediction_(prediction),
      buffered_frames_(buffered_frames),
      predicted_frames_(predicted_frames),
      predicted_presentation_frames_(predicted_presentation_frames),
      previous_frame_(previous_frame),
      fixed_dt_seconds_(fixed_dt_seconds) {}

const FractionalTickSample* ClientDisplaySampler::find_previous_sample(
    ClientEntityNetworkId client_entity_network_id) const {
    const auto found = std::find_if(
        previous_frame_.entities.begin(),
        previous_frame_.entities.end(),
        [client_entity_network_id](const FractionalTickSample& sample) {
            return sample.client_entity_network_id == client_entity_network_id;
        });
    return found != previous_frame_.entities.end() ? &*found : nullptr;
}

FractionalTickSample& ClientDisplaySampler::next_sample(WriteContext& context) const {
    if (context.sampled_count == context.out.entities.size()) {
        context.out.entities.emplace_back();
    }
    return context.out.entities[context.sampled_count];
}

bool ClientDisplaySampler::append_previous_sample(
    WriteContext& context,
    ClientEntityNetworkId client_entity_network_id) const {
    const FractionalTickSample* previous = find_previous_sample(client_entity_network_id);
    if (previous == nullptr) {
        return false;
    }
    FractionalTickSample& display = next_sample(context);
    display = *previous;
    display.source = FractionalTickSample::Source::PreviousSample;
    display.target_frame = context.target_frame;
    display.target_floor_frame = context.floor_frame;
    display.target_alpha = context.alpha;
    display.target_valid = context.target_valid;
    display.floor_frame_present = false;
    display.next_frame_present = false;
    clear_diagnostics(display);
    write_prediction_diagnostics(display, clock_, prediction_);
    ++context.sampled_count;
    return true;
}

bool ClientDisplaySampler::append_frame_sample(
    WriteContext& context,
    const EntityState& state,
    detail::FrameDataView baseline,
    SyncArchetypeId archetype_id,
    SyncFrame frame,
    float alpha,
    const EntityFrameView* next_frame_sample,
    FractionalTickSample::Source source,
    bool floor_frame_present,
    std::uint64_t floor_write_generation,
    FrameWriteSource floor_write_source,
    const EntityFrameView* floor_frame_sample,
    bool apply_snap_errors) const {
    if (archetype_id.value >= context.settings.archetypes.size()) {
        return false;
    }

    FractionalTickSample& display = next_sample(context);
    display.client_entity_network_id = state.identity.client_entity_network_id;
    display.local_entity = state.identity.local;
    display.frame = frame;
    display.alpha = next_frame_sample != nullptr ? alpha : 0.0f;
    display.source = source;
    display.mode = state.mode.current;
    display.target_frame = context.target_frame;
    display.target_floor_frame = context.floor_frame;
    display.target_alpha = context.alpha;
    display.target_valid = context.target_valid;
    display.floor_frame_present = floor_frame_present;
    display.next_frame_present = next_frame_sample != nullptr;
    clear_diagnostics(display);
    write_prediction_diagnostics(display, clock_, prediction_);
    display.source_floor_frame = frame;
    display.source_next_frame = next_frame_sample != nullptr ? next_frame_sample->frame : 0;
    display.source_floor_generation = floor_write_generation;
    display.source_next_generation = next_frame_sample != nullptr ? next_frame_sample->write_generation : 0;
    display.source_floor_write_source = static_cast<std::uint32_t>(floor_write_source);
    display.source_next_write_source = next_frame_sample != nullptr
        ? static_cast<std::uint32_t>(next_frame_sample->write_source)
        : 0U;
    if (floor_frame_sample != nullptr) {
        display.source_floor_presentation_cache_hit = floor_frame_sample->presentation_cache_hit;
        display.source_floor_presentation_origin_valid = floor_frame_sample->presentation_origin_valid;
        display.source_floor_presentation_origin_generation = floor_frame_sample->presentation_origin_generation;
        display.source_floor_presentation_origin_write_source =
            static_cast<std::uint32_t>(floor_frame_sample->presentation_origin_write_source);
        display.source_floor_presentation_origin_payload_hash =
            floor_frame_sample->presentation_origin_payload_hash;
    }
    if (next_frame_sample != nullptr) {
        display.source_next_presentation_cache_hit = next_frame_sample->presentation_cache_hit;
        display.source_next_presentation_origin_valid = next_frame_sample->presentation_origin_valid;
        display.source_next_presentation_origin_generation = next_frame_sample->presentation_origin_generation;
        display.source_next_presentation_origin_write_source =
            static_cast<std::uint32_t>(next_frame_sample->presentation_origin_write_source);
        display.source_next_presentation_origin_payload_hash =
            next_frame_sample->presentation_origin_payload_hash;
    }
    display.components_.clear();

    const SyncArchetype& archetype = context.settings.archetypes[archetype_id.value];
    display.components_.reserve(archetype.components.size());
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const ashiato::Entity component = archetype.components[component_index].component;
        if (!frame_has_component(baseline, component_index) ||
            !context.registry.has<FractionalTickSampled>(component)) {
            continue;
        }
        if (component_index >= archetype.component_ops.size()) {
            return false;
        }

        const SyncComponentOps& ops = archetype.component_ops[component_index];
        const std::uint8_t* baseline_bytes = frame_component_data(archetype, baseline, component_index);
        if (baseline_bytes == nullptr) {
            return false;
        }
        const std::uint64_t floor_hash = hash_bytes(baseline_bytes, ops.serialization.quantized_size);
        display.floor_component_hash = combine_hash(display.floor_component_hash, floor_hash);

        ReplicatedComponentUpdate value;
        value.component = component;
        value.serializer = archetype.components[component_index].serializer;
        const bool next_has_component =
            next_frame_sample != nullptr &&
            frame_has_component(next_frame_sample->baseline, component_index);
        const bool component_interpolates =
            archetype.components[component_index].interpolation == ComponentInterpolation::Interpolate;
        if (next_has_component && component_interpolates) {
            if (ops.interpolate == nullptr) {
                ++display.missing_interpolate_op_count;
                return false;
            }
            const std::uint8_t* next_bytes = frame_component_data(archetype, next_frame_sample->baseline, component_index);
            value.bytes.resize(ops.serialization.quantized_size);
            if (next_bytes == nullptr || !ops.interpolate(baseline_bytes, next_bytes, alpha, value.bytes.data())) {
                return false;
            }
            const std::uint64_t next_hash = hash_bytes(next_bytes, ops.serialization.quantized_size);
            display.next_component_hash = combine_hash(display.next_component_hash, next_hash);
            display.interpolated_equals_floor =
                std::memcmp(value.bytes.data(), baseline_bytes, ops.serialization.quantized_size) == 0;
            display.interpolated_equals_next =
                std::memcmp(value.bytes.data(), next_bytes, ops.serialization.quantized_size) == 0;
            ++display.interpolated_component_count;
        } else {
            value.bytes.assign(baseline_bytes, ops.serialization.quantized_size);
            ++display.stepped_component_count;
            if (next_frame_sample != nullptr && !next_has_component) {
                ++display.missing_next_component_count;
            }
        }

        if (apply_snap_errors) {
            auto found_error = std::find_if(
                state.visual.snap_errors.begin(),
                state.visual.snap_errors.end(),
                [component](const EntityComponentError& error) {
                    return error.component == component;
                });
            if (found_error != state.visual.snap_errors.end()) {
                if (ops.apply_error == nullptr ||
                    !ops.apply_error(value.bytes.data(), found_error->bytes, value.bytes)) {
                    return false;
                }
            }
        }

        display.result_component_hash = combine_hash(
            display.result_component_hash,
            hash_bytes(value.bytes.empty() ? nullptr : value.bytes.data(), value.bytes.size()));
        display.components_.push_back(std::move(value));
        ++display.sampled_component_count;
    }

    if (!display.components_.empty()) {
        ++context.sampled_count;
    }
    return true;
}

bool ClientDisplaySampler::append_latest_buffered_sample(WriteContext& context, const EntityState& state) const {
    if (state.replication.entity_present && state.identity.archetype.value < context.settings.archetypes.size()) {
        const detail::FrameDataView baseline{
            state.replication.baseline.tag_mask,
            state.replication.baseline.present_mask,
            state.replication.baseline.bytes.empty() ? nullptr : state.replication.baseline.bytes.data(),
            state.replication.baseline.bytes.size()};
        return append_frame_sample(
            context,
            state,
            baseline,
            state.identity.archetype,
            state.replication.frame,
            0.0f,
            nullptr,
            FractionalTickSample::Source::LatestBuffered,
            true,
            0U,
            FrameWriteSource::Unknown,
            nullptr,
            false);
    }

    const std::uint32_t entity_index = entity_store_.index_of(state);
    EntityFrameView latest;
    return buffered_frames_.latest_present(entity_index, latest)
        ? append_frame_sample(
            context,
            state,
            latest.baseline,
            state.identity.archetype,
            latest.frame,
            0.0f,
            nullptr,
            FractionalTickSample::Source::LatestBuffered,
            true,
            latest.write_generation,
            latest.write_source,
            &latest,
            false)
        : false;
}

bool ClientDisplaySampler::append_missing_buffered_sample(WriteContext& context, const EntityState& state) const {
    if (!state.replication.entity_present && (!context.target_valid || context.floor_frame >= state.replication.frame)) {
        return false;
    }
    if (append_previous_sample(context, state.identity.client_entity_network_id)) {
        return true;
    }
    return append_latest_buffered_sample(context, state);
}

bool ClientDisplaySampler::append_buffered_sample(
    WriteContext& context,
    std::uint32_t entity_index,
    const EntityState& state) const {
    if (buffered_frames_.empty(entity_index) || !context.target_valid) {
        return append_missing_buffered_sample(context, state);
    }

    EntityFrameView floor_sample;
    if (!buffered_frames_.view(entity_index, context.floor_frame, floor_sample)) {
        return append_missing_buffered_sample(context, state);
    }
    if (!floor_sample.entity_present) {
        return true;
    }

    const EntityFrameView* next_frame_sample = nullptr;
    EntityFrameView next_frame_sample_storage;
    if (context.alpha > 0.0f && context.floor_frame != std::numeric_limits<SyncFrame>::max()) {
        const SyncFrame next_frame = context.floor_frame + 1U;
        if (buffered_frames_.view(entity_index, next_frame, next_frame_sample_storage) &&
            next_frame_sample_storage.entity_present) {
            next_frame_sample = &next_frame_sample_storage;
        }
    }

    return append_frame_sample(
        context,
        state,
        floor_sample.baseline,
        state.identity.archetype,
        context.floor_frame,
        context.alpha,
        next_frame_sample,
        FractionalTickSample::Source::Buffered,
        true,
        floor_sample.write_generation,
        floor_sample.write_source,
        &floor_sample,
        false);
}

bool ClientDisplaySampler::append_predicted_sample(
    WriteContext& context,
    std::uint32_t entity_index,
    const EntityState& state,
    bool& appended) const {
    appended = false;
    if (state.mode.current != ReplicationClientMode::Predict ||
        !prediction_.has_predicted_frame() ||
        predicted_frames_.empty(entity_index)) {
        return true;
    }

    EntityFrameView current_sample;
    const SyncFrame last_predicted_frame = prediction_.last_predicted_frame();
    if (!ensure_predicted_presentation_frame(entity_index, last_predicted_frame, current_sample) ||
        !current_sample.entity_present) {
        return true;
    }

    const EntityFrameView* floor_sample = &current_sample;
    const EntityFrameView* next_frame_sample = nullptr;
    EntityFrameView previous_sample;
    float predicted_alpha = 0.0f;
    if (last_predicted_frame != 0U) {
        const SyncFrame previous_frame = last_predicted_frame - 1U;
        if (ensure_predicted_presentation_frame(entity_index, previous_frame, previous_sample) &&
            previous_sample.entity_present) {
            floor_sample = &previous_sample;
            next_frame_sample = &current_sample;
            predicted_alpha = static_cast<float>(clock_.predicted_accumulator_seconds() / fixed_dt_seconds_);
        }
    }

    if (!append_frame_sample(
            context,
            state,
            floor_sample->baseline,
            state.identity.archetype,
            floor_sample->frame,
            predicted_alpha,
            next_frame_sample,
            FractionalTickSample::Source::Predicted,
            true,
            floor_sample->write_generation,
            floor_sample->write_source,
            floor_sample,
            true)) {
        return false;
    }
    appended = true;
    return true;
}

bool ClientDisplaySampler::ensure_predicted_presentation_frame(
    std::uint32_t entity_index,
    SyncFrame frame,
    EntityFrameView& out) const {
    if (predicted_presentation_frames_.view(entity_index, frame, out)) {
        out.presentation_cache_hit = true;
        return true;
    }

    EntityFrameView source_view;
    if (!predicted_frames_.view(entity_index, frame, source_view)) {
        return false;
    }
    EntityBufferedFrame source;
    if (!predicted_frames_.read(entity_index, frame, source)) {
        return false;
    }
    const FramePresentationOrigin presentation_origin{
        true,
        source_view.write_generation,
        source_view.write_source,
        frame_payload_hash(source_view.baseline)};
    predicted_presentation_frames_.write(
        entity_index,
        source,
        FrameWriteSource::PresentationFrame,
        presentation_origin);
    if (!predicted_presentation_frames_.view(entity_index, frame, out)) {
        return false;
    }
    out.presentation_cache_hit = false;
    return true;
}

bool ClientDisplaySampler::append_live_sample(WriteContext& context, const EntityState& state) const {
    if (state.identity.archetype.value >= context.settings.archetypes.size()) {
        return false;
    }

    FractionalTickSample& display = next_sample(context);
    display.client_entity_network_id = state.identity.client_entity_network_id;
    display.local_entity = state.identity.local;
    display.frame = state.replication.frame;
    display.alpha = 0.0f;
    display.source = FractionalTickSample::Source::Live;
    display.mode = state.mode.current;
    display.target_frame = context.target_frame;
    display.target_floor_frame = context.floor_frame;
    display.target_alpha = context.alpha;
    display.target_valid = context.target_valid;
    display.floor_frame_present = true;
    display.next_frame_present = false;
    clear_diagnostics(display);
    write_prediction_diagnostics(display, clock_, prediction_);
    display.source_floor_frame = state.replication.frame;
    display.components_.clear();

    const SyncArchetype& archetype = context.settings.archetypes[state.identity.archetype.value];
    display.components_.reserve(archetype.components.size());
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const ashiato::Entity component = archetype.components[component_index].component;
        if (!context.registry.has<FractionalTickSampled>(component)) {
            continue;
        }
        if (component_index >= archetype.component_ops.size()) {
            return false;
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        if (ops.serialization.quantize == nullptr) {
            return false;
        }
        const void* current = context.registry.get(state.identity.local, component);
        if (current == nullptr) {
            continue;
        }

        ReplicatedComponentUpdate value;
        value.component = component;
        value.serializer = archetype.components[component_index].serializer;
        value.bytes.resize(ops.serialization.quantized_size);
        ops.serialization.quantize(current, value.bytes.data());
        const std::uint64_t floor_hash = hash_bytes(value.bytes.data(), value.bytes.size());
        display.floor_component_hash = combine_hash(display.floor_component_hash, floor_hash);
        const auto found_error = std::find_if(
            state.visual.snap_errors.begin(),
            state.visual.snap_errors.end(),
            [component](const EntityComponentError& error) {
                return error.component == component;
            });
        if (found_error != state.visual.snap_errors.end()) {
            if (ops.apply_error == nullptr ||
                !ops.apply_error(value.bytes.data(), found_error->bytes, value.bytes)) {
                return false;
            }
        }
        display.result_component_hash = combine_hash(
            display.result_component_hash,
            hash_bytes(value.bytes.empty() ? nullptr : value.bytes.data(), value.bytes.size()));
        display.components_.push_back(std::move(value));
        ++display.sampled_component_count;
    }

    ++context.sampled_count;
    return true;
}

bool ClientDisplaySampler::sample_fractional_tick_frame(
    const ashiato::Registry& registry,
    double target_frame,
    FractionalTickSampleBuffer& out) const {
    out.clear();
    const bool target_valid = target_frame >= 0.0 &&
        std::isfinite(target_frame) &&
        target_frame <= static_cast<double>(std::numeric_limits<SyncFrame>::max());
    const double floor_value = target_valid ? std::floor(target_frame) : 0.0;
    const SyncSettings& settings = registry.get<SyncSettings>();
    WriteContext context{
        registry,
        settings,
        out,
        target_frame,
        static_cast<SyncFrame>(floor_value),
        target_valid ? static_cast<float>(target_frame - floor_value) : 0.0f,
        target_valid,
        target_valid || entity_store_.buffered_entity_indices().empty(),
        0};

    for (const std::uint32_t entity_index : entity_store_.active_entity_indices()) {
        if (entity_index >= entity_store_.entity_count()) {
            continue;
        }
        const EntityState& state = entity_store_.state_unchecked(entity_index);
        if (state.identity.client_entity_network_id == invalid_client_entity_network_id) {
            continue;
        }

        if (state.mode.current == ReplicationClientMode::BufferedInterpolation) {
            if (!append_buffered_sample(context, entity_index, state)) {
                context.all_valid = false;
            }
            continue;
        }
        if (!state.identity.local || !registry.alive(state.identity.local)) {
            continue;
        }

        bool appended_predicted = false;
        if (!append_predicted_sample(context, entity_index, state, appended_predicted)) {
            return false;
        }
        if (appended_predicted) {
            continue;
        }
        if (!append_live_sample(context, state)) {
            return false;
        }
    }

    out.entities.resize(context.sampled_count);
    return context.all_valid;
}

}  // namespace ashiato::sync::client_detail
