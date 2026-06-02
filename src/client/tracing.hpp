#pragma once

#include "ashiato/sync/client.hpp"
#include "ashiato/sync/tracing.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ashiato::sync {

#ifdef ASHIATO_SYNC_ENABLE_TRACING

SyncTraceEvent make_client_trace_event(SyncTraceEventType type, ClientId client, SyncFrame frame);

struct RollbackReasonTraceContext {
    const SyncTracer* tracer = nullptr;
    ClientId client = invalid_client_id;
    SyncFrame frame = 0;
    ashiato::Entity server_entity{};
    ashiato::Entity local_entity{};
    ClientEntityNetworkId client_network_id = invalid_client_entity_network_id;
    std::uint32_t wire_network_id = 0;
    std::uint32_t network_version = 0;
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    ashiato::Entity component{};
    std::string component_name;
};

class ScopedRollbackReasonTraceContext {
public:
    explicit ScopedRollbackReasonTraceContext(const RollbackReasonTraceContext& context);
    ~ScopedRollbackReasonTraceContext();

    ScopedRollbackReasonTraceContext(const ScopedRollbackReasonTraceContext&) = delete;
    ScopedRollbackReasonTraceContext& operator=(const ScopedRollbackReasonTraceContext&) = delete;

private:
    const RollbackReasonTraceContext* previous_ = nullptr;
};

void append_trace_component_data(
    const SyncTracer* tracer,
    const SyncArchetype& archetype,
    std::size_t component_index,
    const std::uint8_t* bytes,
    SyncTraceEvent& event);
void append_trace_input_component_data(
    const SyncTracer* tracer,
    const SyncComponentOps& ops,
    const std::uint8_t* bytes,
    SyncTraceEvent& event);
void append_trace_component_name(
    const SyncArchetype& archetype,
    std::size_t component_index,
    SyncTraceEvent& event);
void append_trace_cue_data(
    const SyncTracer* tracer,
    const SyncSettings& settings,
    SyncCueTypeId cue_type,
    const ashiato::BitBuffer& payload,
    SyncTraceEvent& event);
void append_trace_cue_value(
    const SyncTracer* tracer,
    const SyncSettings& settings,
    SyncCueTypeId cue_type,
    const CueValue& value,
    SyncTraceEvent& event);
void append_trace_data_field(SyncTraceEvent& event, const char* key, const char* value);
void append_trace_cue_name(const SyncSettings& settings, SyncCueTypeId cue_type, SyncTraceEvent& event);

#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS
std::string packet_ack_list(const std::vector<std::uint32_t>& acks);
#endif

#endif

}  // namespace ashiato::sync
