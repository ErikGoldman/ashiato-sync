#include "client/input_buffer.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <limits>

namespace kage::sync::client_detail {

bool ClientInputBuffer::set_latest(
    ecs::Registry& registry,
    const SyncSettings& settings,
    ecs::Entity component,
    const void* input) {
    if (input == nullptr || !settings.input_component || settings.input_component != component) {
        return false;
    }
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end() ||
        found_ops->second.quantize == nullptr ||
        found_ops->second.serialize == nullptr ||
        found_ops->second.apply == nullptr ||
        found_ops->second.quantized_size == 0U) {
        return false;
    }

    component_ = component;
    ops_ = found_ops->second;
    has_ops_ = true;
    latest_.assign(ops_.quantized_size, 0U);
    ops_.quantize(input, latest_.data());
    if (acked_baseline_.size() != ops_.quantized_size) {
        acked_baseline_.assign(ops_.quantized_size, 0U);
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
    if (!has_latest_ || !ready_for(settings) || frame <= acked_frame_) {
        return true;
    }
    ensure_capacity(capacity_frames);
    if (frames_.empty()) {
        return true;
    }
    InputFrame& stored = frames_[frame & (frames_.size() - 1U)];
    if (stored.valid && stored.frame != frame && stored.frame > acked_frame_) {
        history_discontinuous_ = true;
    }
    stored.frame = frame;
    stored.valid = true;
    stored.bytes = latest_;
    last_recorded_frame_ = std::max(last_recorded_frame_, frame);
    if (recorded != nullptr) {
        recorded->frame = frame;
        recorded->component = component_;
        recorded->bytes = stored.bytes.data();
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
        InputFrame& stored = frames_[current & (frames_.size() - 1U)];
        if (stored.valid && stored.frame == current && stored.bytes.size() == ops_.quantized_size) {
            last_recorded_frame_ = std::max(last_recorded_frame_, current);
            continue;
        }
        if (stored.valid && stored.frame != current && stored.frame > acked_frame_) {
            history_discontinuous_ = true;
        }
        stored.frame = current;
        stored.valid = true;
        stored.bytes = latest_;
        last_recorded_frame_ = std::max(last_recorded_frame_, current);
        recorded.push_back(ClientInputRecord{current, component_, stored.bytes.data()});
        if (current == frame) {
            break;
        }
    }
    return true;
}

bool ClientInputBuffer::apply_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame) const {
    if (!ready_for(settings) || frames_.empty()) {
        return true;
    }
    const InputFrame& stored = frames_[frame & (frames_.size() - 1U)];
    if (!stored.valid || stored.frame != frame || stored.bytes.size() != ops_.quantized_size) {
        return true;
    }
    if (settings.local_client == invalid_client_id || ops_.apply == nullptr) {
        return true;
    }
    registry.view<const NetworkOwner>().each([&](ecs::Entity entity, const NetworkOwner& owner) {
        if (owner.client == settings.local_client) {
            (void)ops_.apply(registry, entity, stored.bytes.data());
        }
    });
    return true;
}

void ClientInputBuffer::acknowledge_frame(SyncFrame frame) {
    if (frame <= acked_frame_) {
        return;
    }
    bool found_baseline = false;
    if (!frames_.empty()) {
        for (SyncFrame current = acked_frame_ + 1U; current <= frame; ++current) {
            InputFrame& stored = frames_[current & (frames_.size() - 1U)];
            if (stored.valid && stored.frame == current && current == frame) {
                acked_baseline_ = stored.bytes;
                found_baseline = true;
            }
        }
    }
    has_acked_baseline_ = frame == 0U || found_baseline;
    acked_frame_ = frame;
}

void ClientInputBuffer::apply_latest_to_owned_entities(ecs::Registry& registry, const SyncSettings& settings) const {
    if (settings.local_client == invalid_client_id || latest_.empty() || ops_.apply == nullptr) {
        return;
    }
    registry.view<const NetworkOwner>().each([&](ecs::Entity entity, const NetworkOwner& owner) {
        if (owner.client == settings.local_client) {
            (void)ops_.apply(registry, entity, latest_.data());
        }
    });
}

bool ClientInputBuffer::drain_packet(
    std::size_t mtu_bytes,
    std::size_t packet_id_bits,
    std::vector<std::uint32_t>& pending_acks,
    std::vector<BitBuffer>& packets,
    ClientInputPacketTrace* trace) {
    if (trace != nullptr) {
        *trace = {};
    }
    const bool has_input_frames = has_latest_ && last_recorded_frame_ > acked_frame_;
    if (!has_ops_ || !has_input_frames) {
        return false;
    }

    const std::size_t mtu_bits = mtu_bytes * 8U;
    const std::size_t fixed_header_bits = 8U + 16U + 32U + 16U + 1U;
    if (mtu_bits < fixed_header_bits) {
        return false;
    }

    SyncFrame first_input_frame = 0;
    const InputFrame* first_input = nullptr;
    if (!frames_.empty()) {
        for (SyncFrame frame = acked_frame_ + 1U; frame <= last_recorded_frame_; ++frame) {
            const InputFrame& input = frames_[frame & (frames_.size() - 1U)];
            if (input.valid && input.frame == frame && input.bytes.size() == ops_.quantized_size) {
                first_input_frame = frame;
                first_input = &input;
                break;
            }
        }
    }
    const bool baseline_valid = acked_frame_ == 0U || has_acked_baseline_;
    const bool first_input_full = first_input != nullptr &&
        (history_discontinuous_ || first_input_frame != acked_frame_ + 1U || !baseline_valid);

    BitBuffer packet;
    packet.reserve_bytes(mtu_bytes);
    packet.push_bits(protocol::client_input_message, 8U);
    const std::size_t ack_count_offset = packet.bit_size();
    packet.push_bits(0, 16U);

    std::size_t reserved_first_input_bits = 0;
    if (first_input != nullptr) {
        BitBuffer first_input_payload;
        const std::uint8_t* previous = first_input_full || acked_baseline_.empty()
            ? nullptr
            : acked_baseline_.data();
        ops_.serialize(previous, first_input->bytes.data(), first_input_payload, nullptr);
        reserved_first_input_bits = first_input_payload.bit_size();
    }

    std::uint16_t ack_count = 0;
    const std::size_t max_acks = std::min<std::size_t>(
        std::numeric_limits<std::uint16_t>::max(),
        (mtu_bits - fixed_header_bits) / packet_id_bits);
    while (ack_count < max_acks && ack_count < pending_acks.size()) {
        BitBuffer candidate = packet;
        candidate.push_bits(pending_acks[ack_count], packet_id_bits);
        const std::size_t explicit_first_frame_bits = first_input_full ? 32U : 0U;
        if (protocol::bytes_for_bits(
                candidate.bit_size() + 32U + 16U + 1U + explicit_first_frame_bits + reserved_first_input_bits) >
            mtu_bytes) {
            break;
        }
        packet = std::move(candidate);
        if (trace != nullptr) {
            trace->acks.push_back(pending_acks[ack_count]);
        }
        ++ack_count;
    }
    packet.overwrite_unsigned_bits(ack_count_offset, ack_count, 16U);

    packet.push_bits(acked_frame_, 32U);
    const std::size_t input_count_offset = packet.bit_size();
    packet.push_bits(0, 16U);
    packet.push_bool(first_input_full);
    if (first_input_full) {
        packet.push_bits(first_input_frame, 32U);
    }

    std::uint16_t input_count = 0;
    SyncFrame last_input_frame = 0;
    const std::uint8_t* previous = first_input_full || acked_baseline_.empty()
        ? nullptr
        : acked_baseline_.data();
    if (first_input_frame != 0U) {
        for (SyncFrame frame = first_input_frame; frame <= last_recorded_frame_; ++frame) {
            if (frames_.empty()) {
                break;
            }
            const InputFrame& input = frames_[frame & (frames_.size() - 1U)];
            if (!input.valid || input.frame != frame || input.bytes.size() != ops_.quantized_size) {
                break;
            }

            BitBuffer candidate = packet;
            ops_.serialize(previous, input.bytes.data(), candidate, nullptr);
            if (protocol::bytes_for_bits(candidate.bit_size()) > mtu_bytes) {
                break;
            }
            packet = std::move(candidate);
            previous = input.bytes.data();
            last_input_frame = frame;
            ++input_count;
            if (input_count == std::numeric_limits<std::uint16_t>::max()) {
                break;
            }
        }
    }

    if (ack_count == 0U && input_count == 0U) {
        return false;
    }
    packet.overwrite_unsigned_bits(input_count_offset, input_count, 16U);
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
    if (frames_.empty()) {
        frames_.resize(capacity_frames);
    }
}

}  // namespace kage::sync::client_detail
