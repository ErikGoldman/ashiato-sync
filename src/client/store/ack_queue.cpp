#include "client/store/ack_queue.hpp"

#include "ashiato/sync/tracing.hpp"

#include <algorithm>
#include <limits>
#include <optional>

namespace ashiato::sync::client_detail {

void ClientAckQueue::push(std::uint32_t packet_id) {
    pending_.push_back(packet_id);
}

std::size_t ClientAckQueue::size() const noexcept {
    return pending_.size();
}

std::vector<std::uint32_t>& ClientAckQueue::pending() noexcept {
    return pending_;
}

void ClientAckQueue::drain_ack_packets(
    std::size_t mtu_bytes,
    std::size_t packet_id_bits,
    std::vector<ashiato::BitBuffer>& packets,
    std::vector<ClientAckPacketTrace>* traces
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ,
    const SyncTracer* serialization_tracer,
    ClientId client,
    SyncFrame frame
#endif
) {
    if (pending_.empty()) {
        pending_.clear();
        return;
    }

    const std::size_t one_ack_bytes = protocol::bytes_for_bits(protocol::client_ack_header_bits + packet_id_bits);
    if (one_ack_bytes > mtu_bytes) {
        return;
    }

    const std::size_t mtu_bits = mtu_bytes * 8U;
    const std::size_t max_acks_per_packet =
        std::min<std::size_t>(
            std::numeric_limits<std::uint16_t>::max(),
            (mtu_bits - protocol::client_ack_header_bits) / packet_id_bits);
    if (max_acks_per_packet == 0U) {
        return;
    }

    packets.reserve(packets.size() + (pending_.size() + max_acks_per_packet - 1U) / max_acks_per_packet);
    ashiato::BitBuffer packet;
    std::uint16_t packet_ack_count = 0;
    std::size_t packet_count_offset = 0;
    ClientAckPacketTrace trace;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    std::optional<ScopedSerializationTraceCapture> serialization_capture;
    const bool capture_serialization =
        serialization_tracer != nullptr &&
        serialization_tracer->enabled() &&
        serialization_tracer->serialization_payloads_enabled() &&
        serialization_tracer->traces_client(client);
#endif
    auto begin_packet = [&]() {
        packet.clear();
        packet.reserve_bytes(mtu_bytes);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        if (capture_serialization) {
            serialization_capture.emplace(
                serialization_tracer,
                SyncTracePayloadSource::Network,
                SyncTraceRole::Client,
                client,
                frame,
                "client_ack_packet",
                false);
            serialization_capture->set_target(&packet);
            if (serialization_capture->active()) {
                add_sync_trace_payload_tag(serialization_capture->event(), sync_trace_payload_tag_incoming);
                serialization_capture->event().data = "message=client_ack";
            }
        }
        ScopedSerializationTraceScope header_scope(
            serialization_capture ? &*serialization_capture : nullptr,
            "header");
#endif
        SERIALIZE_TRACE_WITH_CAPTURE(
            serialization_capture ? &*serialization_capture : nullptr,
            packet,
            protocol::client_ack_message,
            8U,
            "message");
        packet_count_offset = packet.bit_size();
        SERIALIZE_TRACE_WITH_CAPTURE(
            serialization_capture ? &*serialization_capture : nullptr,
            packet,
            0,
            16U,
            "ack_count");
        packet_ack_count = 0;
        trace.acks.clear();
    };
    auto finish_packet = [&]() {
        if (packet_ack_count == 0U) {
            return;
        }
        packet.overwrite_unsigned_bits(packet_count_offset, packet_ack_count, 16U);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        if (serialization_capture) {
            serialization_capture->flush();
            serialization_capture.reset();
        }
#endif
        if (traces != nullptr) {
            traces->push_back(trace);
        }
        packets.push_back(std::move(packet));
        packet = ashiato::BitBuffer{};
    };

    begin_packet();
    for (const std::uint32_t packet_id : pending_) {
        if (packet_ack_count == max_acks_per_packet) {
            finish_packet();
            begin_packet();
        }
        SERIALIZE_TRACE_WITH_CAPTURE(
            serialization_capture ? &*serialization_capture : nullptr,
            packet,
            packet_id,
            packet_id_bits,
            "ack");
        trace.acks.push_back(packet_id);
        ++packet_ack_count;
    }
    finish_packet();

    pending_.clear();
}

}  // namespace ashiato::sync::client_detail
