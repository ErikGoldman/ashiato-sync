#include "server/input_buffer.hpp"

#include "ashiato/sync/detail/bit_reader.hpp"
#include "ashiato/sync/protocol.hpp"
#include "ashiato/sync/serialization.hpp"

#include <algorithm>

namespace ashiato::sync::server_detail {

bool ServerInputBuffer::set_local_frame(
    SyncFrame frame,
    const std::uint8_t* bytes,
    std::size_t quantized_size,
    std::size_t capacity_frames) {
    if (frame == 0U || bytes == nullptr || quantized_size == 0U || capacity_frames == 0U) {
        return false;
    }
    if (input_frames_.empty()) {
        input_frames_.resize(capacity_frames);
    }
    if (input_frames_.empty()) {
        return false;
    }

    InputFrame& stored = input_frames_[frame & (input_frames_.size() - 1U)];
    stored.frame = frame;
    stored.valid = true;
    stored.bytes.assign(bytes, bytes + quantized_size);
    latest_input_ = stored.bytes;
    input_ack_frame_ = std::max(input_ack_frame_, frame);
    has_latest_input_ = true;
    stats_.latest_received_input_frame = std::max(stats_.latest_received_input_frame, frame);
    return true;
}

bool ServerInputBuffer::process_packet_payload(
    ashiato::BitBuffer& packet,
    const SyncComponentOps& ops,
    std::size_t capacity_frames,
    ServerInputPacketTrace* trace) {
    if (trace != nullptr) {
        *trace = {};
    }
    detail::BitReader reader(packet);
    SyncFrame baseline_frame = 0;
    bool first_input_relative = false;
    std::uint64_t first_input_value = 0;
    if (!reader.read_bits(32U, baseline_frame) ||
        !serialization::read_varint2_raw(reader, 0U, 32U, first_input_relative, first_input_value)) {
        return false;
    }
    SyncFrame first_input_frame = baseline_frame + 1U;
    const bool first_input_full = !first_input_relative;
    if (first_input_full) {
        first_input_frame = static_cast<SyncFrame>(first_input_value);
    }
    if (trace != nullptr) {
        trace->baseline_frame = baseline_frame;
    }
    std::uint16_t input_count = 0;
    if (!reader.read_bits(protocol::input_count_bits, input_count)) {
        return false;
    }
    if (input_count > protocol::max_input_count) {
        return false;
    }

    std::vector<std::uint8_t> previous(ops.serialization.quantized_size, 0U);
    if (baseline_frame > input_ack_frame_) {
        input_ack_frame_ = baseline_frame;
    }
    if (baseline_frame != 0U && !first_input_full) {
        bool found_baseline = false;
        if (!input_frames_.empty()) {
            const InputFrame& baseline = input_frames_[baseline_frame & (input_frames_.size() - 1U)];
            if (baseline.valid && baseline.frame == baseline_frame &&
                baseline.bytes.size() == ops.serialization.quantized_size) {
                previous = baseline.bytes;
                found_baseline = true;
            }
        }
        if (!found_baseline && has_latest_input_ && baseline_frame == input_ack_frame_ &&
            latest_input_.size() == ops.serialization.quantized_size) {
            previous = latest_input_;
            found_baseline = true;
        }
        if (!found_baseline) {
            return true;
        }
    }
    if (first_input_frame == 0U || input_count == 0U) {
        return true;
    }

    if (input_frames_.empty()) {
        input_frames_.resize(capacity_frames);
    }

    std::vector<std::uint8_t> decoded(ops.serialization.quantized_size, 0U);
    for (std::uint16_t index = 0; index < input_count; ++index) {
        const SyncFrame frame = first_input_frame + static_cast<SyncFrame>(index);
        const std::uint8_t* previous_bytes = index == 0U && first_input_full ? nullptr : previous.data();
        ashiato::ComponentSerializationContext serialization_context;
        serialization_context.currentFrame = frame;
        serialization_context.previousFrame = previous_bytes != nullptr ? frame - 1U : 0U;
        const std::uint64_t before_bits = reader.raw().read_offset_bits();
        if (!ops.serialization.deserialize(reader.raw(), previous_bytes, decoded.data(), serialization_context)) {
            return false;
        }
        if (reader.raw().read_offset_bits() == before_bits) {
            return false;
        }
        if (trace != nullptr) {
            if (index == 0U) {
                trace->first_input_frame = frame;
            }
            trace->last_input_frame = frame;
        }

        if (frame > input_ack_frame_) {
            InputFrame& stored = input_frames_[frame & (input_frames_.size() - 1U)];
            stored.frame = frame;
            stored.valid = true;
            stored.bytes = decoded;
            latest_input_ = decoded;
            input_ack_frame_ = frame;
            has_latest_input_ = true;
            stats_.latest_received_input_frame = frame;
        }
        previous = decoded;
    }
    return true;
}

ServerInputForFrame ServerInputBuffer::select_input_for_frame(SyncFrame due_frame, std::size_t quantized_size) {
    ServerInputForFrame due;
    SyncFrame best_frame = 0;
    const std::vector<std::uint8_t>* best_bytes = nullptr;
    if (!input_frames_.empty()) {
        const SyncFrame capacity_window_begin = due_frame > input_frames_.size()
            ? due_frame - static_cast<SyncFrame>(input_frames_.size()) + 1U
            : 1U;
        const SyncFrame begin = std::max(latest_applied_input_frame_ + 1U, capacity_window_begin);
        for (SyncFrame frame = begin; frame <= due_frame; ++frame) {
            const InputFrame& input = input_frames_[frame & (input_frames_.size() - 1U)];
            if (input.valid && input.frame == frame && input.bytes.size() == quantized_size) {
                best_frame = frame;
                best_bytes = &input.bytes;
            }
        }
    }

    if (best_bytes != nullptr) {
        latest_applied_input_ = *best_bytes;
        latest_applied_input_frame_ = best_frame;
        has_latest_applied_input_ = true;
        stats_.latest_applied_input_frame = best_frame;
        ++stats_.input_frames_applied;
        due.bytes = &latest_applied_input_;
        due.input_frame = best_frame;
        if (best_frame < due_frame) {
            ++stats_.input_starvation_frames;
        }
    } else if (has_latest_applied_input_ && latest_applied_input_.size() == quantized_size) {
        due.bytes = &latest_applied_input_;
        due.input_frame = latest_applied_input_frame_;
        ++stats_.input_starvation_frames;
        ++stats_.input_reused_frames;
    } else {
        ++stats_.input_starvation_frames;
    }
    due.cached = true;
    return due;
}

}  // namespace ashiato::sync::server_detail
