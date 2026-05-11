#pragma once

#include "ashiato/sync/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ashiato::sync::client_detail {

template <typename Metadata>
class FramePayloadRing {
public:
    bool empty() const noexcept {
        return metadata_.empty();
    }

    std::size_t size() const noexcept {
        return metadata_.size();
    }

    std::size_t payload_stride() const noexcept {
        return payload_stride_;
    }

    void ensure(std::size_t capacity) {
        if (metadata_.empty()) {
            metadata_.resize(capacity);
        }
    }

    void reset() {
        metadata_.clear();
        payloads_.clear();
        payload_stride_ = 0;
    }

    std::size_t slot_for(SyncFrame frame) const noexcept {
        return frame & (metadata_.size() - 1U);
    }

    Metadata& metadata(std::size_t slot) noexcept {
        return metadata_[slot];
    }

    const Metadata& metadata(std::size_t slot) const noexcept {
        return metadata_[slot];
    }

    std::vector<Metadata>& metadata_entries() noexcept {
        return metadata_;
    }

    const std::vector<Metadata>& metadata_entries() const noexcept {
        return metadata_;
    }

    void ensure_payload_stride(std::size_t payload_stride) {
        if (payload_stride <= payload_stride_) {
            if (payloads_.size() != metadata_.size() * payload_stride_) {
                payloads_.assign(metadata_.size() * payload_stride_, std::uint8_t{0});
            }
            return;
        }

        const std::size_t previous_stride = payload_stride_;
        const std::vector<std::uint8_t> previous_payloads = std::move(payloads_);
        payload_stride_ = payload_stride;
        payloads_.assign(metadata_.size() * payload_stride_, std::uint8_t{0});
        if (previous_stride == 0U) {
            return;
        }
        for (std::size_t slot = 0; slot < metadata_.size(); ++slot) {
            std::memcpy(
                payloads_.data() + slot * payload_stride_,
                previous_payloads.data() + slot * previous_stride,
                previous_stride);
        }
    }

    void clear_payloads() noexcept {
        if (!payloads_.empty()) {
            std::fill(payloads_.begin(), payloads_.end(), std::uint8_t{0});
        }
    }

    std::uint8_t* payload(std::size_t slot) noexcept {
        return payload_stride_ == 0U || payloads_.empty()
            ? nullptr
            : payloads_.data() + slot * payload_stride_;
    }

    const std::uint8_t* payload(std::size_t slot) const noexcept {
        return payload_stride_ == 0U || payloads_.empty()
            ? nullptr
            : payloads_.data() + slot * payload_stride_;
    }

private:
    std::vector<Metadata> metadata_;
    std::vector<std::uint8_t> payloads_;
    std::size_t payload_stride_ = 0;
};

}  // namespace ashiato::sync::client_detail
