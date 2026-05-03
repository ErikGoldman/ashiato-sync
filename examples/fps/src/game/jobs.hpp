#pragma once

#include "kage/sync/sync.hpp"

namespace fps {

void register_game_jobs(ecs::Registry& registry);
void register_game_jobs(ecs::Registry& registry, kage::sync::ReplicationClient& client);

}  // namespace fps
