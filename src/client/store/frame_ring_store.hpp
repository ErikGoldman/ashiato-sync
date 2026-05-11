#pragma once

#include "client/store/frame_payload_ring.hpp"
#include "client/state.hpp"
#include "detail/frame_data.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ashiato::sync::client_detail {

struct EntityFrameView {
    SyncFrame frame = 0;
    bool valid = false;
    bool entity_present = false;
    detail::FrameDataView baseline;
};

struct MutableEntityFrameView {
    SyncFrame frame = 0;
    bool* valid = nullptr;
    bool* entity_present = nullptr;
    detail::MutableFrameDataView baseline;
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
    void write(std::uint32_t entity_index, const EntityBufferedFrame& sample);
    MutableEntityFrameView begin_write(std::uint32_t entity_index, SyncFrame frame, std::size_t payload_size);
    bool copy_frames(std::uint32_t entity_index, std::vector<EntityBufferedFrame>& out) const;

    struct FrameMetadata {
        SyncFrame frame = 0;
        bool valid = false;
        bool entity_present = false;
        std::uint64_t tag_mask = 0;
        std::uint64_t present_mask = 0;
    };

    using EntityRing = FramePayloadRing<FrameMetadata>;

private:
    std::size_t capacity_ = 64;
    std::vector<EntityRing> rings_;
};

}  // namespace ashiato::sync::client_detail
