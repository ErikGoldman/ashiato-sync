#include "client/store/frame_ring_store.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ashiato::sync::client_detail {
namespace {

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

EntityFrameView make_view(
    const ClientFrameRingStore::EntityRing& ring,
    std::size_t slot) noexcept {
    const ClientFrameRingStore::FrameMetadata& sample = ring.metadata(slot);
    EntityFrameView view{
        sample.frame,
        sample.valid,
        sample.entity_present,
        sample.write_generation,
        sample.write_source,
        false,
        sample.presentation_origin_valid,
        sample.presentation_origin_generation,
        sample.presentation_origin_write_source,
        sample.presentation_origin_payload_hash,
        sample.presentation_boundary_corrected,
        detail::FrameDataView{sample.tag_mask, sample.present_mask, ring.payload(slot), ring.payload_stride()}};
    return view;
}

}  // namespace

ClientFrameRingStore::ClientFrameRingStore(std::size_t capacity)
    : capacity_(capacity) {
    if (!is_power_of_two(capacity_)) {
        throw std::invalid_argument("client frame ring capacity must be a nonzero power of two");
    }
}

bool ClientFrameRingStore::empty(std::uint32_t entity_index) const noexcept {
    return entity_index >= rings_.size() || rings_[entity_index].empty();
}

void ClientFrameRingStore::ensure(std::uint32_t entity_index) {
    if (entity_index >= rings_.size()) {
        rings_.resize(static_cast<std::size_t>(entity_index) + 1U);
    }
    EntityRing& ring = rings_[entity_index];
    ring.ensure(capacity_);
}

void ClientFrameRingStore::clear(std::uint32_t entity_index) noexcept {
    if (entity_index >= rings_.size()) {
        return;
    }
    EntityRing& ring = rings_[entity_index];
    for (FrameMetadata& frame : ring.metadata_entries()) {
        frame.frame = 0;
        frame.valid = false;
        frame.entity_present = false;
        frame.tag_mask = 0;
        frame.present_mask = 0;
        frame.write_generation = 0;
        frame.write_source = FrameWriteSource::Unknown;
        frame.presentation_origin_valid = false;
        frame.presentation_origin_generation = 0;
        frame.presentation_origin_write_source = FrameWriteSource::Unknown;
        frame.presentation_origin_payload_hash = 0;
        frame.presentation_boundary_corrected = false;
    }
    ring.clear_payloads();
}

void ClientFrameRingStore::reset(std::uint32_t entity_index) noexcept {
    if (entity_index >= rings_.size()) {
        return;
    }
    rings_[entity_index].reset();
}

void ClientFrameRingStore::clear_all() noexcept {
    for (std::uint32_t entity_index = 0; entity_index < rings_.size(); ++entity_index) {
        clear(entity_index);
    }
}

bool ClientFrameRingStore::contains(std::uint32_t entity_index, SyncFrame frame) const noexcept {
    if (empty(entity_index)) {
        return false;
    }
    const EntityRing& ring = rings_[entity_index];
    const FrameMetadata& sample = ring.metadata(ring.slot_for(frame));
    return sample.valid && sample.frame == frame;
}

bool ClientFrameRingStore::view(std::uint32_t entity_index, SyncFrame frame, EntityFrameView& out) const noexcept {
    if (!contains(entity_index, frame)) {
        return false;
    }
    const EntityRing& ring = rings_[entity_index];
    out = make_view(ring, ring.slot_for(frame));
    return true;
}

bool ClientFrameRingStore::latest_present(std::uint32_t entity_index, EntityFrameView& out) const noexcept {
    if (empty(entity_index)) {
        return false;
    }
    const EntityRing& ring = rings_[entity_index];
    bool has_latest = false;
    std::size_t latest_slot = 0;
    for (std::size_t slot = 0; slot < ring.size(); ++slot) {
        const FrameMetadata& sample = ring.metadata(slot);
        if (!sample.valid || !sample.entity_present) {
            continue;
        }
        if (!has_latest || sample.frame > ring.metadata(latest_slot).frame) {
            latest_slot = slot;
            has_latest = true;
        }
    }
    if (!has_latest) {
        return false;
    }
    out = make_view(ring, latest_slot);
    return true;
}

bool ClientFrameRingStore::read(std::uint32_t entity_index, SyncFrame frame, EntityBufferedFrame& out) const {
    EntityFrameView frame_view;
    if (!view(entity_index, frame, frame_view)) {
        return false;
    }
    out.frame = frame_view.frame;
    out.valid = frame_view.valid;
    out.entity_present = frame_view.entity_present;
    out.baseline.tag_mask = frame_view.baseline.tag_mask;
    out.baseline.present_mask = frame_view.baseline.present_mask;
    out.baseline.bytes.resize(frame_view.baseline.byte_count);
    if (frame_view.baseline.byte_count != 0U) {
        if (frame_view.baseline.bytes == nullptr) {
            return false;
        }
        std::memcpy(out.baseline.bytes.data(), frame_view.baseline.bytes, frame_view.baseline.byte_count);
    }
    return true;
}

void ClientFrameRingStore::write(
    std::uint32_t entity_index,
    const EntityBufferedFrame& sample,
    FrameWriteSource source,
    FramePresentationOrigin presentation_origin) {
    MutableEntityFrameView frame = begin_write(
        entity_index,
        sample.frame,
        sample.baseline.bytes.size(),
        source);
    *frame.valid = sample.valid;
    *frame.entity_present = sample.entity_present;
    *frame.presentation_origin_valid = presentation_origin.valid;
    *frame.presentation_origin_generation = presentation_origin.write_generation;
    *frame.presentation_origin_write_source = presentation_origin.write_source;
    *frame.presentation_origin_payload_hash = presentation_origin.payload_hash;
    *frame.presentation_boundary_corrected = presentation_origin.boundary_corrected;
    *frame.baseline.tag_mask = sample.baseline.tag_mask;
    *frame.baseline.present_mask = sample.baseline.present_mask;
    if (frame.baseline.byte_count != 0U) {
        if (frame.baseline.bytes == nullptr) {
            throw std::logic_error("frame payload storage is unavailable");
        }
        if (!sample.baseline.bytes.empty()) {
            std::memcpy(frame.baseline.bytes, sample.baseline.bytes.data(), sample.baseline.bytes.size());
        }
    }
}

MutableEntityFrameView ClientFrameRingStore::begin_write(
    std::uint32_t entity_index,
    SyncFrame frame,
    std::size_t payload_size,
    FrameWriteSource source) {
    ensure(entity_index);
    EntityRing& ring = rings_[entity_index];
    ring.ensure_payload_stride(payload_size);
    const std::size_t slot = ring.slot_for(frame);
    FrameMetadata& metadata = ring.metadata(slot);
    metadata.frame = frame;
    metadata.valid = true;
    metadata.entity_present = false;
    metadata.tag_mask = 0;
    metadata.present_mask = 0;
    metadata.write_generation = ++write_generation_counter_;
    metadata.write_source = source;
    metadata.presentation_origin_valid = false;
    metadata.presentation_origin_generation = 0;
    metadata.presentation_origin_write_source = FrameWriteSource::Unknown;
    metadata.presentation_origin_payload_hash = 0;
    metadata.presentation_boundary_corrected = false;
    std::uint8_t* payload = ring.payload(slot);
    if (payload != nullptr) {
        std::fill(payload, payload + ring.payload_stride(), std::uint8_t{0});
    }
    return MutableEntityFrameView{
        frame,
        &metadata.valid,
        &metadata.entity_present,
        &metadata.write_generation,
        &metadata.write_source,
        &metadata.presentation_origin_valid,
        &metadata.presentation_origin_generation,
        &metadata.presentation_origin_write_source,
        &metadata.presentation_origin_payload_hash,
        &metadata.presentation_boundary_corrected,
        detail::MutableFrameDataView{&metadata.tag_mask, &metadata.present_mask, payload, ring.payload_stride()}};
}

bool ClientFrameRingStore::copy_frames(std::uint32_t entity_index, std::vector<EntityBufferedFrame>& out) const {
    out.clear();
    if (empty(entity_index)) {
        return false;
    }
    const EntityRing& ring = rings_[entity_index];
    out.reserve(ring.size());
    for (const FrameMetadata& metadata : ring.metadata_entries()) {
        EntityBufferedFrame sample;
        if (metadata.valid) {
            (void)read(entity_index, metadata.frame, sample);
        } else {
            sample.frame = metadata.frame;
            sample.valid = false;
            sample.entity_present = false;
        }
        out.push_back(std::move(sample));
    }
    return true;
}

}  // namespace ashiato::sync::client_detail
