#pragma once

#include "kage/sync/sync.hpp"

#include <memory>

namespace ecs {
class Registry;
}

namespace fps {

struct AppConfig;
struct SyncSchema;

class ListenServerFrontend {
public:
    ListenServerFrontend(
        const AppConfig& config,
        ecs::Registry& registry,
        const SyncSchema& schema,
        kage::sync::ReplicationServer& server);
    ~ListenServerFrontend();

    ListenServerFrontend(const ListenServerFrontend&) = delete;
    ListenServerFrontend& operator=(const ListenServerFrontend&) = delete;
    ListenServerFrontend(ListenServerFrontend&&) noexcept;
    ListenServerFrontend& operator=(ListenServerFrontend&&) noexcept;

    kage::sync::ClientId host_client() const noexcept;
    bool window_should_close() const;
    void update_input(ecs::Registry& registry, kage::sync::ReplicationServer& server);
    void capture_display(ecs::Registry& registry, kage::sync::ReplicationServer& server);
    void update_effects(ecs::Registry& registry, float dt);
    void render(ecs::Registry& registry, kage::sync::ReplicationServer& server);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fps
