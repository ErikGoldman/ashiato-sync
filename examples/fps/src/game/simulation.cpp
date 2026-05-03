#include "game/simulation.hpp"

#include "game/constants.hpp"
#include "game/math.hpp"

#include <algorithm>

namespace fps {

void simulate_character(
    FpsTransform& transform,
    FpsVelocity& velocity,
    FpsCombatState& combat,
    const FpsInput& input) {
    if (combat.dead != 0U) {
        velocity = FpsVelocity{};
        transform.y = 0.0f;
        return;
    }

    transform.yaw = input.yaw;
    transform.pitch = clamp_pitch(input.pitch);

    if (input.reload_seq != combat.last_reload_seq) {
        combat.last_reload_seq = input.reload_seq;
        if (combat.ammo < magazine_size && combat.reload_remaining <= 0.0f) {
            combat.reload_remaining = reload_seconds;
        }
    }

    if (combat.reload_remaining > 0.0f) {
        combat.reload_remaining = std::max(0.0f, combat.reload_remaining - fixed_dt);
        if (combat.reload_remaining <= 0.0f) {
            combat.ammo = magazine_size;
        }
    }

    Vector3 wish = add(scale(flat_right(transform.yaw), input.move_x), scale(flat_forward(transform.yaw), input.move_y));
    wish = normalize_or_zero(wish);
    velocity.x = wish.x * move_speed;
    velocity.z = wish.z * move_speed;

    if (input.jump_seq != combat.last_jump_seq) {
        combat.last_jump_seq = input.jump_seq;
        if (velocity.grounded != 0U) {
            velocity.y = jump_speed;
            velocity.grounded = 0;
        }
    }

    velocity.y -= gravity * fixed_dt;
    transform.x += velocity.x * fixed_dt;
    transform.y += velocity.y * fixed_dt;
    transform.z += velocity.z * fixed_dt;

    if (transform.y <= 0.0f) {
        transform.y = 0.0f;
        velocity.y = 0.0f;
        velocity.grounded = 1;
    }
    transform.x = std::clamp(transform.x, -arena_half_x + capsule_radius, arena_half_x - capsule_radius);
    transform.z = std::clamp(transform.z, -arena_half_z + capsule_radius, arena_half_z - capsule_radius);
}

}  // namespace fps
