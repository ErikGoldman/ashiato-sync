#pragma once

#include "ashiato/sync/client.hpp"
#include "ashiato/sync/server.hpp"

namespace ashiato::sync {

class FractionalTickSampler {
public:
    explicit FractionalTickSampler(ReplicationClient& client);
    explicit FractionalTickSampler(ReplicationServer& server);

    ClientId local_client() const noexcept;
    double target_frame() const noexcept;
    void capture_server_frame(ashiato::Registry& registry);
    const std::vector<FractionalTickSample>& entities(const ashiato::Registry& registry);

private:
    enum class Source {
        Client,
        Server
    };

    struct SnapshotEntity {
        FractionalTickSample sample;
        SyncArchetypeId archetype = invalid_sync_archetype_id;
    };

    struct Snapshot {
        SyncFrame frame = 0;
        std::vector<SnapshotEntity> entities;
        bool valid = false;
    };

    void rebuild_from_client(const ashiato::Registry& registry);
    void rebuild_from_server(const ashiato::Registry& registry);

    Source source_;
    ReplicationClient* client_ = nullptr;
    ReplicationServer* server_ = nullptr;
    Snapshot previous_;
    Snapshot current_;
    std::vector<FractionalTickSample> samples_;
};

}  // namespace ashiato::sync
