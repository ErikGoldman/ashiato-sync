#include "client/runtime/display_sampler.hpp"

#include "client/store/entity_store.hpp"
#include "client/frame_data.hpp"
#include "client/store/frame_ring_store.hpp"
#include "client/runtime/prediction_runtime.hpp"

#include "ashiato/sync/assert.hpp"
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

detail::FrameDataView frame_data_view(const QuantizedFrameData& frame) noexcept {
    return detail::FrameDataView{
        frame.tag_mask,
        frame.present_mask,
        frame.bytes.empty() ? nullptr : frame.bytes.data(),
        frame.bytes.size()};
}

bool fractional_tick_error_blend_mismatch(const char* reason) {
    (void)reason;
    ASHIATO_SYNC_ASSERT_FAIL(reason);
    return false;
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
    bool boundary_corrected,
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

    const SyncArchetype& archetype = context.settings.archetypes[archetype_id.value];
    display.components_.reserve(archetype.components.size());
    std::size_t sampled_component_count = 0;
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

        if (sampled_component_count == display.components_.size()) {
            display.components_.emplace_back();
        }
        ReplicatedComponentUpdate& value = display.components_[sampled_component_count];
        value.component = component;
        value.serializer = archetype.components[component_index].serializer;
        const bool next_has_component =
            next_frame_sample != nullptr &&
            frame_has_component(next_frame_sample->baseline, component_index);
        const bool component_interpolates =
            archetype.components[component_index].interpolation == ComponentInterpolation::Interpolate;
        if (next_has_component && component_interpolates) {
            if (ops.interpolate == nullptr) {
                return false;
            }
            const std::uint8_t* next_bytes = frame_component_data(archetype, next_frame_sample->baseline, component_index);
            value.bytes.resize(ops.serialization.quantized_size);
            if (next_bytes == nullptr || !ops.interpolate(baseline_bytes, next_bytes, alpha, value.bytes.data())) {
                return false;
            }
        } else {
            value.bytes.assign(baseline_bytes, ops.serialization.quantized_size);
        }

        if (apply_snap_errors && !boundary_corrected) {
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

        ++sampled_component_count;
    }

    display.components_.resize(sampled_component_count);
    if (sampled_component_count != 0U) {
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
            false,
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
            false,
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
        false,
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
    const EntityFrameView* floor_sample = &current_sample;
    const EntityFrameView* next_frame_sample = nullptr;
    EntityFrameView previous_sample;
    bool previous_can_anchor_visual_continuity = false;
    float predicted_alpha = 0.0f;
    if (last_predicted_frame != 0U) {
        const SyncFrame previous_frame = last_predicted_frame - 1U;
        if (ensure_predicted_presentation_frame(
                context,
                entity_index,
                state,
                previous_frame,
                nullptr,
                previous_sample) &&
            previous_sample.entity_present) {
            floor_sample = &previous_sample;
            previous_can_anchor_visual_continuity =
                previous_sample.presentation_boundary_corrected ||
                is_stale_presentation_cache(entity_index, previous_sample);
            predicted_alpha = static_cast<float>(clock_.predicted_accumulator_seconds() / fixed_dt_seconds_);
        }
    }

    if (!ensure_predicted_presentation_frame(
            context,
            entity_index,
            state,
            last_predicted_frame,
            previous_can_anchor_visual_continuity ? floor_sample : nullptr,
            current_sample) ||
        !current_sample.entity_present) {
        return true;
    }
    if (floor_sample != &current_sample) {
        next_frame_sample = &current_sample;
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
            current_sample.presentation_boundary_corrected ||
                is_stale_presentation_cache(entity_index, current_sample),
            true)) {
        return false;
    }
    appended = true;
    return true;
}

bool ClientDisplaySampler::ensure_predicted_presentation_frame(
    const WriteContext& context,
    std::uint32_t entity_index,
    const EntityState& state,
    SyncFrame frame,
    const EntityFrameView* visual_boundary,
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
    bool boundary_corrected = false;
    if (visual_boundary != nullptr &&
        !apply_predicted_presentation_boundary_error(
            context,
            entity_index,
            state,
            *visual_boundary,
            source,
            boundary_corrected)) {
        return false;
    }
    const FramePresentationOrigin presentation_origin{
        true,
        source_view.write_generation,
        source_view.write_source,
        frame_payload_hash(source_view.baseline),
        boundary_corrected};
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

bool ClientDisplaySampler::is_stale_presentation_cache(
    std::uint32_t entity_index,
    const EntityFrameView& sample) const {
    if (!sample.presentation_cache_hit || !sample.presentation_origin_valid) {
        return false;
    }

    EntityFrameView source;
    if (!predicted_frames_.view(entity_index, sample.frame, source)) {
        return true;
    }

    return sample.presentation_origin_generation != source.write_generation ||
        sample.presentation_origin_write_source != source.write_source ||
        sample.presentation_origin_payload_hash != frame_payload_hash(source.baseline);
}

bool ClientDisplaySampler::apply_predicted_presentation_boundary_error(
    const WriteContext& context,
    std::uint32_t entity_index,
    const EntityState& state,
    const EntityFrameView& visual_boundary,
    EntityBufferedFrame& target,
    bool& out_corrected) const {
    out_corrected = false;
    if (state.identity.archetype.value >= context.settings.archetypes.size()) {
        return fractional_tick_error_blend_mismatch("fractional tick error blend archetype missing");
    }

    EntityFrameView source_boundary;
    if (!predicted_frames_.view(
            entity_index,
            visual_boundary.frame,
            source_boundary) ||
        !source_boundary.entity_present) {
        return true;
    }

    const SyncArchetype& archetype = context.settings.archetypes[state.identity.archetype.value];
    if (source_boundary.baseline.byte_count < archetype.total_quantized_bytes ||
        visual_boundary.baseline.byte_count < archetype.total_quantized_bytes ||
        target.baseline.bytes.size() < archetype.total_quantized_bytes) {
        return fractional_tick_error_blend_mismatch("fractional tick error blend frame byte size mismatch");
    }

    const detail::FrameDataView target_view = frame_data_view(target.baseline);
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const ComponentReplication& replication = archetype.components[component_index];
        if (!context.registry.has<FractionalTickSampled>(replication.component)) {
            continue;
        }
        if (component_index >= archetype.component_ops.size()) {
            return fractional_tick_error_blend_mismatch("fractional tick error blend component ops missing");
        }

        const SyncComponentOps& ops = archetype.component_ops[component_index];
        if (ops.compute_error == nullptr ||
            ops.apply_error == nullptr ||
            ops.blend_out_error == nullptr ||
            !frame_has_component(source_boundary.baseline, component_index) ||
            !frame_has_component(visual_boundary.baseline, component_index) ||
            !frame_has_component(target.baseline, component_index)) {
            return fractional_tick_error_blend_mismatch("fractional tick error blend component contract mismatch");
        }

        const std::uint8_t* source_boundary_bytes =
            unchecked_frame_component_data(archetype, source_boundary.baseline, component_index);
        const std::uint8_t* visual_boundary_bytes =
            unchecked_frame_component_data(archetype, visual_boundary.baseline, component_index);
        const std::uint8_t* target_bytes =
            unchecked_frame_component_data(archetype, target_view, component_index);
        std::uint8_t* mutable_target_bytes =
            unchecked_mutable_frame_component_data(archetype, target.baseline, component_index);
        if (source_boundary_bytes == nullptr ||
            visual_boundary_bytes == nullptr ||
            target_bytes == nullptr ||
            mutable_target_bytes == nullptr) {
            return fractional_tick_error_blend_mismatch("fractional tick error blend component bytes missing");
        }

        SyncComponentOps::QuantizedBytes boundary_error;
        if (!ops.compute_error(source_boundary_bytes, visual_boundary_bytes, boundary_error) ||
            boundary_error.size() != ops.error_size) {
            return fractional_tick_error_blend_mismatch("fractional tick error blend compute_error mismatch");
        }

        SyncComponentOps::QuantizedBytes frame_error;
        if (!ops.blend_out_error(boundary_error, static_cast<float>(fixed_dt_seconds_), frame_error) ||
            frame_error.size() != ops.error_size) {
            return fractional_tick_error_blend_mismatch("fractional tick error blend blend_out_error mismatch");
        }

        SyncComponentOps::QuantizedBytes adjusted;
        if (!ops.apply_error(target_bytes, frame_error, adjusted) ||
            adjusted.size() != ops.serialization.quantized_size) {
            return fractional_tick_error_blend_mismatch("fractional tick error blend apply_error mismatch");
        }
        std::copy(adjusted.begin(), adjusted.end(), mutable_target_bytes);
        out_corrected = true;
    }

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

    const SyncArchetype& archetype = context.settings.archetypes[state.identity.archetype.value];
    display.components_.reserve(archetype.components.size());
    std::size_t sampled_component_count = 0;
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

        if (sampled_component_count == display.components_.size()) {
            display.components_.emplace_back();
        }
        ReplicatedComponentUpdate& value = display.components_[sampled_component_count];
        value.component = component;
        value.serializer = archetype.components[component_index].serializer;
        value.bytes.resize(ops.serialization.quantized_size);
        ops.serialization.quantize(current, value.bytes.data());
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
        ++sampled_component_count;
    }

    display.components_.resize(sampled_component_count);
    ++context.sampled_count;
    return true;
}

bool ClientDisplaySampler::sample_fractional_tick_frame(
    const ashiato::Registry& registry,
    double target_frame,
    FractionalTickSampleBuffer& out) const {
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
            out.entities.resize(context.sampled_count);
            return false;
        }
        if (appended_predicted) {
            continue;
        }
        if (!append_live_sample(context, state)) {
            out.entities.resize(context.sampled_count);
            return false;
        }
    }

    out.entities.resize(context.sampled_count);
    return context.all_valid;
}

}  // namespace ashiato::sync::client_detail
