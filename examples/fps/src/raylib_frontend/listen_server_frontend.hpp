#pragma once

#include "ashiato/sync/sync.hpp"

#include <memory>

namespace ashiato {
class Registry;
}

namespace fps {

struct AppConfig;
struct SyncSchema;

class ListenServerFrontend {
public:
    ListenServerFrontend(
        const AppConfig& config,
        ashiato::Registry& registry,
        const SyncSchema& schema,
        ashiato::sync::ReplicationServer& server);
    ~ListenServerFrontend();

    ListenServerFrontend(const ListenServerFrontend&) = delete;
    ListenServerFrontend& operator=(const ListenServerFrontend&) = delete;
    ListenServerFrontend(ListenServerFrontend&&) noexcept;
    ListenServerFrontend& operator=(ListenServerFrontend&&) noexcept;

    ashiato::sync::ClientId host_client() const noexcept;
    bool window_should_close() const;
    void update_input(ashiato::Registry& registry, ashiato::sync::ReplicationServer& server);
    void capture_display(ashiato::Registry& registry, ashiato::sync::ReplicationServer& server);
    void update_effects(ashiato::Registry& registry, float dt);
    void render(ashiato::Registry& registry, ashiato::sync::ReplicationServer& server);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fps
