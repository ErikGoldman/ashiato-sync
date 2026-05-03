#pragma once

#include <array>
#include <cstdint>

#include <raylib.h>

namespace fps {

constexpr std::uint16_t default_port = 37043;
constexpr float fixed_dt = 1.0f / 60.0f;
constexpr float arena_half_x = 8.0f;
constexpr float arena_half_z = 8.0f;
constexpr float arena_height = 4.0f;
constexpr float capsule_radius = 0.35f;
constexpr float capsule_height = 1.65f;
constexpr float eye_height = 1.45f;
constexpr float move_speed = 4.8f;
constexpr float jump_speed = 5.2f;
constexpr float gravity = 13.0f;
constexpr int magazine_size = 30;
constexpr float reload_seconds = 3.0f;
constexpr float shot_range = 32.0f;
constexpr int shot_damage = 20;
constexpr float respawn_seconds = 3.0f;

struct CoverBox {
    Vector3 center{};
    Vector3 size{};
};

extern const std::array<CoverBox, 7> g_cover_boxes;

}  // namespace fps
