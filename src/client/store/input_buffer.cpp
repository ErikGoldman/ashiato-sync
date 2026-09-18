#include "client/store/input_buffer.hpp"

#include "ashiato/sync/protocol.hpp"
#include "ashiato/sync/serialization.hpp"
#include "ashiato/sync/tracing.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace ashiato::sync::client_detail {

struct ClientInputBuffer::InputFrameRange {
    SyncFrame first = 0;
    SyncFrame last = 0;

    SyncFrame count() const noexcept {
        return first == 0U ? 0U : last - first + 1U;
    }

    InputFrameRange trailing(SyncFrame frame_count) const noexcept {
        return InputFrameRange{last - frame_count + 1U, last};
    }
};

struct ClientInputBuffer::InputWriteResult {
    std::size_t count_offset = 0;
    std::uint16_t count = 0;
    SyncFrame first_frame = 0;
    SyncFrame last_frame = 0;

    bool empty() const noexcept {
        return count == 0U;
    }

    bool ends_at(SyncFrame frame) const noexcept {
        return last_frame == frame;
    }
};

class ClientInputBuffer::InputPacketWriter {
public:
    InputPacketWriter(
        ClientInputBuffer& input,
        std::size_t mtu_bytes,
        std::size_t packet_id_bits
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        ,
        ScopedSerializationTraceCapture& serialization_capture
#endif
    );

    void append_header();
    std::uint16_t append_acks(
        const std::vector<std::uint32_t>& pending_acks,
        InputFrameRange desired_input_range);
    InputWriteResult append_newest_input_suffix(InputFrameRange desired_input_range);
    ashiato::BitBuffer finish();

private:
    std::size_t serialized_first_input_bits(InputFrameRange range) const;
    InputWriteResult write_input_attempt(InputFrameRange range);
    void serialize_input_frame(
        SyncFrame frame,
        const std::uint8_t* previous,
        const std::uint8_t* current);
    void truncate_to(std::size_t bit_offset);

    ClientInputBuffer& input_;
    std::size_t mtu_bytes_ = 0;
    std::size_t packet_id_bits_ = 0;
    ashiato::BitBuffer packet_;
    std::size_t ack_count_offset_ = 0;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ScopedSerializationTraceCapture& serialization_capture_;
#endif
};

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

ClientInputBuffer::InputPacketWriter::InputPacketWriter(
    ClientInputBuffer& input,
    std::size_t mtu_bytes,
    std::size_t packet_id_bits
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ,
    ScopedSerializationTraceCapture& serialization_capture
#endif
    )
    : input_(input),
      mtu_bytes_(mtu_bytes),
      packet_id_bits_(packet_id_bits)
#ifdef ASHIATO_SYNC_ENABLE_TRACING
      ,
      serialization_capture_(serialization_capture)
#endif
{
    packet_.reserve_bytes(mtu_bytes_);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    serialization_capture_.set_target(&packet_);
#endif
}

void ClientInputBuffer::InputPacketWriter::append_header() {
    ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture_, "message_header");
    ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(
        serialization_capture_,
        packet_,
        protocol::client_input_message,
        protocol::message_bits,
        "message");
    ack_count_offset_ = packet_.bit_size();
    ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(
        serialization_capture_,
        packet_,
        0,
        protocol::ack_count_bits,
        "ack_count");
}

std::size_t ClientInputBuffer::InputPacketWriter::serialized_first_input_bits(InputFrameRange range) const {
    if (range.first == 0U) {
        return 0U;
    }
    const bool first_input_full = input_.must_encode_first_frame_in_full(range.first);
    const std::uint8_t* previous = first_input_full || input_.acked_baseline_.empty()
        ? nullptr
        : input_.acked_baseline_.data();
    ashiato::BitBuffer payload;
    ashiato::ComponentSerializationContext serialization_context;
    serialization_context.currentFrame = range.first;
    serialization_context.previousFrame = previous != nullptr ? input_.acked_frame_ : 0U;
    input_.ops_.serialization.serialize(
        previous,
        input_.frame_bytes(input_.frames_.slot_for(range.first)),
        payload,
        serialization_context);
    return payload.bit_size();
}

std::uint16_t ClientInputBuffer::InputPacketWriter::append_acks(
    const std::vector<std::uint32_t>& pending_acks,
    InputFrameRange desired_input_range) {
    const std::size_t fixed_header_bits =
        protocol::message_bits + protocol::ack_count_bits + 32U +
        serialization::varint2_raw_bits(true, 0U, 32U) +
        protocol::input_count_bits;
    const std::size_t max_acks = std::min<std::size_t>(
        protocol::max_ack_count,
        ((mtu_bytes_ * 8U) - fixed_header_bits) / packet_id_bits_);
    const bool first_input_full = desired_input_range.first != 0U &&
        input_.must_encode_first_frame_in_full(desired_input_range.first);
    const std::size_t first_input_bits = serialized_first_input_bits(desired_input_range);

    std::uint16_t ack_count = 0;
    while (ack_count < max_acks && ack_count < pending_acks.size()) {
        const std::size_t ack_offset = packet_.bit_size();
        ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(
            serialization_capture_,
            packet_,
            pending_acks[ack_count],
            packet_id_bits_,
            "ack");
        const std::size_t explicit_first_frame_bits =
            serialization::varint2_raw_bits(!first_input_full, 0U, 32U);
        if (protocol::bytes_for_bits(
                packet_.bit_size() + 32U + explicit_first_frame_bits + protocol::input_count_bits +
                    first_input_bits) >
            mtu_bytes_) {
            truncate_to(ack_offset);
            break;
        }
        ++ack_count;
    }
    packet_.overwrite_unsigned_bits(ack_count_offset_, ack_count, protocol::ack_count_bits);
    return ack_count;
}

void ClientInputBuffer::InputPacketWriter::serialize_input_frame(
    SyncFrame frame,
    const std::uint8_t* previous,
    const std::uint8_t* current) {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ashiato::ComponentSerializationContext serialization_context{
        nullptr,
        serialization_capture_.payload_capture()};
    serialization_context.currentFrame = frame;
    serialization_context.previousFrame = previous != nullptr ? frame - 1U : 0U;
    {
        ScopedSerializationTraceScope input_frame_scope(&serialization_capture_, "input_frame");
        input_.ops_.serialization.serialize(previous, current, packet_, serialization_context);
    }
#else
    ashiato::ComponentSerializationContext serialization_context{nullptr};
    serialization_context.currentFrame = frame;
    serialization_context.previousFrame = previous != nullptr ? frame - 1U : 0U;
    input_.ops_.serialization.serialize(previous, current, packet_, serialization_context);
#endif
}

ClientInputBuffer::InputWriteResult ClientInputBuffer::InputPacketWriter::write_input_attempt(
    InputFrameRange range) {
    InputWriteResult result;
    result.first_frame = range.first;
    const bool first_input_full = range.first != 0U && input_.must_encode_first_frame_in_full(range.first);
    {
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture_, "input_header");
        ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(
            serialization_capture_,
            packet_,
            input_.acked_frame_,
            32U,
            "acked_frame");
        {
            ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture_, "first_input_frame");
            serialization::serialize_varint2_raw(packet_, !first_input_full, 0U, 0U, range.first, 32U);
        }
    }

    {
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture_, "input_count");
        result.count_offset = packet_.bit_size();
        ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(
            serialization_capture_,
            packet_,
            0,
            protocol::input_count_bits,
            "input_count");
    }

    const std::uint8_t* previous = first_input_full || input_.acked_baseline_.empty()
        ? nullptr
        : input_.acked_baseline_.data();
    for (SyncFrame frame = range.first; frame != 0U && frame <= range.last; ++frame) {
        const std::size_t slot = input_.frames_.slot_for(frame);
        const InputFrameSlot& input = input_.frames_.metadata(slot);
        if (!input.valid || input.frame != frame ||
            input_.frames_.payload_stride() != input_.ops_.serialization.quantized_size) {
            break;
        }

        const std::uint8_t* input_bytes = input_.frame_bytes(slot);
        const std::size_t frame_offset = packet_.bit_size();
        serialize_input_frame(frame, previous, input_bytes);
        if (protocol::bytes_for_bits(packet_.bit_size()) > mtu_bytes_) {
            truncate_to(frame_offset);
            break;
        }
        previous = input_bytes;
        result.last_frame = frame;
        ++result.count;
    }
    return result;
}

ClientInputBuffer::InputWriteResult ClientInputBuffer::InputPacketWriter::append_newest_input_suffix(
    InputFrameRange desired_input_range) {
    const std::size_t input_section_offset = packet_.bit_size();
    InputFrameRange candidate_input_range = desired_input_range;
    InputWriteResult result;
    for (;;) {
        truncate_to(input_section_offset);
        result = write_input_attempt(candidate_input_range);
        if (result.empty() || result.ends_at(desired_input_range.last)) {
            break;
        }
        candidate_input_range = desired_input_range.trailing(result.count);
    }
    packet_.overwrite_unsigned_bits(result.count_offset, result.count, protocol::input_count_bits);
    return result;
}

void ClientInputBuffer::InputPacketWriter::truncate_to(std::size_t bit_offset) {
    packet_.truncate_bits(bit_offset);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    serialization_capture_.truncate_to_bits(bit_offset);
#endif
}

ashiato::BitBuffer ClientInputBuffer::InputPacketWriter::finish() {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    serialization_capture_.flush();
#endif
    return std::move(packet_);
}

ClientInputBuffer::InputFrameRange ClientInputBuffer::find_newest_contiguous_input_range(
    SyncFrame transmit_floor) const {
    InputFrameRange range;
    if (frames_.empty() || frames_.payload_stride() != ops_.serialization.quantized_size) {
        return range;
    }
    constexpr SyncFrame cap = protocol::max_input_count;
    const SyncFrame oldest = last_recorded_frame_ - transmit_floor > cap
        ? last_recorded_frame_ - cap + 1U
        : transmit_floor + 1U;
    for (SyncFrame frame = last_recorded_frame_; frame >= oldest; --frame) {
        const InputFrameSlot& input = frames_.metadata(frames_.slot_for(frame));
        if (!input.valid || input.frame != frame) {
            break;
        }
        range = InputFrameRange{frame, last_recorded_frame_};
    }
    return range;
}

bool ClientInputBuffer::must_encode_first_frame_in_full(SyncFrame frame) const noexcept {
    const bool baseline_valid = acked_frame_ == 0U || has_acked_baseline_;
    return history_discontinuous_ || frame != acked_frame_ + 1U || !baseline_valid;
}

void ClientInputBuffer::record_input_truncation(
    InputFrameRange desired,
    const InputWriteResult& written) noexcept {
    if (written.count < desired.count()) {
        ++truncated_packets_;
        truncated_frames_ += desired.count() - written.count;
    }
}

bool ClientInputBuffer::append_input_packet(
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
    const std::size_t fixed_header_bits =
        protocol::message_bits + protocol::ack_count_bits + 32U +
        serialization::varint2_raw_bits(true, 0U, 32U) +
        protocol::input_count_bits;
    if (!has_ops_ || !has_input_frames || mtu_bytes * 8U < fixed_header_bits) {
        return false;
    }

    const InputFrameRange desired_input_range = find_newest_contiguous_input_range(transmit_floor);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ScopedSerializationTraceCapture serialization_capture(
        serialization_tracer,
        SyncTracePayloadSource::Network,
        SyncTraceRole::Client,
        client,
        trace_frame,
        "client_input_packet",
        false);
    if (serialization_capture.active()) {
        serialization_capture.event().component = component_;
        serialization_capture.event().component_name = ops_.serialization.name;
        add_sync_trace_payload_tag(serialization_capture.event(), sync_trace_payload_tag_incoming);
        serialization_capture.event().data = "message=client_input";
    }
#endif

    InputPacketWriter writer(
        *this,
        mtu_bytes,
        packet_id_bits
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        ,
        serialization_capture
#endif
    );
    writer.append_header();
    const std::uint16_t ack_count = writer.append_acks(pending_acks, desired_input_range);
    const InputWriteResult input_write = writer.append_newest_input_suffix(desired_input_range);
    record_input_truncation(desired_input_range, input_write);
    if (ack_count == 0U && input_write.empty()) {
        return false;
    }

    packets.push_back(writer.finish());
    if (!input_write.empty()) {
        history_discontinuous_ = false;
    }
    if (trace != nullptr) {
        trace->acks.assign(pending_acks.begin(), pending_acks.begin() + ack_count);
        trace->baseline_frame = acked_frame_;
        trace->first_input_frame = input_write.first_frame;
        trace->last_input_frame = input_write.last_frame;
        trace->sent = true;
    }
    if (ack_count != 0U) {
        pending_acks.erase(pending_acks.begin(), pending_acks.begin() + ack_count);
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
