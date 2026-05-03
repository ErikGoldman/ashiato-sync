#include "game/jobs.hpp"

#include "game/components.hpp"
#include "game/constants.hpp"
#include "game/cues.hpp"
#include "game/math.hpp"
#include "game/simulation.hpp"
#include "game/sync_traits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace fps {

template <typename View>
void simulate_fire(
    View& view,
    kage::sync::SyncSettings& sync,
    const kage::sync::SyncAuthority& authority,
    ecs::Entity shooter,
    const FpsTransform& transform) {
    const Vector3 origin = eye_position(transform);
    const Vector3 dir = normalize_or_zero(forward_from_angles(transform.yaw, transform.pitch));
    float wall_t = shot_range;
    Vector3 wall_normal{};
    (void)ray_arena_wall(origin, dir, wall_t, wall_normal);
    float cover_t = wall_t;
    Vector3 cover_normal{};
    if (ray_cover(origin, dir, cover_t, cover_normal)) {
        wall_t = cover_t;
        wall_normal = cover_normal;
    }

    ecs::Entity best_entity;
    float best_t = wall_t;
    view.each(
        [&view, &best_entity, &best_t, shooter, origin, dir](ecs::Entity entity, const FpsTransform& target_transform, FpsVelocity&, FpsCombatState& combat, const FpsInput&, kage::sync::SyncSettings&, const kage::sync::SyncAuthority&) {
            if (entity == shooter || combat.dead != 0U || combat.health <= 0) {
                return;
            }
            const FpsVisual* visual = view.template try_get<const FpsVisual>(entity);
            if (visual == nullptr) {
                return;
            }
            float t = shot_range;
            if (ray_character(origin, dir, target_transform, *visual, t) && t < best_t) {
                best_t = t;
                best_entity = entity;
            }
        });

    if (best_entity) {
        if (authority.is_authoritative()) {
            (void)kage::sync::emit_cue(sync, best_entity, PlayerHitCue{kage::sync::EntityReference{shooter}}, 0.5f);

            FpsCombatState& target = view.template write<FpsCombatState>(best_entity);
            target.health = static_cast<std::int16_t>(std::max<int>(0, target.health - shot_damage));
            (void)kage::sync::emit_cue(
                sync,
                shooter,
                HitConfirmCue{kage::sync::EntityReference{best_entity}},
                0.5f,
                true);
            if (target.health <= 0 && target.dead == 0U) {
                target.dead = 1;
                target.respawn_remaining = respawn_seconds;
            }
        }
    } else if (wall_t < shot_range) {
        (void)kage::sync::emit_cue(
            sync,
            shooter,
            SurfaceHitCue{add(origin, scale(dir, wall_t)), wall_normal},
            0.5f);
    }
}

void register_game_jobs(ecs::Registry& registry) {
    auto bot_job = registry.job<FpsTransform, FpsCombatState, FpsInput>(-1);
        bot_job.access_other_entities<BotBrain, const kage::sync::NetworkOwner>().single_thread().each([](
        auto& view,
        ecs::Entity entity,
        FpsTransform& transform,
        FpsCombatState& combat,
        FpsInput& input) {
        if (!view.template contains<BotBrain>(entity)) {
            return;
        }
        BotBrain& brain = view.template write<BotBrain>(entity);
        brain.phase += fixed_dt;
        input.move_x = 0.0f;
        input.move_y = 0.0f;
        if (combat.dead != 0U) {
            return;
        }

        const Vector3 position{transform.x, transform.y, transform.z};
        Vector3 to_destination{brain.destination.x - position.x, 0.0f, brain.destination.z - position.z};
        if (length_sq(to_destination) < 0.35f * 0.35f) {
            brain.destination = random_spawn_position();
            to_destination = Vector3{brain.destination.x - position.x, 0.0f, brain.destination.z - position.z};
        }

        ecs::Entity nearest_client;
        Vector3 nearest_eye{};
        float nearest_distance_sq = std::numeric_limits<float>::max();
        view.each(
            [&view, &nearest_distance_sq, &nearest_client, &nearest_eye, entity, &transform](ecs::Entity target, const FpsTransform& target_transform, const FpsCombatState& target_combat, FpsInput&) {
                const kage::sync::NetworkOwner* owner = view.template try_get<const kage::sync::NetworkOwner>(target);
                if (target == entity || owner == nullptr ||
                    owner->client == kage::sync::invalid_client_id || target_combat.dead != 0U) {
                    return;
                }
                const Vector3 target_eye = eye_position(target_transform);
                const Vector3 delta{target_eye.x - transform.x, target_eye.y - eye_position(transform).y, target_eye.z - transform.z};
                const float distance_sq = length_sq(delta);
                if (distance_sq < nearest_distance_sq) {
                    nearest_distance_sq = distance_sq;
                    nearest_client = target;
                    nearest_eye = target_eye;
                }
            });

        if (nearest_client) {
            const Vector3 eye = eye_position(transform);
            const Vector3 aim{nearest_eye.x - eye.x, nearest_eye.y - eye.y, nearest_eye.z - eye.z};
            const float horizontal = std::sqrt(aim.x * aim.x + aim.z * aim.z);
            input.yaw = std::atan2(aim.x, aim.z);
            input.pitch = clamp_pitch(std::atan2(aim.y, horizontal));
            if (combat.ammo == 0U && combat.reload_remaining <= 0.0f) {
                ++input.reload_seq;
            }
            brain.fire_timer -= fixed_dt;
            if (brain.fire_timer <= 0.0f && combat.reload_remaining <= 0.0f && combat.ammo > 0U) {
                brain.fire_timer = random_spawn_float(3.0f, 8.0f);
                ++input.fire_seq;
            }
        } else if (length_sq(to_destination) > 0.0001f) {
            input.yaw = std::atan2(to_destination.x, to_destination.z);
            input.pitch = 0.0f;
        }

        const Vector3 right = flat_right(input.yaw);
        const Vector3 forward = flat_forward(input.yaw);
        const Vector3 move_dir = normalize_or_zero(to_destination);
        input.move_x = dot(move_dir, right);
        input.move_y = dot(move_dir, forward);
    });;

    auto character_job = registry.job<FpsTransform, FpsVelocity, FpsCombatState, const FpsInput, kage::sync::SyncSettings, const kage::sync::SyncAuthority>(0);
        character_job.access_other_entities<const FpsVisual>().single_thread().each([](
        auto& view,
        ecs::Entity entity,
        FpsTransform& transform,
        FpsVelocity& velocity,
        FpsCombatState& combat,
        const FpsInput& input,
        kage::sync::SyncSettings& sync,
        const kage::sync::SyncAuthority& authority) {
        if (combat.dead != 0U) {
            if (authority.is_authoritative()) {
                combat.respawn_remaining = std::max(0.0f, combat.respawn_remaining - fixed_dt);
                if (combat.respawn_remaining <= 0.0f) {
                    respawn_character(transform, velocity, combat);
                }
            }
            return;
        }
        const std::uint32_t previous_fire = combat.last_fire_seq;
        simulate_character(transform, velocity, combat, input);
        if (input.fire_seq != previous_fire) {
            combat.last_fire_seq = input.fire_seq;
            if (combat.reload_remaining <= 0.0f && combat.ammo > 0U) {
                --combat.ammo;
                (void)kage::sync::emit_cue(sync, entity, ShotCue{}, 0.35f);
                simulate_fire(view, sync, authority, entity, transform);
            }
        }
    });;
}

void register_game_jobs(ecs::Registry& registry, kage::sync::ReplicationClient& client) {
    auto bot_job = client.simulation_job<FpsTransform, FpsCombatState, FpsInput>(registry, -1);
        bot_job.access_other_entities<BotBrain, const kage::sync::NetworkOwner>().single_thread().each([](
        auto& view,
        ecs::Entity entity,
        FpsTransform& transform,
        FpsCombatState& combat,
        FpsInput& input) {
        if (!view.template contains<BotBrain>(entity)) {
            return;
        }
        BotBrain& brain = view.template write<BotBrain>(entity);
        brain.phase += fixed_dt;
        input.move_x = 0.0f;
        input.move_y = 0.0f;
        if (combat.dead != 0U) {
            return;
        }

        const Vector3 position{transform.x, transform.y, transform.z};
        Vector3 to_destination{brain.destination.x - position.x, 0.0f, brain.destination.z - position.z};
        if (length_sq(to_destination) < 0.35f * 0.35f) {
            brain.destination = random_spawn_position();
            to_destination = Vector3{brain.destination.x - position.x, 0.0f, brain.destination.z - position.z};
        }

        ecs::Entity nearest_client;
        Vector3 nearest_eye{};
        float nearest_distance_sq = std::numeric_limits<float>::max();
        view.each(
            [&view, &nearest_distance_sq, &nearest_client, &nearest_eye, entity, &transform](ecs::Entity target, const FpsTransform& target_transform, const FpsCombatState& target_combat, FpsInput&) {
                const kage::sync::NetworkOwner* owner = view.template try_get<const kage::sync::NetworkOwner>(target);
                if (target == entity || owner == nullptr ||
                    owner->client == kage::sync::invalid_client_id || target_combat.dead != 0U) {
                    return;
                }
                const Vector3 target_eye = eye_position(target_transform);
                const Vector3 delta{target_eye.x - transform.x, target_eye.y - eye_position(transform).y, target_eye.z - transform.z};
                const float distance_sq = length_sq(delta);
                if (distance_sq < nearest_distance_sq) {
                    nearest_distance_sq = distance_sq;
                    nearest_client = target;
                    nearest_eye = target_eye;
                }
            });

        if (nearest_client) {
            const Vector3 eye = eye_position(transform);
            const Vector3 aim{nearest_eye.x - eye.x, nearest_eye.y - eye.y, nearest_eye.z - eye.z};
            const float horizontal = std::sqrt(aim.x * aim.x + aim.z * aim.z);
            input.yaw = std::atan2(aim.x, aim.z);
            input.pitch = clamp_pitch(std::atan2(aim.y, horizontal));
            if (combat.ammo == 0U && combat.reload_remaining <= 0.0f) {
                ++input.reload_seq;
            }
            brain.fire_timer -= fixed_dt;
            if (brain.fire_timer <= 0.0f && combat.reload_remaining <= 0.0f && combat.ammo > 0U) {
                brain.fire_timer = random_spawn_float(3.0f, 8.0f);
                ++input.fire_seq;
            }
        } else if (length_sq(to_destination) > 0.0001f) {
            input.yaw = std::atan2(to_destination.x, to_destination.z);
            input.pitch = 0.0f;
        }

        const Vector3 right = flat_right(input.yaw);
        const Vector3 forward = flat_forward(input.yaw);
        const Vector3 move_dir = normalize_or_zero(to_destination);
        input.move_x = dot(move_dir, right);
        input.move_y = dot(move_dir, forward);
    });;

    auto character_job = client.simulation_job<FpsTransform, FpsVelocity, FpsCombatState, const FpsInput, kage::sync::SyncSettings, const kage::sync::SyncAuthority>(registry, 0);
        character_job.access_other_entities<const FpsVisual>().single_thread().each([](
        auto& view,
        ecs::Entity entity,
        FpsTransform& transform,
        FpsVelocity& velocity,
        FpsCombatState& combat,
        const FpsInput& input,
        kage::sync::SyncSettings& sync,
        const kage::sync::SyncAuthority& authority) {
        if (combat.dead != 0U) {
            if (authority.is_authoritative()) {
                combat.respawn_remaining = std::max(0.0f, combat.respawn_remaining - fixed_dt);
                if (combat.respawn_remaining <= 0.0f) {
                    respawn_character(transform, velocity, combat);
                }
            }
            return;
        }
        const std::uint32_t previous_fire = combat.last_fire_seq;
        simulate_character(transform, velocity, combat, input);
        if (input.fire_seq != previous_fire) {
            combat.last_fire_seq = input.fire_seq;
            if (combat.reload_remaining <= 0.0f && combat.ammo > 0U) {
                --combat.ammo;
                (void)kage::sync::emit_cue(sync, entity, ShotCue{}, 0.35f);
                simulate_fire(view, sync, authority, entity, transform);
            }
        }
    });;
}

}  // namespace fps
