#pragma once

#include "client/state.hpp"

#include "ashiato/sync/client.hpp"
#include "ashiato/sync/client_clock.hpp"

#include <cstddef>
#include <cstdint>

namespace ashiato::sync::client_detail {

class ClientEntityStore;
class ClientFrameRingStore;
class ClientPredictionRuntime;
enum class FrameWriteSource : std::uint8_t;

class ClientDisplaySampler {
public:
    ClientDisplaySampler(
        const ReplicationClientClock& clock,
        const ClientEntityStore& entity_store,
        ClientPredictionRuntime& prediction,
        const ClientFrameRingStore& buffered_frames,
        const ClientFrameRingStore& predicted_frames,
        ClientFrameRingStore& predicted_presentation_frames,
        const FractionalTickSampleBuffer& previous_frame,
        double fixed_dt_seconds) noexcept;

    bool sample_fractional_tick_frame(
        const ashiato::Registry& registry,
        double target_frame,
        FractionalTickSampleBuffer& out) const;

private:
    struct WriteContext {
        const ashiato::Registry& registry;
        const SyncSettings& settings;
        FractionalTickSampleBuffer& out;
        double target_frame = 0.0;
        SyncFrame floor_frame = 0;
        float alpha = 0.0f;
        bool target_valid = false;
        bool all_valid = true;
        std::size_t sampled_count = 0;
    };

    const FractionalTickSample* find_previous_sample(ClientEntityNetworkId client_entity_network_id) const;
    FractionalTickSample& next_sample(WriteContext& context) const;
    bool append_previous_sample(WriteContext& context, ClientEntityNetworkId client_entity_network_id) const;
    bool append_frame_sample(
        WriteContext& context,
        const EntityState& state,
        detail::FrameDataView baseline,
        SyncArchetypeId archetype_id,
        SyncFrame frame,
        float alpha,
        const EntityFrameView* next_sample,
        FractionalTickSample::Source source,
        bool floor_frame_present,
        bool apply_snap_errors) const;
    bool append_latest_buffered_sample(WriteContext& context, const EntityState& state) const;
    bool append_missing_buffered_sample(WriteContext& context, const EntityState& state) const;
    bool append_buffered_sample(WriteContext& context, std::uint32_t entity_index, const EntityState& state) const;
    bool append_predicted_sample(
        WriteContext& context,
        std::uint32_t entity_index,
        const EntityState& state,
        bool& appended) const;
    bool ensure_predicted_presentation_frame(
        std::uint32_t entity_index,
        SyncFrame frame,
        EntityFrameView& out) const;
    bool append_live_sample(WriteContext& context, const EntityState& state) const;

    const ReplicationClientClock& clock_;
    const ClientEntityStore& entity_store_;
    ClientPredictionRuntime& prediction_;
    const ClientFrameRingStore& buffered_frames_;
    const ClientFrameRingStore& predicted_frames_;
    ClientFrameRingStore& predicted_presentation_frames_;
    const FractionalTickSampleBuffer& previous_frame_;
    double fixed_dt_seconds_ = 1.0 / 60.0;
};

}  // namespace ashiato::sync::client_detail
