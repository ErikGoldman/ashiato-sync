#pragma once

#include "game/components.hpp"

#include <raylib.h>

namespace fps {

float clamp_pitch(float value);
Vector3 forward_from_angles(float yaw, float pitch);
Vector3 flat_forward(float yaw);
Vector3 flat_right(float yaw);
float dot(Vector3 a, Vector3 b);
Vector3 add(Vector3 a, Vector3 b);
Vector3 scale(Vector3 value, float amount);
float length_sq(Vector3 value);
Vector3 normalize_or_zero(Vector3 value);
Vector3 eye_position(const FpsTransform& transform);
float random_spawn_float(float min_value, float max_value);
Vector3 random_spawn_position();
void respawn_character(FpsTransform& transform, FpsVelocity& velocity, FpsCombatState& combat);
bool ray_aabb(Vector3 origin, Vector3 dir, Vector3 min, Vector3 max, float& out_t, Vector3& out_normal);
bool ray_arena_wall(Vector3 origin, Vector3 dir, float& out_t, Vector3& out_normal);
bool ray_cover(Vector3 origin, Vector3 dir, float& out_t, Vector3& out_normal);
bool ray_character(Vector3 origin, Vector3 dir, const FpsTransform& transform, const FpsVisual& visual, float& out_t);

}  // namespace fps
