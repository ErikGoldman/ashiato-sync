#pragma once

#include "ashiato/sync/sync.hpp"

namespace fps {

void register_game_jobs(ashiato::Registry& registry);
void register_game_jobs(ashiato::Registry& registry, ashiato::sync::ReplicationClient& client);

}  // namespace fps
