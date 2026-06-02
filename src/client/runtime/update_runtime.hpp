#pragma once

#include "client/state.hpp"

#include "ashiato/ashiato.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ashiato::sync {

class ReplicationClient;

namespace detail {
class BitReader;
}  // namespace detail

namespace client_detail {

class ClientUpdateRuntime {
public:
    bool apply_update(
        ReplicationClient& client,
        ashiato::Registry& registry,
        detail::BitReader& packet,
        std::uint32_t packet_id,
        SyncFrame frame,
        std::uint16_t record_count);
    const std::string& last_apply_failure_reason() const noexcept;

private:
    using ComponentBaseline = ReplicatedComponentUpdate;

    struct UpsertMetadata {
        bool is_full_upsert = false;
        SyncFrame frame = 0;
        SyncFrame baseline_frame = 0;
        std::uint32_t wire_network_id = 0;
        ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
        SyncArchetypeId archetype = invalid_sync_archetype_id;
        EntityState* found_state = nullptr;
        EntityState* previous_state = nullptr;
        const QuantizedFrameData* previous_baseline = nullptr;
        bool previous_absent = false;
    };

    struct AuthoritativeUpsertRecord {
        QuantizedFrameData authoritative;
        std::uint64_t changed_sync_slots = 0;
        std::vector<EntityCue>* received_cues = nullptr;
    };

    struct UpsertModeApplyContext {
        std::uint32_t entity_index;
        SyncFrame frame;
        ClientEntityNetworkId client_entity_network_id;
        SyncArchetypeId archetype;
        QuantizedFrameData& authoritative;
        QuantizedFrameData& decoded;
        bool full;
        const std::vector<EntityCue>& received_cues;
    };

    bool apply_upsert(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        std::uint32_t wire_network_id,
        detail::BitReader& packet);
    bool read_upsert_metadata(
        ReplicationClient& client,
        const SyncSettings& settings,
        SyncFrame frame,
        std::uint32_t wire_network_id,
        detail::BitReader& packet,
        UpsertMetadata& metadata);
    bool decode_upsert_record(
        ReplicationClient& client,
        const SyncSettings& settings,
        const UpsertMetadata& metadata,
        detail::BitReader& packet,
        AuthoritativeUpsertRecord& record);
    bool decode_full_upsert_record_payload(
        ReplicationClient& client,
        const SyncSettings& settings,
        const UpsertMetadata& metadata,
        detail::BitReader& packet,
        AuthoritativeUpsertRecord& record);
    bool decode_delta_upsert_record_payload(
        ReplicationClient& client,
        const SyncSettings& settings,
        const UpsertMetadata& metadata,
        detail::BitReader& packet,
        AuthoritativeUpsertRecord& record);
    EntityReferenceContext make_upsert_reference_context(ReplicationClient& client);
    bool read_cues(
        ReplicationClient& client,
        const SyncSettings& settings,
        SyncFrame transmitted_frame,
        detail::BitReader& packet,
        std::vector<EntityCue>& out);
    void trace_cues_received(
        ReplicationClient& client,
        const SyncSettings& settings,
        const UpsertMetadata& metadata,
        const std::vector<EntityCue>& out);
    EntityState* ensure_upsert_entity(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        const UpsertMetadata& metadata,
        const AuthoritativeUpsertRecord& record,
        std::vector<ComponentBaseline>& selector_updates);
    void build_upsert_mode_selector_updates(
        ReplicationClient& client,
        const SyncSettings& settings,
        const UpsertMetadata& metadata,
        const AuthoritativeUpsertRecord& record,
        std::vector<ComponentBaseline>& out) const;
    void trace_received_upsert_record(
        ReplicationClient& client,
        const SyncSettings& settings,
        const UpsertMetadata& metadata,
        const AuthoritativeUpsertRecord& record,
        const EntityState& state);
    bool apply_upsert_record(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const UpsertMetadata& metadata,
        AuthoritativeUpsertRecord& record);
    void finish_upsert(ReplicationClient& client, const UpsertMetadata& metadata);
    void reset_previously_absent_entity(ReplicationClient& client, EntityState& state, SyncArchetypeId archetype);
    ReplicationClientMode select_entity_mode(
        ReplicationClient& client,
        const ReplicatedEntityUpdateView& update,
        const SyncSettings& settings,
        bool mode_needs_selection) const;
    bool apply_destroy(
        ReplicationClient& client,
        ashiato::Registry& registry,
        SyncFrame frame,
        std::uint32_t wire_network_id);
    bool apply_destroy_for_mode(
        ReplicationClient& client,
        ashiato::Registry& registry,
        SyncFrame frame,
        EntityState& state,
        ClientEntityNetworkId client_entity_network_id);
    bool apply_snap_destroy(
        ReplicationClient& client,
        ashiato::Registry& registry,
        SyncFrame frame,
        EntityState& state,
        ClientEntityNetworkId client_entity_network_id);
    bool apply_upsert_for_mode(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const UpsertModeApplyContext& context);
    bool apply_snap_upsert(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const UpsertModeApplyContext& context);
    bool apply_buffered_upsert(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ClientEntityNetworkId client_entity_network_id,
        SyncArchetypeId archetype,
        QuantizedFrameData& decoded);
    bool apply_buffered_destroy(
        ReplicationClient& client,
        ashiato::Registry& registry,
        SyncFrame frame,
        ClientEntityNetworkId client_entity_network_id);
    bool apply_predicted_upsert(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ClientEntityNetworkId client_entity_network_id,
        SyncArchetypeId archetype,
        QuantizedFrameData& authoritative,
        bool full);
    bool apply_predicted_destroy(
        ReplicationClient& client,
        ashiato::Registry& registry,
        SyncFrame frame,
        ClientEntityNetworkId client_entity_network_id);
    void begin_apply_record(std::uint16_t record_index);
    bool fail_apply(const char* reason);
    bool fail_apply_if_empty(const char* reason);

    static constexpr std::uint16_t invalid_apply_record_index = std::numeric_limits<std::uint16_t>::max();
    std::vector<EntityCue> received_cues_scratch_;
    std::vector<ComponentBaseline> selector_updates_scratch_;
    std::string last_apply_failure_reason_;
    std::uint16_t active_apply_record_index_ = invalid_apply_record_index;
};

}  // namespace client_detail
}  // namespace ashiato::sync
