#pragma once

#include "kage/sync/client.hpp"
#include "kage/sync/server.hpp"

namespace kage::sync {

struct DisplayFrameEntity {
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
    ecs::Entity local_entity;
    SyncArchetypeId archetype;
    SyncFrame frame = 0;
    float alpha = 0.0f;
    std::uint64_t tag_mask = 0;
    std::vector<ReplicatedComponentUpdate> components;

    template <typename T>
    bool try_get_display_value(const ecs::Registry& registry, T& out) const {
        const ecs::Entity component = registry.component<T>();
        return try_get_display_value(registry, component, &out);
    }

    bool try_get_display_value(const ecs::Registry& registry, ecs::Entity component, void* out) const;
    bool has_tag(const ecs::Registry& registry, ecs::Entity tag) const;
};

class DisplayFrameInterpolation {
public:
    explicit DisplayFrameInterpolation(ReplicationClient& client);
    explicit DisplayFrameInterpolation(ReplicationServer& server);

    ClientId local_client() const noexcept;
    double target_frame() const noexcept;
    void capture_server_frame(ecs::Registry& registry);
    const std::vector<DisplayFrameEntity>& entities(const ecs::Registry& registry);

private:
    enum class Source {
        Client,
        Server
    };

    struct Snapshot {
        SyncFrame frame = 0;
        std::vector<DisplayFrameEntity> entities;
        bool valid = false;
    };

    void rebuild_from_client(const ecs::Registry& registry);
    void rebuild_from_server(const ecs::Registry& registry);

    Source source_;
    ReplicationClient* client_ = nullptr;
    ReplicationServer* server_ = nullptr;
    Snapshot previous_;
    Snapshot current_;
    std::vector<DisplayFrameEntity> display_;
};

}  // namespace kage::sync
