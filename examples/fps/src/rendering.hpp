#pragma once

#include "app.hpp"
#include "game/components.hpp"

#include "kage/sync/sync.hpp"

#include <vector>

#include <raylib.h>

namespace fps {

Sound make_tone(float frequency, float seconds, float volume);
void draw_capsule(Vector3 base, const FpsVisual& visual, Color color);
void draw_character_body(Vector3 base, const FpsVisual& visual, Color color);
void draw_arena();
void draw_viewmodel_gun(float shot_seconds);
void draw_third_person_gun(const FpsTransform& transform, const FpsVisual& visual);
void draw_stunned_effect(const FpsTransform& transform, const FpsVisual& visual);
Vector3 third_person_muzzle_position(const FpsTransform& transform, const FpsVisual& visual);
void update_effects(ecs::Registry& registry, float dt);
FpsInput read_player_input(
    FpsInput previous,
    MouseLookState& look,
    kage::sync::SyncFrame display_target_frame);
void spawn_wall_particles(std::vector<WallParticle>& particles, Vector3 point, Vector3 normal);
void spawn_death_particles(std::vector<WallParticle>& particles, Vector3 point, Color color);
void adjust_link_settings(ClientLinkSimulator& incoming_link, ClientLinkSimulator& outgoing_link);

}  // namespace fps
