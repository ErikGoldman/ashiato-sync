#pragma once

#include "game/components.hpp"

namespace fps {

void simulate_character(
    FpsTransform& transform,
    FpsVelocity& velocity,
    FpsCombatState& combat,
    const FpsInput& input,
    bool stunned = false);

}  // namespace fps
