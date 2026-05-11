#pragma once

#include "ashiato/sync/sync.hpp"

#include <raylib.h>

namespace fps {

struct ShotCue {};

struct SurfaceHitCue {
    Vector3 position{};
    Vector3 normal{};
};

struct PlayerHitCue {
    ashiato::sync::EntityReference shooter;
};

struct HitConfirmCue {
    ashiato::sync::EntityReference victim;
};

struct PlayerDeathCue {
    ashiato::sync::EntityReference shooter;
};

}  // namespace fps
