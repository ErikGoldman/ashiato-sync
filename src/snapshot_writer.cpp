#include "kage/sync/snapshot_writer.hpp"

#include "kage/sync/server.hpp"

#include <sstream>
#include <utility>

namespace kage::sync {

SnapshotWriter::SnapshotWriter(SnapshotWriterOptions options)
    : options_(std::move(options)) {}

SnapshotWriter::~SnapshotWriter() = default;

SnapshotWriter::SnapshotWriter(SnapshotWriter&& other) noexcept
    : options_(std::move(other.options_)),
      last_full_(std::move(other.last_full_)),
      last_delta_(std::move(other.last_delta_)) {
    other.detach();
}

SnapshotWriter& SnapshotWriter::operator=(SnapshotWriter&& other) noexcept {
    if (this != &other) {
        detach();
        other.detach();
        options_ = std::move(other.options_);
        last_full_ = std::move(other.last_full_);
        last_delta_ = std::move(other.last_delta_);
    }
    return *this;
}

void SnapshotWriter::attach(ReplicationServer& server) {
    detach();
    subscription_ = server.subscribe_frame_consumer(*this);
}

void SnapshotWriter::detach() {
    subscription_.reset();
}

bool SnapshotWriter::attached() const noexcept {
    return subscription_.active();
}

void SnapshotWriter::accumulate_frame_delta(const ServerFrameDelta& frame) {
    record(frame);
}

void SnapshotWriter::record(const ServerFrameDelta& frame) {
    if (!options_.write) {
        return;
    }

    const SyncFrame full_interval = options_.full_snapshot_interval_frames;
    const bool write_full =
        last_full_ == nullptr ||
        full_interval == 0U ||
        frame.frame % full_interval == 0U;

    std::stringstream payload(std::ios::in | std::ios::out | std::ios::binary);
    if (write_full) {
        auto snapshot = std::make_unique<ecs::Registry::Snapshot>(frame.registry.create_snapshot());
        snapshot->write(payload, options_.component_options);
        options_.write(SnapshotWriterFrame{
            &frame.registry,
            frame.frame,
            SnapshotWriterFrameKind::Full,
            payload.str(),
            frame.cues});
        last_full_ = std::move(snapshot);
        last_delta_.reset();
        return;
    }

    auto delta = last_delta_ != nullptr
        ? std::make_unique<ecs::Registry::DeltaSnapshot>(frame.registry.create_delta_snapshot(*last_delta_))
        : std::make_unique<ecs::Registry::DeltaSnapshot>(frame.registry.create_delta_snapshot(*last_full_));
    delta->write(payload, options_.component_options);
    options_.write(SnapshotWriterFrame{
        &frame.registry,
        frame.frame,
        SnapshotWriterFrameKind::Delta,
        payload.str(),
        frame.cues});
    last_delta_ = std::move(delta);
}

}  // namespace kage::sync
