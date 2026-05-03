#pragma once

#include "game/components.hpp"

#include "kage/sync/sync.hpp"

#include <raylib.h>

namespace fps {

SyncSchema define_schema(ecs::Registry& registry);
ecs::Entity spawn_character(
    ecs::Registry& registry,
    const SyncSchema& schema,
    Vector3 position,
    Color color,
    kage::sync::ClientId owner = kage::sync::invalid_client_id);

}  // namespace fps
