#pragma once

#include "game/components.hpp"

#include "ashiato/sync/sync.hpp"

#include <raylib.h>

namespace fps {

SyncSchema define_schema(ashiato::Registry& registry);
ashiato::Entity spawn_character(
    ashiato::Registry& registry,
    const SyncSchema& schema,
    Vector3 position,
    Color color,
    ashiato::sync::ClientId owner = ashiato::sync::invalid_client_id);

}  // namespace fps
