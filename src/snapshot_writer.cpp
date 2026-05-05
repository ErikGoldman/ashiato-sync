#include "kage/sync/snapshot_writer.hpp"

#include "kage/sync/server.hpp"

#include <sstream>
#include <utility>

namespace kage::sync {

SnapshotWriter::SnapshotWriter(SnapshotWriterOptions options)
    : options_(std::move(options)) {}

SnapshotWriter::~SnapshotWriter() = default;

SnapshotWriter::SnapshotWriter(SnapshotWriter&&) noexcept = default;
SnapshotWriter& SnapshotWriter::operator=(SnapshotWriter&&) noexcept = default;

void SnapshotWriter::attach(ReplicationServer& server) {
    detach();
    subscription_ = server.after_frame().subscribe([this](const ServerFrameContext& context) {
        record(context);
    });
}

void SnapshotWriter::detach() {
    subscription_.reset();
}

bool SnapshotWriter::attached() const noexcept {
    return subscription_.active();
}

void SnapshotWriter::record(const ServerFrameContext& context) {
    if (!options_.write) {
        return;
    }

    const SyncFrame full_interval = options_.full_snapshot_interval_frames;
    const bool write_full =
        last_full_ == nullptr ||
        full_interval == 0U ||
        context.frame % full_interval == 0U;

    std::stringstream payload(std::ios::in | std::ios::out | std::ios::binary);
    if (write_full) {
        auto snapshot = std::make_unique<ecs::Registry::Snapshot>(context.registry.create_snapshot());
        snapshot->write(payload, options_.component_options);
        options_.write(SnapshotWriterFrame{
            &context.registry,
            context.frame,
            SnapshotWriterFrameKind::Full,
            payload.str(),
            context.cues});
        last_full_ = std::move(snapshot);
        last_delta_.reset();
        return;
    }

    auto delta = last_delta_ != nullptr
        ? std::make_unique<ecs::Registry::DeltaSnapshot>(context.registry.create_delta_snapshot(*last_delta_))
        : std::make_unique<ecs::Registry::DeltaSnapshot>(context.registry.create_delta_snapshot(*last_full_));
    delta->write(payload, options_.component_options);
    options_.write(SnapshotWriterFrame{
        &context.registry,
        context.frame,
        SnapshotWriterFrameKind::Delta,
        payload.str(),
        context.cues});
    last_delta_ = std::move(delta);
}

}  // namespace kage::sync
