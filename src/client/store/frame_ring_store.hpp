#pragma once

#include "client/store/frame_payload_ring.hpp"
#include "client/state.hpp"
#include "detail/frame_data.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ashiato::sync::client_detail {

enum class FrameWriteSource : std::uint8_t {
    Unknown,
    BufferedFrame,
    PredictedFrame,
    AuthoritativeSeed,
    ResimulatedFrame,
    CopiedFrame,
    PresentationFrame
};

struct EntityFrameView {
    SyncFrame frame = 0;
    bool valid = false;
    bool entity_present = false;
    std::uint64_t write_generation = 0;
    FrameWriteSource write_source = FrameWriteSource::Unknown;
    bool presentation_cache_hit = false;
    bool presentation_origin_valid = false;
    std::uint64_t presentation_origin_generation = 0;
    FrameWriteSource presentation_origin_write_source = FrameWriteSource::Unknown;
    std::uint64_t presentation_origin_payload_hash = 0;
    detail::FrameDataView baseline;
};

struct MutableEntityFrameView {
    SyncFrame frame = 0;
    bool* valid = nullptr;
    bool* entity_present = nullptr;
    std::uint64_t* write_generation = nullptr;
    FrameWriteSource* write_source = nullptr;
    bool* presentation_origin_valid = nullptr;
    std::uint64_t* presentation_origin_generation = nullptr;
    FrameWriteSource* presentation_origin_write_source = nullptr;
    std::uint64_t* presentation_origin_payload_hash = nullptr;
    detail::MutableFrameDataView baseline;
};

struct FramePresentationOrigin {
    bool valid = false;
    std::uint64_t write_generation = 0;
    FrameWriteSource write_source = FrameWriteSource::Unknown;
    std::uint64_t payload_hash = 0;
};

class ClientFrameRingStore {
public:
    explicit ClientFrameRingStore(std::size_t capacity = 64);

    std::size_t capacity() const noexcept {
        return capacity_;
    }
    bool empty(std::uint32_t entity_index) const noexcept;
    void ensure(std::uint32_t entity_index);
    void clear(std::uint32_t entity_index) noexcept;
    void reset(std::uint32_t entity_index) noexcept;
    void clear_all() noexcept;
    bool contains(std::uint32_t entity_index, SyncFrame frame) const noexcept;
    bool view(std::uint32_t entity_index, SyncFrame frame, EntityFrameView& out) const noexcept;
    bool latest_present(std::uint32_t entity_index, EntityFrameView& out) const noexcept;
    bool read(std::uint32_t entity_index, SyncFrame frame, EntityBufferedFrame& out) const;
    void write(
        std::uint32_t entity_index,
        const EntityBufferedFrame& sample,
        FrameWriteSource source = FrameWriteSource::CopiedFrame,
        FramePresentationOrigin presentation_origin = {});
    MutableEntityFrameView begin_write(
        std::uint32_t entity_index,
        SyncFrame frame,
        std::size_t payload_size,
        FrameWriteSource source = FrameWriteSource::Unknown);
    bool copy_frames(std::uint32_t entity_index, std::vector<EntityBufferedFrame>& out) const;

    struct FrameMetadata {
        SyncFrame frame = 0;
        bool valid = false;
        bool entity_present = false;
        std::uint64_t tag_mask = 0;
        std::uint64_t present_mask = 0;
        std::uint64_t write_generation = 0;
        FrameWriteSource write_source = FrameWriteSource::Unknown;
        bool presentation_origin_valid = false;
        std::uint64_t presentation_origin_generation = 0;
        FrameWriteSource presentation_origin_write_source = FrameWriteSource::Unknown;
        std::uint64_t presentation_origin_payload_hash = 0;
    };

    using EntityRing = FramePayloadRing<FrameMetadata>;

private:
    std::size_t capacity_ = 64;
    std::uint64_t write_generation_counter_ = 0;
    std::vector<EntityRing> rings_;
};

}  // namespace ashiato::sync::client_detail
