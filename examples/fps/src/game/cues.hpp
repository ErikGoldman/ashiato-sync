#pragma once

#include "kage/sync/sync.hpp"

#include <raylib.h>

namespace fps {

struct ShotCue {};

struct SurfaceHitCue {
    Vector3 position{};
    Vector3 normal{};
};

struct PlayerHitCue {
    kage::sync::EntityReference shooter;
};

struct HitConfirmCue {
    kage::sync::EntityReference victim;
};

struct PlayerDeathCue {};

}  // namespace fps
