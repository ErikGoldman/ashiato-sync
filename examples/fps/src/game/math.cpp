#include "game/math.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

namespace fps {

float clamp_pitch(float value) {
    return std::clamp(value, -1.35f, 1.35f);
}

Vector3 forward_from_angles(float yaw, float pitch) {
    const float cp = std::cos(pitch);
    return Vector3{std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};
}

Vector3 flat_forward(float yaw) {
    return Vector3{std::sin(yaw), 0.0f, std::cos(yaw)};
}

Vector3 flat_right(float yaw) {
    return Vector3{std::cos(yaw), 0.0f, -std::sin(yaw)};
}

float dot(Vector3 a, Vector3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 add(Vector3 a, Vector3 b) {
    return Vector3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector3 scale(Vector3 value, float amount) {
    return Vector3{value.x * amount, value.y * amount, value.z * amount};
}

float length_sq(Vector3 value) {
    return dot(value, value);
}

Vector3 normalize_or_zero(Vector3 value) {
    const float len_sq = length_sq(value);
    if (len_sq <= 0.000001f) {
        return Vector3{};
    }
    return scale(value, 1.0f / std::sqrt(len_sq));
}

Vector3 eye_position(const FpsTransform& transform) {
    return Vector3{transform.x, transform.y + eye_height, transform.z};
}

float random_spawn_float(float min_value, float max_value) {
    static std::mt19937 rng{0x9e3779b9U};
    return std::uniform_real_distribution<float>{min_value, max_value}(rng);
}

Vector3 random_spawn_position() {
    return Vector3{
        random_spawn_float(-arena_half_x + capsule_radius, arena_half_x - capsule_radius),
        0.0f,
        random_spawn_float(-arena_half_z + capsule_radius, arena_half_z - capsule_radius)};
}

void respawn_character(FpsTransform& transform, FpsVelocity& velocity, FpsCombatState& combat) {
    const Vector3 position = random_spawn_position();
    transform.x = position.x;
    transform.y = position.y;
    transform.z = position.z;
    transform.yaw = random_spawn_float(-3.14159f, 3.14159f);
    transform.pitch = 0.0f;
    velocity = FpsVelocity{};
    combat.health = 100;
    combat.ammo = magazine_size;
    combat.dead = 0;
    combat.reload_remaining = 0.0f;
    combat.respawn_remaining = 0.0f;
}

bool ray_aabb(Vector3 origin, Vector3 dir, Vector3 min, Vector3 max, float& out_t, Vector3& out_normal) {
    float t_min = 0.0f;
    float t_max = shot_range;
    Vector3 normal{};
    auto axis = [&t_min, &t_max, &normal](float o, float d, float mn, float mx, Vector3 n0, Vector3 n1) {
        if (std::fabs(d) < 0.000001f) {
            return o >= mn && o <= mx;
        }
        float t0 = (mn - o) / d;
        float t1 = (mx - o) / d;
        Vector3 near_normal = n0;
        if (t0 > t1) {
            std::swap(t0, t1);
            near_normal = n1;
        }
        if (t0 > t_min) {
            t_min = t0;
            normal = near_normal;
        }
        t_max = std::min(t_max, t1);
        return t_min <= t_max;
    };

    if (!axis(origin.x, dir.x, min.x, max.x, Vector3{-1.0f, 0.0f, 0.0f}, Vector3{1.0f, 0.0f, 0.0f}) ||
        !axis(origin.y, dir.y, min.y, max.y, Vector3{0.0f, -1.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}) ||
        !axis(origin.z, dir.z, min.z, max.z, Vector3{0.0f, 0.0f, -1.0f}, Vector3{0.0f, 0.0f, 1.0f})) {
        return false;
    }
    out_t = t_min;
    out_normal = normal;
    return out_t >= 0.0f;
}

bool ray_arena_wall(Vector3 origin, Vector3 dir, float& out_t, Vector3& out_normal) {
    float best = shot_range;
    Vector3 best_normal{};
    auto plane = [&best, &best_normal, origin, dir](float denom, float numer, Vector3 normal) {
        if (std::fabs(denom) < 0.000001f) {
            return;
        }
        const float t = numer / denom;
        if (t < 0.0f || t >= best) {
            return;
        }
        const Vector3 hit = add(origin, scale(dir, t));
        if (hit.x >= -arena_half_x && hit.x <= arena_half_x &&
            hit.z >= -arena_half_z && hit.z <= arena_half_z &&
            hit.y >= 0.0f && hit.y <= arena_height) {
            best = t;
            best_normal = normal;
        }
    };
    plane(dir.x, arena_half_x - origin.x, Vector3{-1.0f, 0.0f, 0.0f});
    plane(dir.x, -arena_half_x - origin.x, Vector3{1.0f, 0.0f, 0.0f});
    plane(dir.z, arena_half_z - origin.z, Vector3{0.0f, 0.0f, -1.0f});
    plane(dir.z, -arena_half_z - origin.z, Vector3{0.0f, 0.0f, 1.0f});
    plane(dir.y, arena_height - origin.y, Vector3{0.0f, -1.0f, 0.0f});
    plane(dir.y, -origin.y, Vector3{0.0f, 1.0f, 0.0f});
    out_t = best;
    out_normal = best_normal;
    return best < shot_range;
}

bool ray_cover(Vector3 origin, Vector3 dir, float& out_t, Vector3& out_normal) {
    bool hit_any = false;
    float best_t = out_t;
    Vector3 best_normal{};
    for (const CoverBox& box : g_cover_boxes) {
        const Vector3 half = scale(box.size, 0.5f);
        const Vector3 min{box.center.x - half.x, box.center.y - half.y, box.center.z - half.z};
        const Vector3 max{box.center.x + half.x, box.center.y + half.y, box.center.z + half.z};
        float t = best_t;
        Vector3 normal{};
        if (ray_aabb(origin, dir, min, max, t, normal) && t < best_t) {
            hit_any = true;
            best_t = t;
            best_normal = normal;
        }
    }
    if (hit_any) {
        out_t = best_t;
        out_normal = best_normal;
    }
    return hit_any;
}

bool ray_character(
    Vector3 origin,
    Vector3 dir,
    const FpsTransform& transform,
    const FpsVisual& visual,
    float& out_t) {
    const Vector3 min{transform.x - visual.radius, transform.y, transform.z - visual.radius};
    const Vector3 max{transform.x + visual.radius, transform.y + visual.height, transform.z + visual.radius};
    Vector3 normal{};
    return ray_aabb(origin, dir, min, max, out_t, normal);
}

}  // namespace fps
