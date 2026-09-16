#include "client/store/input_buffer.hpp"

#include "ashiato/sync/protocol.hpp"
#include "ashiato/sync/serialization.hpp"
#include "ashiato/sync/tracing.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace ashiato::sync::client_detail {

bool ClientInputBuffer::set_latest(
    ashiato::Registry& registry,
    const SyncSettings& settings,
    ashiato::Entity component,
    const void* input) {
    if (input == nullptr || !settings.input_component || settings.input_component != component) {
        return false;
    }
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end() ||
        found_ops->second.serialization.quantize == nullptr ||
        found_ops->second.serialization.serialize == nullptr ||
        found_ops->second.serialization.push_to_registry == nullptr ||
        found_ops->second.serialization.quantized_size == 0U) {
        return false;
    }

    const std::size_t quantized_size = found_ops->second.serialization.quantized_size;
    if (has_ops_ && (component_ != component || frames_.payload_stride() != quantized_size)) {
        frames_.reset();
        last_recorded_frame_ = 0;
        acked_frame_ = 0;
        retired_transmit_frame_ = 0;
        history_discontinuous_ = false;
    }
    component_ = component;
    ops_ = found_ops->second;
    has_ops_ = true;
    latest_.assign(quantized_size, 0U);
    ops_.serialization.quantize(input, latest_.data());
    if (acked_baseline_.size() != quantized_size) {
        acked_baseline_.assign(quantized_size, 0U);
    }
    has_acked_baseline_ = true;
    has_latest_ = true;
    apply_latest_to_owned_entities(registry, settings);
    return true;
}

bool ClientInputBuffer::record_frame(
    const SyncSettings& settings,
    std::size_t capacity_frames,
    SyncFrame frame,
    ClientInputRecord* recorded) {
    if (recorded != nullptr) {
        *recorded = {};
    }
    if (!has_latest_ || !ready_for(settings)) {
        return true;
    }
    if (frame <= acked_frame_) {
        if (recorded != nullptr) {
            recorded->frame = frame;
            recorded->component = component_;
            recorded->bytes = latest_.data();
            if (!frames_.empty()) {
                const std::size_t slot = frames_.slot_for(frame);
                const InputFrameSlot& stored = frames_.metadata(slot);
                if (stored.valid && stored.frame == frame && frames_.payload_stride() == ops_.serialization.quantized_size) {
                    recorded->bytes = frame_bytes(slot);
                }
            }
        }
        return true;
    }
    ensure_capacity(capacity_frames);
    if (frames_.empty()) {
        return true;
    }
    const std::size_t slot = frames_.slot_for(frame);
    InputFrameSlot& stored = frames_.metadata(slot);
    if (stored.valid && stored.frame != frame && stored.frame > acked_frame_) {
        history_discontinuous_ = true;
    }
    stored.frame = frame;
    stored.valid = true;
    std::memcpy(frame_bytes(slot), latest_.data(), frames_.payload_stride());
    last_recorded_frame_ = std::max(last_recorded_frame_, frame);
    if (recorded != nullptr) {
        recorded->frame = frame;
        recorded->component = component_;
        recorded->bytes = frame_bytes(slot);
    }
    return true;
}

bool ClientInputBuffer::fill_frames_through(
    const SyncSettings& settings,
    std::size_t capacity_frames,
    SyncFrame frame,
    std::vector<ClientInputRecord>& recorded) {
    recorded.clear();
    if (!has_latest_ || !ready_for(settings) || frame <= acked_frame_) {
        return true;
    }
    ensure_capacity(capacity_frames);
    if (frames_.empty()) {
        return true;
    }
    const SyncFrame begin = std::max(acked_frame_ + 1U, last_recorded_frame_ + 1U);
    recorded.reserve(recorded.size() + (frame >= begin ? static_cast<std::size_t>(frame - begin + 1U) : 0U));
    for (SyncFrame current = begin; current <= frame; ++current) {
        const std::size_t slot = frames_.slot_for(current);
        InputFrameSlot& stored = frames_.metadata(slot);
        if (stored.valid && stored.frame == current && frames_.payload_stride() == ops_.serialization.quantized_size) {
            last_recorded_frame_ = std::max(last_recorded_frame_, current);
            continue;
        }
        if (stored.valid && stored.frame != current && stored.frame > acked_frame_) {
            history_discontinuous_ = true;
        }
        stored.frame = current;
        stored.valid = true;
        std::memcpy(frame_bytes(slot), latest_.data(), frames_.payload_stride());
        last_recorded_frame_ = std::max(last_recorded_frame_, current);
        recorded.push_back(ClientInputRecord{current, component_, frame_bytes(slot)});
        if (current == frame) {
            break;
        }
    }
    return true;
}

bool ClientInputBuffer::apply_frame(ashiato::Registry& registry, const SyncSettings& settings, SyncFrame frame) const {
    if (!ready_for(settings) || frames_.empty()) {
        return true;
    }
    const std::size_t slot = frames_.slot_for(frame);
    const InputFrameSlot& stored = frames_.metadata(slot);
    if (!stored.valid || stored.frame != frame || frames_.payload_stride() != ops_.serialization.quantized_size) {
        return true;
    }
    apply_quantized_to_owned_entities(registry, settings, frame_bytes(slot));
    return true;
}

void ClientInputBuffer::acknowledge_frame(SyncFrame frame) {
    if (frame <= acked_frame_ || frame > last_recorded_frame_) {
        return;
    }
    bool found_baseline = false;
    if (!frames_.empty()) {
        const std::size_t slot = frames_.slot_for(frame);
        const InputFrameSlot& stored = frames_.metadata(slot);
        if (stored.valid && stored.frame == frame) {
            const std::uint8_t* bytes = frame_bytes(slot);
            acked_baseline_.assign(bytes, bytes + frames_.payload_stride());
            found_baseline = true;
        }
    }
    has_acked_baseline_ = frame == 0U || found_baseline;
    acked_frame_ = frame;
    retired_transmit_frame_ = std::max(retired_transmit_frame_, frame);
}

void ClientInputBuffer::retire_transmit_frames_through(SyncFrame frame) noexcept {
    retired_transmit_frame_ = std::max(retired_transmit_frame_, frame);
}

void ClientInputBuffer::apply_latest_to_owned_entities(ashiato::Registry& registry, const SyncSettings& settings) const {
    if (latest_.empty()) {
        return;
    }
    apply_quantized_to_owned_entities(registry, settings, latest_.data());
}

void ClientInputBuffer::apply_quantized_to_owned_entities(
    ashiato::Registry& registry,
    const SyncSettings& settings,
    const std::uint8_t* quantized) const {
    if (settings.local_client == invalid_client_id ||
        quantized == nullptr ||
        ops_.serialization.push_to_registry == nullptr) {
        return;
    }
    registry.view<const NetworkOwner>().each([&](ashiato::Entity entity, const NetworkOwner& owner) {
        if (owner.client == settings.local_client) {
            (void)ops_.serialization.push_to_registry(registry, entity, quantized);
        }
    });
}

bool ClientInputBuffer::drain_packet(
    std::size_t mtu_bytes,
    std::size_t packet_id_bits,
    std::vector<std::uint32_t>& pending_acks,
    std::vector<ashiato::BitBuffer>& packets,
    ClientInputPacketTrace* trace
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ,
    const SyncTracer* serialization_tracer,
    ClientId client,
    SyncFrame trace_frame
#endif
) {
    if (trace != nullptr) {
        *trace = {};
    }
    const SyncFrame transmit_floor = std::max(acked_frame_, retired_transmit_frame_);
    const bool has_input_frames = has_latest_ && transmit_floor < std::numeric_limits<SyncFrame>::max() &&
        last_recorded_frame_ > transmit_floor;
    if (!has_ops_ || !has_input_frames) {
        return false;
    }

    const std::size_t mtu_bits = mtu_bytes * 8U;
    const std::size_t fixed_header_bits =
        protocol::message_bits + protocol::ack_count_bits + 32U +
        serialization::varint2_raw_bits(true, 0U, 32U) +
        protocol::input_count_bits;
    if (mtu_bits < fixed_header_bits) {
        return false;
    }

    // NEWEST FIRST. The frames the server may still want are transmit_floor + 1 .. last_recorded_frame_, and that
    // window is the whole round trip: a frame stays in it until an acknowledgement a round trip old says it arrived.
    // A packet carries the newest max_frames_per_packet_ of them, as one contiguous run ending at the newest, so what
    // a packet costs does not grow with latency and no cap can leave the newest frame on the client. It used to carry
    // the OLDEST frames and stop at the MTU or the input count, and past a round trip of about 30 frames the server
    // simulated every frame on input it had already used (cockpit, 2026-09-16: 600 of 600 frames starved at a
    // 16-tick link). An older frame left out is either already on the server or was lost in more packets in a row
    // than a packet holds frames; the server takes a first frame that is not acked + 1 as a full frame, and gaps.
    SyncFrame window_first = 0;
    if (!frames_.empty() && frames_.payload_stride() == ops_.serialization.quantized_size) {
        const SyncFrame cap = std::min<SyncFrame>(max_frames_per_packet_, protocol::max_input_count);
        const SyncFrame oldest = last_recorded_frame_ - transmit_floor > cap
            ? last_recorded_frame_ - cap + 1U
            : transmit_floor + 1U;
        for (SyncFrame frame = last_recorded_frame_; frame >= oldest; --frame) {
            const InputFrameSlot& input = frames_.metadata(frames_.slot_for(frame));
            if (!input.valid || input.frame != frame) {
                break;
            }
            window_first = frame;
        }
    }
    const SyncFrame wanted_frames = window_first == 0U ? 0U : last_recorded_frame_ - window_first + 1U;
    const bool baseline_valid = acked_frame_ == 0U || has_acked_baseline_;
    const auto starts_full = [this, baseline_valid](SyncFrame frame) {
        return history_discontinuous_ || frame != acked_frame_ + 1U || !baseline_valid;
    };
    SyncFrame first_input_frame = window_first;
    bool first_input_full = first_input_frame != 0U && starts_full(first_input_frame);

    ashiato::BitBuffer packet;
    packet.reserve_bytes(mtu_bytes);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ScopedSerializationTraceCapture serialization_capture(
        serialization_tracer,
        SyncTracePayloadSource::Network,
        SyncTraceRole::Client,
        client,
        trace_frame,
        "client_input_packet",
        false);
    serialization_capture.set_target(&packet);
    if (serialization_capture.active()) {
        serialization_capture.event().component = component_;
        serialization_capture.event().component_name = ops_.serialization.name;
        add_sync_trace_payload_tag(serialization_capture.event(), sync_trace_payload_tag_incoming);
        serialization_capture.event().data = "message=client_input";
    }
#endif
    std::size_t ack_count_offset = 0;
    {
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, "message_header");
        ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(
            serialization_capture,
            packet,
            protocol::client_input_message,
            protocol::message_bits,
            "message");
        ack_count_offset = packet.bit_size();
        ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, packet, 0, protocol::ack_count_bits, "ack_count");
    }

    std::size_t reserved_first_input_bits = 0;
    if (first_input_frame != 0U) {
        ashiato::BitBuffer first_input_payload;
        const std::uint8_t* previous = first_input_full || acked_baseline_.empty()
            ? nullptr
            : acked_baseline_.data();
        ashiato::ComponentSerializationContext serialization_context;
        serialization_context.currentFrame = first_input_frame;
        serialization_context.previousFrame = previous != nullptr ? acked_frame_ : 0U;
        ops_.serialization.serialize(
            previous,
            frame_bytes(frames_.slot_for(first_input_frame)),
            first_input_payload,
            serialization_context);
        reserved_first_input_bits = first_input_payload.bit_size();
    }

    std::uint16_t ack_count = 0;
    const std::size_t max_acks = std::min<std::size_t>(
        protocol::max_ack_count,
        (mtu_bits - fixed_header_bits) / packet_id_bits);
    while (ack_count < max_acks && ack_count < pending_acks.size()) {
        const std::size_t rollback_bits = packet.bit_size();
        ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, packet, pending_acks[ack_count], packet_id_bits, "ack");
        const std::size_t explicit_first_frame_bits =
            serialization::varint2_raw_bits(!first_input_full, 0U, 32U);
        if (protocol::bytes_for_bits(
                packet.bit_size() + 32U + explicit_first_frame_bits + protocol::input_count_bits +
                    reserved_first_input_bits) >
            mtu_bytes) {
            packet.truncate_bits(rollback_bits);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
            serialization_capture.truncate_to_bits(rollback_bits);
#endif
            break;
        }
        if (trace != nullptr) {
            trace->acks.push_back(pending_acks[ack_count]);
        }
        ++ack_count;
    }
    packet.overwrite_unsigned_bits(ack_count_offset, ack_count, protocol::ack_count_bits);

    // WHEN THE MTU CUTS THE RUN SHORT, THE OLDEST FRAMES GO. The frames are written oldest to newest because each is
    // encoded against the one before it, so a run that does not fit is written again starting as many frames later
    // as were left off the end, until the newest frame is in. Only that rare packet serializes anything twice.
    const std::size_t input_section_offset = packet.bit_size();
    std::size_t input_count_offset = 0;
    std::uint16_t input_count = 0;
    SyncFrame last_input_frame = 0;
    for (;;) {
        {
            ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, "input_header");
            ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, packet, acked_frame_, 32U, "acked_frame");
            {
                ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, "first_input_frame");
                serialization::serialize_varint2_raw(
                    packet,
                    !first_input_full,
                    0U,
                    0U,
                    first_input_frame,
                    32U);
            }
        }

        {
            ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, "input_count");
            input_count_offset = packet.bit_size();
            ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, packet, 0, protocol::input_count_bits, "input_count");
        }

        input_count = 0;
        last_input_frame = 0;
        const std::uint8_t* previous = first_input_full || acked_baseline_.empty()
            ? nullptr
            : acked_baseline_.data();
        if (first_input_frame != 0U) {
            for (SyncFrame frame = first_input_frame; frame <= last_recorded_frame_; ++frame) {
                if (frames_.empty()) {
                    break;
                }
                const std::size_t slot = frames_.slot_for(frame);
                const InputFrameSlot& input = frames_.metadata(slot);
                if (!input.valid || input.frame != frame || frames_.payload_stride() != ops_.serialization.quantized_size) {
                    break;
                }

                const std::uint8_t* input_bytes = frame_bytes(slot);
                const std::size_t rollback_bits = packet.bit_size();
#ifdef ASHIATO_SYNC_ENABLE_TRACING
                ashiato::ComponentSerializationContext serialization_context{nullptr, serialization_capture.payload_capture()};
                serialization_context.currentFrame = frame;
                serialization_context.previousFrame = previous != nullptr ? frame - 1U : 0U;
                {
                    ScopedSerializationTraceScope input_frame_scope(&serialization_capture, "input_frame");
                    ops_.serialization.serialize(previous, input_bytes, packet, serialization_context);
                }
#else
                ashiato::ComponentSerializationContext serialization_context{nullptr};
                serialization_context.currentFrame = frame;
                serialization_context.previousFrame = previous != nullptr ? frame - 1U : 0U;
                ops_.serialization.serialize(previous, input_bytes, packet, serialization_context);
#endif
                if (protocol::bytes_for_bits(packet.bit_size()) > mtu_bytes) {
                    packet.truncate_bits(rollback_bits);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
                    serialization_capture.truncate_to_bits(rollback_bits);
#endif
                    break;
                }
                previous = input_bytes;
                last_input_frame = frame;
                ++input_count;
                if (input_count == protocol::max_input_count) {
                    break;
                }
            }
        }
        if (input_count == 0U || last_input_frame >= last_recorded_frame_) {
            break;
        }
        first_input_frame += last_recorded_frame_ - last_input_frame;
        first_input_full = starts_full(first_input_frame);
        packet.truncate_bits(input_section_offset);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        serialization_capture.truncate_to_bits(input_section_offset);
#endif
    }

    // COUNTED, because a cut here is silent on the wire and costs the server stale input: only the MTU or the input
    // count cutting below the frames this packet meant to carry. Frames max_frames_per_packet_ leaves out are not.
    if (input_count < wanted_frames) {
        ++truncated_packets_;
        truncated_frames_ += wanted_frames - input_count;
    }

    if (ack_count == 0U && input_count == 0U) {
        return false;
    }
    packet.overwrite_unsigned_bits(input_count_offset, input_count, protocol::input_count_bits);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    serialization_capture.flush();
#endif
    packets.push_back(std::move(packet));
    if (input_count != 0U) {
        history_discontinuous_ = false;
    }
    if (ack_count != 0U) {
        pending_acks.erase(pending_acks.begin(), pending_acks.begin() + ack_count);
    }
    if (trace != nullptr) {
        trace->baseline_frame = acked_frame_;
        trace->first_input_frame = first_input_frame;
        trace->last_input_frame = last_input_frame;
        trace->sent = true;
    }
    return true;
}

bool ClientInputBuffer::ready_for(const SyncSettings& settings) const noexcept {
    return has_ops_ && settings.input_component && settings.input_component == component_;
}

void ClientInputBuffer::ensure_capacity(std::size_t capacity_frames) {
    if (!has_ops_ || ops_.serialization.quantized_size == 0U) {
        return;
    }
    if (frames_.empty()) {
        frames_.ensure(capacity_frames);
        frames_.ensure_payload_stride(ops_.serialization.quantized_size);
    }
}

std::uint8_t* ClientInputBuffer::frame_bytes(std::size_t slot) noexcept {
    return frames_.payload(slot);
}

const std::uint8_t* ClientInputBuffer::frame_bytes(std::size_t slot) const noexcept {
    return frames_.payload(slot);
}

}  // namespace ashiato::sync::client_detail
