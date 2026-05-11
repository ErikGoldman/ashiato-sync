#include "raylib_frontend/listen_server_frontend.hpp"

#include "app.hpp"
#include "game/components.hpp"
#include "game/constants.hpp"
#include "game/math.hpp"
#include "game/schema.hpp"
#include "raylib_frontend/rendering.hpp"
#include "replay.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include <raylib.h>
#include <rlgl.h>

namespace fps {
namespace {

ecs::Entity find_local_player(ecs::Registry& registry, kage::sync::ClientId client) {
    ecs::Entity result;
    registry.view<const kage::sync::NetworkOwner>().each(
        [&result, client](ecs::Entity entity, const kage::sync::NetworkOwner& owner) {
            if (!result && owner.client == client) {
                result = entity;
            }
        });
    return result;
}

void draw_kill_cam_overlay() {
    const int width = GetScreenWidth();
    const int height = GetScreenHeight();
    const char* label = "KILL CAM";
    constexpr int font_size = 34;
    const int label_width = MeasureText(label, font_size);

    DrawRectangle(0, 0, width, 82, Color{8, 6, 8, 215});
    DrawRectangle(0, height - 38, width, 38, Color{8, 6, 8, 190});
    DrawRectangle(0, 82, width, 3, Color{230, 40, 35, 230});
    DrawRectangle(0, 0, width, height, Color{120, 22, 18, 28});
    DrawRectangleLinesEx(
        Rectangle{2.0f, 2.0f, static_cast<float>(width - 4), static_cast<float>(height - 4)},
        4.0f,
        Color{230, 40, 35, 180});
    DrawText(label, width / 2 - label_width / 2, 22, font_size, Color{255, 245, 235, 255});
    DrawText(
        "killer perspective",
        width / 2 - MeasureText("killer perspective", 14) / 2,
        58,
        14,
        Color{255, 185, 170, 255});
}

}  // namespace

struct ListenServerFrontend::Impl {
    kage::sync::ClientId host_client = kage::sync::invalid_client_id;
    std::string replay_host = "127.0.0.1";
    std::uint16_t replay_port = 0;
    std::unique_ptr<kage::sync::FractionalTickSampler> display;
    std::unique_ptr<kage::sync::FractionalTickSampler> death_cam_display;
    FpsDeathCamClient death_cam;
    FpsInput current_input{};
    MouseLookState look;
    bool show_stats = true;
    bool was_local_dead = false;
    kage::sync::SyncFrame captured_frame = 0;
    Sound shot_sound{};
    Sound hit_confirm_sound{};
    Sound took_damage_sound{};
    Sound death_sound{};
    std::unique_ptr<RemoteClientLauncher> remote_clients;
};

ListenServerFrontend::ListenServerFrontend(
    const AppConfig& config,
    ecs::Registry& registry,
    const SyncSchema& schema,
    kage::sync::ReplicationServer& server)
    : impl_(std::make_unique<Impl>()) {
    impl_->host_client = server.add_local_client(registry);
    if (impl_->host_client == kage::sync::invalid_client_id) {
        throw std::runtime_error("failed to allocate listen-server local client");
    }
    impl_->replay_port = config.replay_port;
    (void)spawn_character(registry, schema, Vector3{0.0f, 0.0f, -3.0f}, Color{90, 180, 255, 255}, impl_->host_client);

    impl_->display = std::make_unique<kage::sync::FractionalTickSampler>(server);

    InitWindow(1280, 720, "kage-sync FPS listen server");
    InitAudioDevice();
    EnableCursor();
    SetTargetFPS(120);
    impl_->shot_sound = make_tone(180.0f, 0.09f, 0.45f);
    impl_->hit_confirm_sound = make_tone(720.0f, 0.08f, 0.32f);
    impl_->took_damage_sound = make_tone(260.0f, 0.16f, 0.45f);
    impl_->death_sound = make_tone(95.0f, 0.45f, 0.55f);
    impl_->remote_clients = std::make_unique<RemoteClientLauncher>(config);
}

ListenServerFrontend::~ListenServerFrontend() {
    if (!impl_) {
        return;
    }
    UnloadSound(impl_->shot_sound);
    UnloadSound(impl_->hit_confirm_sound);
    UnloadSound(impl_->took_damage_sound);
    UnloadSound(impl_->death_sound);
    CloseAudioDevice();
    CloseWindow();
}

ListenServerFrontend::ListenServerFrontend(ListenServerFrontend&&) noexcept = default;
ListenServerFrontend& ListenServerFrontend::operator=(ListenServerFrontend&&) noexcept = default;

kage::sync::ClientId ListenServerFrontend::host_client() const noexcept {
    return impl_ != nullptr ? impl_->host_client : kage::sync::invalid_client_id;
}

bool ListenServerFrontend::window_should_close() const {
    return WindowShouldClose();
}

void ListenServerFrontend::update_input(ecs::Registry& registry, kage::sync::ReplicationServer& server) {
    const double target = impl_->display != nullptr ? impl_->display->target_frame() : 0.0;
    const kage::sync::SyncFrame target_frame =
        target > 0.0 && std::isfinite(target)
        ? static_cast<kage::sync::SyncFrame>(std::floor(target))
        : 0U;
    impl_->current_input = read_player_input(impl_->current_input, impl_->look, target_frame);
    if (IsKeyPressed(KEY_SLASH)) {
        impl_->show_stats = !impl_->show_stats;
    }
    (void)server.set_local_input(registry, impl_->current_input);
}

void ListenServerFrontend::capture_display(ecs::Registry& registry, kage::sync::ReplicationServer& server) {
    if (impl_->display != nullptr && impl_->captured_frame != server.frame()) {
        impl_->display->capture_server_frame(registry);
        impl_->captured_frame = server.frame();
    }
}

void ListenServerFrontend::update_effects(ecs::Registry& registry, float dt) {
    fps::update_effects(registry, dt);

    const ecs::Entity local_entity = find_local_player(registry, impl_->host_client);
    bool is_local_dead = false;
    if (local_entity && registry.alive(local_entity)) {
        if (const FpsCombatState* combat = registry.try_get<FpsCombatState>(local_entity)) {
            is_local_dead = combat->dead != 0U;
        }
    }
    if (is_local_dead && !impl_->was_local_dead && !impl_->death_cam.active()) {
        impl_->death_cam.start(impl_->replay_host, impl_->replay_port, impl_->host_client);
    }
    impl_->was_local_dead = is_local_dead;
    impl_->death_cam.tick(dt);
    if (impl_->death_cam.active()) {
        fps::update_effects(impl_->death_cam.registry(), dt);
    }
}

void ListenServerFrontend::render(ecs::Registry& registry, kage::sync::ReplicationServer& server) {
    const ecs::Entity replay_local_entity =
        impl_->death_cam.active() ? find_local_player(impl_->death_cam.registry(), impl_->host_client) : ecs::Entity{};
    const bool replay_ready = replay_local_entity != ecs::Entity{};
    if (replay_ready && impl_->death_cam_display == nullptr) {
        impl_->death_cam_display = std::make_unique<kage::sync::FractionalTickSampler>(impl_->death_cam.client());
    } else if (!replay_ready) {
        impl_->death_cam_display.reset();
    }
    ecs::Registry& render_registry = replay_ready ? impl_->death_cam.registry() : registry;
    kage::sync::FractionalTickSampler& render_display_source =
        replay_ready ? *impl_->death_cam_display : *impl_->display;
    const std::vector<kage::sync::FractionalTickSample>& render_display = render_display_source.entities(render_registry);
    ecs::Entity local_entity = find_local_player(render_registry, impl_->host_client);
    if (replay_ready) {
        ecs::Entity ecs_target;
        render_registry.view<const FpsTransform>().with_tags<const FpsKillCamTarget>().each(
            [&ecs_target](ecs::Entity entity, const FpsTransform&) {
                if (!ecs_target) {
                    ecs_target = entity;
                }
            });
        const std::uint64_t replay_target_player_id = impl_->death_cam.target_player_id();
        if (replay_target_player_id != 0U) {
            render_registry.view<const FpsUniquePlayerId, const FpsTransform>().each(
                [&ecs_target, replay_target_player_id](
                    ecs::Entity entity,
                    const FpsUniquePlayerId& unique,
                    const FpsTransform&) {
                    if (unique.value == replay_target_player_id) {
                        ecs_target = entity;
                    }
                });
        }
        if (ecs_target) {
            local_entity = ecs_target;
        }
    }
    Camera3D camera{};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 70.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    FpsCombatState local_combat{};
    float local_shot_seconds = 0.0f;
    bool local_stunned = false;
    bool has_local = false;
    if (local_entity && render_registry.alive(local_entity)) {
        FpsTransform sampled_transform{};
        const FpsTransform* transform = nullptr;
        for (const kage::sync::FractionalTickSample& sample : render_display) {
            if (sample.local_entity == local_entity && sample.try_get_sampled_value(render_registry, sampled_transform)) {
                transform = &sampled_transform;
                break;
            }
        }
        if (transform == nullptr) {
            transform = render_registry.try_get<FpsTransform>(local_entity);
        }
        if (transform != nullptr) {
            has_local = true;
            camera.position = eye_position(*transform);
            camera.target = add(camera.position, forward_from_angles(transform->yaw, transform->pitch));
            if (const FpsCombatState* combat = render_registry.try_get<FpsCombatState>(local_entity)) {
                local_combat = *combat;
                local_stunned = render_registry.has(local_entity, render_registry.component<FpsStunned>());
            }
            if (const FpsShotEffect* shot = render_registry.try_get<FpsShotEffect>(local_entity)) {
                local_shot_seconds = shot->seconds;
            }
        }
    }
    if (!has_local) {
        camera.position = Vector3{0.0f, 2.0f, -6.0f};
        camera.target = Vector3{0.0f, 1.2f, 0.0f};
    }

    BeginDrawing();
    ClearBackground(Color{20, 22, 26, 255});
    BeginMode3D(camera);
    draw_arena();
    for (const kage::sync::FractionalTickSample& sample : render_display) {
        FpsTransform transform{};
        if (!sample.try_get_sampled_value(render_registry, transform)) {
            continue;
        }
        const FpsVisual* visual = render_registry.try_get<FpsVisual>(sample.local_entity);
        const FpsCombatState* combat = render_registry.try_get<FpsCombatState>(sample.local_entity);
        if (visual == nullptr || combat == nullptr || combat->dead != 0U || sample.local_entity == local_entity) {
            continue;
        }
        Color color{visual->r, visual->g, visual->b, visual->a};
        if (render_registry.contains<FpsHitEffect>(sample.local_entity)) {
            color = Color{255, 80, 80, 255};
        }
        draw_character_body(Vector3{transform.x, transform.y, transform.z}, *visual, color);
        draw_third_person_gun(transform, *visual);
        if (render_registry.has<FpsStunned>(sample.local_entity)) {
            draw_stunned_effect(transform, *visual);
        }
        if (render_registry.contains<FpsShotEffect>(sample.local_entity)) {
            DrawSphere(third_person_muzzle_position(transform, *visual), 0.12f, Color{255, 230, 80, 220});
        }
    }
    EndMode3D();
    if (has_local && local_combat.dead == 0U) {
        draw_viewmodel_gun(local_shot_seconds);
    }
    rlDisableDepthTest();

    render_registry.view<FpsShotEffect>().each([this](ecs::Entity entity, FpsShotEffect& effect) {
        if (effect.sound_played == 0U) {
            PlaySound(impl_->shot_sound);
            effect.sound_played = 1;
        }
        (void)entity;
    });
    render_registry.view<FpsHitEffect>().each([this, local_entity](ecs::Entity entity, FpsHitEffect& effect) {
        if (effect.sound_played != 0U) {
            return;
        }
        if (effect.sound == FpsHitSound::ConfirmedHit) {
            PlaySound(impl_->hit_confirm_sound);
            effect.sound_played = 1;
            return;
        }
        if (entity == local_entity && effect.sound == FpsHitSound::TookDamage) {
            PlaySound(impl_->took_damage_sound);
            effect.sound_played = 1;
            return;
        }
        if (entity == local_entity && effect.sound == FpsHitSound::Died) {
            PlaySound(impl_->death_sound);
            effect.sound_played = 1;
        }
    });

    const int cx = GetScreenWidth() / 2;
    const int cy = GetScreenHeight() / 2;
    if (local_combat.reload_remaining > 0.0f) {
        const float progress = 1.0f - local_combat.reload_remaining / reload_seconds;
        DrawRing(
            Vector2{static_cast<float>(cx), static_cast<float>(cy)},
            16.0f,
            20.0f,
            -90.0f,
            -90.0f + progress * 360.0f,
            48,
            Color{90, 180, 255, 230});
    }
    DrawLine(cx - 8, cy, cx - 3, cy, RAYWHITE);
    DrawLine(cx + 3, cy, cx + 8, cy, RAYWHITE);
    DrawLine(cx, cy - 8, cx, cy - 3, RAYWHITE);
    DrawLine(cx, cy + 3, cx, cy + 8, RAYWHITE);
    if (local_stunned && local_combat.dead == 0U) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{70, 200, 255, 45});
        DrawRing(
            Vector2{static_cast<float>(cx), static_cast<float>(cy)},
            34.0f,
            46.0f,
            0.0f,
            360.0f,
            64,
            Color{150, 235, 255, 180});
        DrawText("STUNNED", cx - MeasureText("STUNNED", 24) / 2, cy + 58, 24, Color{190, 245, 255, 230});
    }
    DrawRectangle(18, 18, 290, 112, Color{12, 14, 18, 190});
    DrawText(TextFormat("listen server client %llu", static_cast<unsigned long long>(impl_->host_client)), 30, 30, 16, RAYWHITE);
    DrawText(TextFormat("health %d", local_combat.health), 30, 56, 20, RAYWHITE);
    DrawText(TextFormat("ammo %u / %d", local_combat.ammo, magazine_size), 30, 82, 20, RAYWHITE);
    DrawText(
        impl_->look.captured ? "mouse captured  Esc release" : "click window or press Enter/C to capture mouse",
        30,
        108,
        14,
        Color{190, 200, 215, 255});
    if (impl_->show_stats) {
        const int stats_width = 330;
        const int stats_x = GetScreenWidth() - stats_width - 18;
        DrawRectangle(stats_x, 18, stats_width, 96, Color{12, 14, 18, 170});
        DrawText("listen stats   / hide", stats_x + 12, 28, 16, RAYWHITE);
        DrawText(TextFormat("server frame %u", server.frame()), stats_x + 12, 56, 14, Color{210, 220, 235, 255});
        DrawText(TextFormat("clients %zu", server.client_count()), stats_x + 12, 78, 14, Color{210, 220, 235, 255});
        DrawText(TextFormat("display target %.2f", impl_->display->target_frame()), stats_x + 12, 100, 14, Color{210, 220, 235, 255});
    }
    if (replay_ready) {
        draw_kill_cam_overlay();
    } else if (local_combat.dead != 0U) {
        const char* message = TextFormat("you died, respawning in %.1f", local_combat.respawn_remaining);
        const int font_size = 26;
        const int width = MeasureText(message, font_size);
        DrawRectangle(cx - width / 2 - 18, cy + 34, width + 36, 44, Color{12, 14, 18, 210});
        DrawText(message, cx - width / 2, cy + 44, font_size, Color{255, 230, 230, 255});
    }
    EndDrawing();
}

}  // namespace fps
