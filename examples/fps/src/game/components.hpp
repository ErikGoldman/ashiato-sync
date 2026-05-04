#pragma once

#include "game/constants.hpp"

#include "kage/sync/sync.hpp"

#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <raylib.h>

namespace fps {

struct FpsTransform {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct FpsVelocity {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint8_t grounded = 1;
};

struct FpsCombatState {
    std::int16_t health = 100;
    std::uint8_t ammo = magazine_size;
    std::uint8_t dead = 0;
    float reload_remaining = 0.0f;
    float respawn_remaining = 0.0f;
    std::uint32_t last_jump_seq = 0;
    std::uint32_t last_fire_seq = 0;
    std::uint32_t last_reload_seq = 0;
};

struct FpsStunned {};

struct FpsKillCamTarget {};

struct FpsStunState {
    float remaining = 0.0f;
};

struct FpsVisual {
    float radius = capsule_radius;
    float height = capsule_height;
    std::uint8_t r = 90;
    std::uint8_t g = 180;
    std::uint8_t b = 255;
    std::uint8_t a = 255;
    std::uint8_t style = 0;
};

struct FpsInput {
    float move_x = 0.0f;
    float move_y = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::uint32_t jump_seq = 0;
    std::uint32_t fire_seq = 0;
    std::uint32_t reload_seq = 0;
    kage::sync::SyncFrame shot_interpolation_frame = 0;
};

struct FpsTransformHistory {
    struct Sample {
        kage::sync::SyncFrame frame = 0;
        FpsTransform transform;
        bool valid = false;
    };

    static constexpr std::size_t capacity = 64;
    std::array<Sample, capacity> samples{};
};

struct FpsServerFrame {
    kage::sync::SyncFrame frame = 0;
};

struct FpsDeathInfo {
    kage::sync::ClientId killer = kage::sync::invalid_client_id;
};

struct FpsShotEffect {
    float seconds = 0.0f;
    std::uint8_t sound_played = 0;
};

enum class FpsHitSound : std::uint8_t {
    None = 0,
    ConfirmedHit = 1,
    TookDamage = 2,
    Died = 3,
};

struct FpsHitEffect {
    float seconds = 0.0f;
    std::uint8_t sound_played = 0;
    FpsHitSound sound = FpsHitSound::None;
};

struct WallParticle {
    Vector3 position{};
    Vector3 velocity{};
    float seconds = 0.0f;
    Color color{255, 220, 120, 255};
};

struct FpsSurfaceHitEffect {
    std::vector<WallParticle> particles;
};

struct FpsHitConfirmSuppression {
    struct Entry {
        ecs::Entity victim;
        kage::sync::ClientEntityNetworkId victim_network_id = kage::sync::invalid_client_entity_network_id;
        kage::sync::SyncFrame frame = 0;
        float seconds = 0.0f;
    };

    std::vector<Entry> entries;
};

struct BotBrain {
    float phase = 0.0f;
    Vector3 destination{};
    float fire_timer = 0.0f;
    std::uint8_t stun_attacks = 0;
};

struct MouseLookState {
    float yaw = 3.14159f;
    float pitch = 0.0f;
    bool captured = false;
    int skip_delta_frames = 2;
    Vector2 last_delta{};
};

struct SyncSchema {
    kage::sync::SyncArchetypeId character;
};

}  // namespace fps

namespace ecs {

template <>
struct is_singleton_component<fps::FpsServerFrame> : std::true_type {};

}  // namespace ecs
