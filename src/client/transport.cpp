#include "ashiato/sync/client.hpp"

#include "client/store/ack_queue.hpp"
#include "client/runtime/cue_runtime.hpp"
#include "client/store/entity_store.hpp"
#include "client/store/input_buffer.hpp"
#include "client/runtime/prediction_runtime.hpp"
#include "client/session_transport.hpp"
#include "client/state.hpp"
#include "client/timing_stats.hpp"
#include "client/tracing.hpp"
#include "client/runtime/update_runtime.hpp"
#include "detail/logging.hpp"

#include "ashiato/sync/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

namespace ashiato::sync {

namespace {

std::size_t configured_packet_id_bits(const ReplicationClientOptions& options) noexcept {
    return protocol::packet_id_bits(options.network.protocol);
}

double decode_continuous_frame(SyncFrame frame, std::uint16_t subframe) noexcept {
    return static_cast<double>(frame) +
        static_cast<double>(subframe) / static_cast<double>(protocol::frame_subframe_scale);
}

using client_detail::PendingPing;

}  // namespace

std::vector<ashiato::BitBuffer> ReplicationClient::drain_packets() {
    std::vector<ashiato::BitBuffer> packets;
    drain_connect_packets(packets);
    drain_ping_packets(packets);
    drain_input_packets_into(packets);
    drain_ack_packets_into(packets);
    return packets;
}

std::vector<ashiato::BitBuffer> ReplicationClient::drain_ack_packets() {
    std::vector<ashiato::BitBuffer> packets;
    drain_ack_packets_into(packets);
    return packets;
}

void ReplicationClient::process_inbound_packets(ashiato::Registry& registry) {
    for (ashiato::BitBuffer& packet : session_transport_->inbound_packets) {
        (void)receive(registry, std::move(packet));
    }
    session_transport_->inbound_packets.clear();
}

void ReplicationClient::send_pending_packets() {
    if (!session_transport_->packet_sender) {
        return;
    }
    for (const ashiato::BitBuffer& packet : drain_packets()) {
        std::uint8_t message = 0;
        ashiato::BitBuffer inspect = packet;
        message = static_cast<std::uint8_t>(inspect.read_bits(8U));
        try {
            session_transport_->packet_sender(packet);
        } catch (const std::exception& ex) {
            log_client_error(message, "transport_error_client_send", ex.what());
            throw;
        }
    }
}

void ReplicationClient::drain_ack_packets_into(std::vector<ashiato::BitBuffer>& packets) {
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
    std::vector<client_detail::ClientAckPacketTrace> traces;
    ack_queue_->drain_ack_packets(options_.network.mtu_bytes, configured_packet_id_bits(options_), packets, &traces);
    for (const client_detail::ClientAckPacketTrace& trace : traces) {
        trace_outgoing_ack_packet(trace.acks);
    }
#else
    ack_queue_->drain_ack_packets(options_.network.mtu_bytes, configured_packet_id_bits(options_), packets, nullptr);
#endif
}

void ReplicationClient::drain_input_packets_into(std::vector<ashiato::BitBuffer>& packets) {
    if (session_transport_->connection_state != ReplicationClientConnectionState::Ready || !clock_.bootstrapped()) {
        return;
    }
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
    client_detail::ClientInputPacketTrace trace;
    const bool sent = input_->drain_packet(
        options_.network.mtu_bytes,
        configured_packet_id_bits(options_),
        ack_queue_->pending(),
        packets,
        &trace);
    if (sent && trace.sent) {
        trace_outgoing_input_packet(trace.acks, trace.baseline_frame, trace.first_input_frame, trace.last_input_frame);
    }
#else
    (void)input_->drain_packet(
        options_.network.mtu_bytes,
        configured_packet_id_bits(options_),
        ack_queue_->pending(),
        packets,
        nullptr);
#endif
}

std::size_t ReplicationClient::pending_ack_count() const noexcept {
    return ack_queue_->size();
}

void ReplicationClient::queue_ack(std::uint32_t packet_id) {
    ack_queue_->push(packet_id);
}

bool ReplicationClient::receive_connect_response(ashiato::Registry& registry, ashiato::BitBuffer& packet) {
    detail::BitReader reader(packet);
    bool accepted = false;
    if (!reader.read_bits(1U, accepted)) {
        return false;
    }

    if (!accepted) {
        std::string error;
        if (!protocol::read_string(packet, error)) {
            return false;
        }
        client_id_ = invalid_client_id;
        session_transport_->connect_error = std::move(error);
        session_transport_->connection_state = ReplicationClientConnectionState::Rejected;
        ++observability_stats_.client_connects_rejected;
        log_info("client_connect_rejected", "reason=" + detail::log_token(session_transport_->connect_error));
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        if (tracer_ != nullptr && tracer_->enabled()) {
            SyncTraceEvent event = make_client_trace_event(
                SyncTraceEventType::ClientDisconnected,
                invalid_client_id,
                static_cast<SyncFrame>(std::max(0.0, clock_.estimated_server_frame())));
            event.data = session_transport_->connect_error;
            tracer_->trace(event);
        }
#endif
        return true;
    }

    if (!reader.read_bits(64U, client_id_)) {
        return false;
    }
    if (client_id_ == invalid_client_id || client_id_ > max_client_entity_network_id_client) {
        client_id_ = invalid_client_id;
        return false;
    }
    session_transport_->connect_error.clear();
    session_transport_->connection_state = ReplicationClientConnectionState::Accepted;
    session_transport_->connect_resend_accumulator_seconds = options_.session.connect_resend_interval_seconds;
    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Client;
    registry.write<SyncAuthority>().authoritative = false;
    set_client_id(registry, client_id_);
    ++observability_stats_.client_connects_accepted;
    log_info("client_connect_accepted", "client=" + std::to_string(client_id_));
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled()) {
        SyncTraceEvent event = make_client_trace_event(
            SyncTraceEventType::ClientConnected,
            client_id_,
            static_cast<SyncFrame>(std::max(0.0, clock_.estimated_server_frame())));
        tracer_->trace(event);
    }
#endif
    return true;
}

#ifdef ASHIATO_SYNC_ENABLE_TRACING
#define ASHIATO_SYNC_TRACE_RECEIVE_CLOCK_SKEW(stage, server_frame, last_input_frame, prefill_frame) \
    trace_clock_skew( \
        (stage), \
        static_cast<SyncFrame>(std::max(0.0, context.estimated_server_frame)), \
        (server_frame), \
        context.estimated_server_frame, \
        context.buffered_frame, \
        (last_input_frame), \
        (prefill_frame))
#else
#define ASHIATO_SYNC_TRACE_RECEIVE_CLOCK_SKEW(stage, server_frame, last_input_frame, prefill_frame) ((void)0)
#endif

bool ReplicationClient::receive_entity_update(
    ashiato::Registry& registry,
    ashiato::BitBuffer& packet,
    const ReceiveContext& context) {
    detail::BitReader reader(packet);
    SyncFrame frame = 0;
    std::uint32_t packet_id = 0;
    SyncFrame input_ack_frame = 0;
    std::uint16_t record_count = 0;
    if (!reader.read_bits(32U, frame) ||
        !reader.read_bits(configured_packet_id_bits(options_), packet_id) ||
        !reader.read_bits(32U, input_ack_frame) ||
        !reader.read_bits(16U, record_count)) {
        return false;
    }

    cue_runtime_->clear_current_packet_cue_summaries();

    input_->acknowledge_frame(input_ack_frame);
    const bool applied = update_runtime_->apply_update(*this, registry, reader, packet_id, frame, record_count);

#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
    trace_incoming_update_packet(
        static_cast<SyncFrame>(std::max(0.0, context.estimated_server_frame)),
        frame,
        packet_id,
        input_ack_frame,
        record_count);
#endif

    if (!applied) {
        return false;
    }

    session_transport_->connection_state = ReplicationClientConnectionState::Ready;
    last_server_update_frame_ = frame;
    has_received_server_update_ = true;
    input_->retire_transmit_frames_through(frame);
    cue_runtime_->prune_confirmed(frame);
    if (clock_.maybe_bootstrap_from_first_server_update(frame)) {
        const SyncSettings& settings = registry.get<SyncSettings>();
        (void)fill_input_frames_through(registry, settings, clock_.predicted_frame());
    }
    timing_stats_->record_entity_update_packet(packet_id, clock_.mutable_stats());

    update_prediction_input_prefill_from_entity_update(registry, frame, context);
    return true;

}

bool ReplicationClient::update_prediction_input_prefill_from_entity_update(
    ashiato::Registry& registry,
    SyncFrame server_frame,
    const ReceiveContext& context) {
    const SyncFrame decision_last_recorded_input_frame = input_->last_recorded_frame();
    SyncFrame prefill_input_frame = clock_.record_server_update(
        server_frame,
        context.estimated_server_frame,
        context.continuous_buffered_frame,
        context.continuous_predicted_frame,
        has_buffered_entities(),
        decision_last_recorded_input_frame);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    const bool clock_requested_prefill = prefill_input_frame != 0U;
#endif
    if (prefill_input_frame != 0U) {
        (void)prediction_->update_active_snap_lead_from_server_update(
            server_frame,
            true,
            clock_.stats().target_prediction_lead_frames,
            clock_.stats().current_prediction_lead_frames);
    } else {
        prefill_input_frame = prediction_->update_active_snap_lead_from_server_update(
            server_frame,
            false,
            clock_.stats().target_prediction_lead_frames,
            clock_.stats().current_prediction_lead_frames);
    }
    ASHIATO_SYNC_TRACE_RECEIVE_CLOCK_SKEW(
        clock_requested_prefill ? "clock_requested_prefill" :
        (prefill_input_frame != 0U ? "active_snap_topup" : "no_prefill"),
        server_frame,
        decision_last_recorded_input_frame,
        prefill_input_frame);
    if (prefill_input_frame > input_->last_recorded_frame()) {
        (void)apply_prediction_input_prefill(registry, server_frame, prefill_input_frame, 0);
        ASHIATO_SYNC_TRACE_RECEIVE_CLOCK_SKEW(
            "prefill_applied",
            server_frame,
            decision_last_recorded_input_frame,
            prefill_input_frame);
        return true;
    }
    return false;
}

bool ReplicationClient::apply_prediction_input_prefill(
    ashiato::Registry& registry,
    SyncFrame server_frame,
    SyncFrame prefill_input_frame,
    SyncFrame prediction_snap_lead_frames) {
    if (prefill_input_frame <= input_->last_recorded_frame()) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    (void)fill_input_frames_through(registry, settings, prefill_input_frame);
    if (prediction_snap_lead_frames != 0U) {
        prediction_->set_active_snap_lead(prediction_snap_lead_frames);
    }
    prediction_->schedule_catchup(server_frame, prefill_input_frame);
    if (!has_predicted_entities()) {
        clock_.advance_predicted_frame_to(prefill_input_frame);
    }
    clock_.record_prediction_lead(server_frame, prefill_input_frame);
    return true;
}

bool ReplicationClient::receive_pong(
    ashiato::Registry& registry,
    ashiato::BitBuffer& packet,
    const ReceiveContext& context) {
    detail::BitReader reader(packet);
    std::uint32_t sequence = 0;
    SyncFrame server_receive_frame = 0;
    std::uint16_t server_receive_subframe = 0;
    SyncFrame server_send_frame = 0;
    std::uint16_t server_send_subframe = 0;
    if (!reader.read_bits(32U, sequence) ||
        !reader.read_bits(32U, server_receive_frame) ||
        !reader.read_bits(protocol::frame_subframe_bits, server_receive_subframe) ||
        !reader.read_bits(32U, server_send_frame) ||
        !reader.read_bits(protocol::frame_subframe_bits, server_send_subframe)) {
        return false;
    }
    const auto found = session_transport_->pending_pings.find(sequence);
    if (found == session_transport_->pending_pings.end() ||
        context.local_time_seconds < found->second.local_send_time_seconds) {
        return false;
    }
    const double client_send_time_seconds = found->second.local_send_time_seconds;
    session_transport_->pending_pings.erase(found);
    const double server_receive_time_seconds =
        decode_continuous_frame(server_receive_frame, server_receive_subframe) * fixed_dt_seconds_;
    const double server_send_time_seconds =
        decode_continuous_frame(server_send_frame, server_send_subframe) * fixed_dt_seconds_;
    const float sample = static_cast<float>(
        ((context.local_time_seconds - client_send_time_seconds) -
         (server_send_time_seconds - server_receive_time_seconds)) *
        0.5 / fixed_dt_seconds_);
    if (clock_.stats().sample_count == 0U) {
        session_transport_->stable_ping_samples = 1U;
    } else {
        const float delta = std::fabs(sample - clock_.stats().latency_frames);
        if (delta >= options_.session.adaptive_ping_jump_threshold_frames) {
            session_transport_->adaptive_ping_active = true;
            session_transport_->stable_ping_samples = 0U;
        } else if (delta <= options_.session.adaptive_ping_stable_threshold_frames) {
            ++session_transport_->stable_ping_samples;
            if (session_transport_->stable_ping_samples >= options_.session.adaptive_ping_stable_samples) {
                session_transport_->adaptive_ping_active = false;
            }
        } else {
            session_transport_->stable_ping_samples = 0U;
        }
    }
    const ReplicationClientTimingStats before = clock_.stats();
    clock_.record_time_sync_sample(
        client_send_time_seconds,
        server_receive_time_seconds,
        server_send_time_seconds,
        context.local_time_seconds);
    const ReplicationClientTimingStats& after = clock_.stats();
    if (options_.clock.auto_timing_fast_recovery &&
        options_.prediction.auto_lead_frames &&
        has_received_server_update_ &&
        after.target_prediction_lead_frames > before.current_prediction_lead_frames &&
        after.target_prediction_lead_frames - before.current_prediction_lead_frames >=
            options_.clock.auto_timing_fast_recovery_min_frame_gap) {
        const SyncFrame prefill_input_frame = last_server_update_frame_ + after.target_prediction_lead_frames;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        trace_clock_skew(
            "pong_requested_prefill",
            static_cast<SyncFrame>(std::max(0.0, context.estimated_server_frame)),
            last_server_update_frame_,
            context.estimated_server_frame,
            clock_.buffered_frame(),
            input_->last_recorded_frame(),
            prefill_input_frame);
#endif
        if (apply_prediction_input_prefill(
                registry,
                last_server_update_frame_,
                prefill_input_frame,
                after.target_prediction_lead_frames)) {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
            trace_clock_skew(
                "pong_prefill_applied",
                static_cast<SyncFrame>(std::max(0.0, context.estimated_server_frame)),
                last_server_update_frame_,
                context.estimated_server_frame,
                clock_.buffered_frame(),
                input_->last_recorded_frame(),
                prefill_input_frame);
#endif
        }
    }
    return true;
}

void ReplicationClient::drain_connect_packets(std::vector<ashiato::BitBuffer>& packets) {
    if (session_transport_->connection_state == ReplicationClientConnectionState::Connecting) {
        if (session_transport_->sent_initial_connect_request &&
            session_transport_->connect_resend_accumulator_seconds < options_.session.connect_resend_interval_seconds) {
            return;
        }

        ashiato::BitBuffer packet;
        packet.reserve_bytes(options_.network.mtu_bytes);
        packet.push_bits(protocol::client_connect_request_message, 8U);
        protocol::write_string(packet, options_.session.connect_token);
        if (packet.byte_size() <= options_.network.mtu_bytes) {
            packets.push_back(std::move(packet));
            session_transport_->sent_initial_connect_request = true;
            session_transport_->connect_resend_accumulator_seconds = 0.0;
        }
        return;
    }

    if (session_transport_->connection_state == ReplicationClientConnectionState::Accepted && client_id_ != invalid_client_id) {
        if (session_transport_->connect_resend_accumulator_seconds < options_.session.connect_resend_interval_seconds) {
            return;
        }

        ashiato::BitBuffer packet;
        packet.reserve_bytes(options_.network.mtu_bytes);
        packet.push_bits(protocol::client_connect_ack_message, 8U);
        packet.push_unsigned_bits(client_id_, 64U);
        packets.push_back(std::move(packet));
        session_transport_->connect_resend_accumulator_seconds = 0.0;
    }
}

void ReplicationClient::drain_ping_packets(std::vector<ashiato::BitBuffer>& packets) {
    if (session_transport_->connection_state != ReplicationClientConnectionState::Accepted &&
        session_transport_->connection_state != ReplicationClientConnectionState::Ready) {
        return;
    }
    const double interval_seconds = session_transport_->adaptive_ping_active
        ? options_.session.adaptive_ping_interval_seconds
        : options_.session.ping_interval_seconds;
    if (session_transport_->sent_initial_ping && session_transport_->ping_accumulator_seconds < interval_seconds) {
        return;
    }

    const std::uint32_t sequence = session_transport_->next_ping_sequence++;
    session_transport_->pending_pings[sequence] = PendingPing{clock_.local_time_seconds()};

    ashiato::BitBuffer packet;
    packet.reserve_bytes(options_.network.mtu_bytes);
    packet.push_bits(protocol::client_ping_message, 8U);
    packet.push_bits(sequence, 32U);
    packets.push_back(std::move(packet));
    session_transport_->sent_initial_ping = true;
    session_transport_->ping_accumulator_seconds = 0.0;
}

}  // namespace ashiato::sync

#undef ASHIATO_SYNC_TRACE_RECEIVE_CLOCK_SKEW
