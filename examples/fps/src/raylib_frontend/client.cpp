#include "app.hpp"

#include "game/components.hpp"
#include "game/constants.hpp"
#include "game/jobs.hpp"
#include "game/math.hpp"
#include "game/schema.hpp"
#include "net.hpp"
#include "replay.hpp"
#include "rendering.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <raylib.h>
#include <rlgl.h>

namespace fps {

struct FpsEntityModeSelector {
    ecs::Registry* registry = nullptr;

    kage::sync::ReplicationClientMode operator()(const kage::sync::ReplicatedEntityUpdateView& update) const {
        kage::sync::NetworkOwner owner{};
        if (registry != nullptr && update.try_get(*registry, owner) &&
            owner.client == registry->get<kage::sync::SyncSettings>().local_client) {
            return kage::sync::ReplicationClientMode::Predict;
        }
        return kage::sync::ReplicationClientMode::BufferedInterpolation;
    }
};

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
    DrawRectangleLinesEx(Rectangle{2.0f, 2.0f, static_cast<float>(width - 4), static_cast<float>(height - 4)}, 4.0f, Color{230, 40, 35, 180});
    DrawText(label, width / 2 - label_width / 2, 22, font_size, Color{255, 245, 235, 255});
    DrawText("killer perspective", width / 2 - MeasureText("killer perspective", 14) / 2, 58, 14, Color{255, 185, 170, 255});
}

void run_client(const AppConfig& config) {
    ecs::Registry registry;
    const SyncSchema schema = define_schema(registry);
    (void)schema;
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClientOptions client_options;
    client_options.connect_token = "fps";
    client_options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    client_options.fixed_dt_seconds = fixed_dt;
    client_options.rollback_policy = kage::sync::ReplicationRollbackPolicy::OnlyAffected;
    client_options.interpolation_buffer_frames = 2;
    client_options.interpolation_buffer_capacity_frames = 64;
    client_options.entity_mode_selector = FpsEntityModeSelector{&registry};
#ifdef KAGE_SYNC_ENABLE_TRACING
    client_options.trace = make_trace_options(config);
#endif
    kage::sync::ReplicationClient client(client_options);
    register_game_jobs(registry, client);
    kage::sync::FractionalTickSampler client_display(client);
    std::unique_ptr<kage::sync::FractionalTickSampler> death_cam_display;

    SocketHandle socket = make_udp_socket(0);
    const sockaddr_in server_address = make_address(config.host, config.port);

    InitWindow(1280, 720, "kage-sync FPS");
    InitAudioDevice();
    EnableCursor();
    SetTargetFPS(120);
    Sound shot_sound = make_tone(180.0f, 0.09f, 0.45f);
    Sound hit_confirm_sound = make_tone(720.0f, 0.08f, 0.32f);
    Sound took_damage_sound = make_tone(260.0f, 0.16f, 0.45f);
    Sound death_sound = make_tone(95.0f, 0.45f, 0.55f);

    FpsInput current_input{};
    MouseLookState look;
    const kage::sync::examples::NetworkSimulatorSettings initial_link_settings{
        std::max(0.0, config.latency_ms),
        std::max(0.0, config.jitter_ms),
        std::clamp(config.loss_percent, 0.0, 100.0),
        std::max(0.0, config.link_bandwidth_kbps),
        static_cast<std::size_t>(std::max(0.0, config.link_queue_kb) * 1024.0)};
    ClientLinkSimulator incoming_link{initial_link_settings, 0x13572468U};
    ClientLinkSimulator outgoing_link{initial_link_settings, 0x24681357U};
    double link_time_seconds = 0.0;
    std::vector<WallParticle> particles;
    particles.reserve(256);
    std::unordered_map<std::uint64_t, std::uint8_t> previous_dead;
    FpsDeathCamClient death_cam;
    bool was_local_dead = false;
    bool show_latency_stats = true;
    double packet_drop_remaining_seconds = 0.0;
    client.set_packet_sender([&packet_drop_remaining_seconds, &outgoing_link, &link_time_seconds](const ecs::BitBuffer& packet) {
        if (packet_drop_remaining_seconds <= 0.0) {
            (void)outgoing_link.enqueue(0, packet, link_time_seconds);
        }
    });

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        link_time_seconds += static_cast<double>(dt);
        packet_drop_remaining_seconds =
            std::max(0.0, packet_drop_remaining_seconds - static_cast<double>(dt));
        const double display_target = client.fractional_tick_target_frame();
        const kage::sync::SyncFrame display_target_frame =
            display_target > 0.0 && std::isfinite(display_target)
            ? static_cast<kage::sync::SyncFrame>(std::floor(display_target))
            : 0U;
        current_input = read_player_input(current_input, look, display_target_frame);
        if (IsKeyPressed(KEY_SLASH)) {
            show_latency_stats = !show_latency_stats;
        }
        if (IsKeyPressed(KEY_ZERO)) {
            packet_drop_remaining_seconds = 3.0;
        }
        adjust_link_settings(incoming_link, outgoing_link);
        (void)client.set_input(registry, current_input);

        const bool dropping_packets = packet_drop_remaining_seconds > 0.0;
        ecs::BitBuffer received;
        while (receive_packet(socket, received)) {
            if (!dropping_packets) {
                (void)incoming_link.enqueue(0, received, link_time_seconds);
            }
        }
        if (dropping_packets) {
            (void)incoming_link.drop_queued(link_time_seconds);
        } else {
            incoming_link.deliver_ready(link_time_seconds, [&client](int, const ecs::BitBuffer& packet) {
                client.receive_packet(packet);
            });
        }
        (void)client.tick(registry, dt);
        if (dropping_packets) {
            (void)outgoing_link.drop_queued(link_time_seconds);
        } else {
            outgoing_link.deliver_ready(link_time_seconds, [socket, server_address](int, const ecs::BitBuffer& packet) {
                send_packet(socket, server_address, packet);
            });
        }

        auto find_local_entity = [](ecs::Registry& source) {
            ecs::Entity result;
            const kage::sync::ClientId local_client_id = source.get<kage::sync::SyncSettings>().local_client;
            source.view<const kage::sync::NetworkOwner>().each([&result, local_client_id](ecs::Entity entity, const kage::sync::NetworkOwner& owner) {
                if (owner.client == local_client_id) {
                    result = entity;
                }
            });
            return result;
        };

        const ecs::Entity live_local_entity = find_local_entity(registry);
        bool is_local_dead = false;
        if (live_local_entity && registry.alive(live_local_entity)) {
            if (const FpsCombatState* combat = registry.try_get<FpsCombatState>(live_local_entity)) {
                is_local_dead = combat->dead != 0U;
            }
        }
        if (is_local_dead && !was_local_dead && !death_cam.active()) {
            death_cam.start(config.host, config.replay_port, registry.get<kage::sync::SyncSettings>().local_client);
            previous_dead.clear();
        }
        was_local_dead = is_local_dead;
        death_cam.tick(dt);

        const ecs::Entity replay_local_entity = death_cam.active() ? find_local_entity(death_cam.registry()) : ecs::Entity{};
        const bool replay_ready = replay_local_entity != ecs::Entity{};
        if (replay_ready && death_cam_display == nullptr) {
            death_cam_display = std::make_unique<kage::sync::FractionalTickSampler>(death_cam.client());
        } else if (!replay_ready) {
            death_cam_display.reset();
        }
        ecs::Registry& render_registry = replay_ready ? death_cam.registry() : registry;
        kage::sync::ReplicationClient& render_client = replay_ready ? death_cam.client() : client;
        kage::sync::FractionalTickSampler& render_display_source =
            replay_ready ? *death_cam_display : client_display;
        ecs::Entity local_entity = find_local_entity(render_registry);
        const std::vector<kage::sync::FractionalTickSample>& render_display =
            render_display_source.entities(render_registry);
        if (replay_ready) {
            const ecs::Entity kill_cam_target = render_registry.component<FpsKillCamTarget>();
            ecs::Entity ecs_target;
            render_registry.view<const FpsTransform>().with_tags<const FpsKillCamTarget>().each(
                [&ecs_target](ecs::Entity entity, const FpsTransform&) {
                    if (!ecs_target) {
                        ecs_target = entity;
                    }
                });
            const std::uint64_t replay_target_player_id = death_cam.target_player_id();
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

        update_effects(render_registry, dt);
        for (WallParticle& particle : particles) {
            particle.seconds -= dt;
            particle.position = add(particle.position, scale(particle.velocity, dt));
            particle.velocity.y -= 4.0f * dt;
        }
        particles.erase(
            std::remove_if(particles.begin(), particles.end(), [](const WallParticle& particle) {
                return particle.seconds <= 0.0f;
            }),
            particles.end());

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
                    local_stunned = render_registry.has(local_entity, render_registry.component<FpsStunned>());
                    local_combat = *combat;
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
            if (visual == nullptr || combat == nullptr) {
                continue;
            }
            const std::uint8_t was_dead = previous_dead[sample.local_entity.value];
            if (combat->dead != 0U && was_dead == 0U) {
                spawn_death_particles(
                    particles,
                    Vector3{transform.x, transform.y + visual->height * 0.5f, transform.z},
                    Color{visual->r, visual->g, visual->b, visual->a});
            }
            previous_dead[sample.local_entity.value] = combat->dead;
            if (combat->dead != 0U) {
                continue;
            }
            if (sample.local_entity == local_entity) {
                continue;
            }
            Color color{visual->r, visual->g, visual->b, visual->a};
            if (render_registry.contains<FpsHitEffect>(sample.local_entity)) {
                color = Color{255, 80, 80, 255};
            }
            draw_character_body(Vector3{transform.x, transform.y, transform.z}, *visual, color);
            draw_third_person_gun(transform, *visual);
            if (render_registry.has(sample.local_entity, render_registry.component<FpsStunned>())) {
                draw_stunned_effect(transform, *visual);
            }
            if (render_registry.contains<FpsShotEffect>(sample.local_entity)) {
                const Vector3 muzzle = third_person_muzzle_position(transform, *visual);
                DrawSphere(muzzle, 0.12f, Color{255, 230, 80, 220});
            }
        }
        for (const WallParticle& particle : particles) {
            const float alpha = std::clamp(particle.seconds / 0.35f, 0.0f, 1.0f);
            Color particle_color = particle.color;
            particle_color.a = static_cast<unsigned char>(alpha * static_cast<float>(particle.color.a));
            DrawSphere(particle.position, 0.035f, particle_color);
        }
        render_registry.view<const FpsSurfaceHitEffect>().each([](ecs::Entity, const FpsSurfaceHitEffect& effect) {
            for (const WallParticle& particle : effect.particles) {
                const float alpha = std::clamp(particle.seconds / 0.35f, 0.0f, 1.0f);
                Color particle_color = particle.color;
                particle_color.a = static_cast<unsigned char>(alpha * static_cast<float>(particle.color.a));
                DrawSphere(particle.position, 0.035f, particle_color);
            }
        });
        EndMode3D();
        if (has_local && local_combat.dead == 0U) {
            draw_viewmodel_gun(local_shot_seconds);
        }
        rlDisableDepthTest();

        render_registry.view<FpsShotEffect>().each([&shot_sound](ecs::Entity entity, FpsShotEffect& effect) {
            if (effect.sound_played == 0U) {
                PlaySound(shot_sound);
                effect.sound_played = 1;
            }
            (void)entity;
        });
        render_registry.view<FpsHitEffect>().each([&hit_confirm_sound, &took_damage_sound, &death_sound, local_entity](ecs::Entity entity, FpsHitEffect& effect) {
            if (effect.sound_played != 0U) {
                return;
            }
            if (effect.sound == FpsHitSound::ConfirmedHit) {
                PlaySound(hit_confirm_sound);
                effect.sound_played = 1;
                return;
            }
            if (entity == local_entity && effect.sound == FpsHitSound::TookDamage) {
                PlaySound(took_damage_sound);
                effect.sound_played = 1;
                return;
            }
            if (entity == local_entity && effect.sound == FpsHitSound::Died) {
                PlaySound(death_sound);
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

        DrawRectangle(18, 18, 250, 86, Color{12, 14, 18, 190});
        DrawText(TextFormat("health %d", local_combat.health), 30, 30, 20, RAYWHITE);
        DrawText(TextFormat("ammo %u / %d", local_combat.ammo, magazine_size), 30, 56, 20, RAYWHITE);
        DrawText(
            replay_ready
                ? TextFormat("replay client %llu", static_cast<unsigned long long>(render_registry.get<kage::sync::SyncSettings>().local_client))
                : TextFormat("client %llu", static_cast<unsigned long long>(render_registry.get<kage::sync::SyncSettings>().local_client)),
            30,
            82,
            14,
            Color{190, 200, 215, 255});
        if (!replay_ready) {
            DrawText(
                look.captured
                    ? TextFormat(
                          "mouse captured  yaw %.2f pitch %.2f  delta %.1f %.1f  Esc release",
                          look.yaw,
                          look.pitch,
                          look.last_delta.x,
                          look.last_delta.y)
                    : "click window or press Enter/C to capture mouse",
                30,
                106,
                14,
                Color{190, 200, 215, 255});
            const kage::sync::ReplicationClientTimingStats& timing = client.timing_stats();
            DrawRectangle(18, 132, 470, 164, Color{12, 14, 18, 170});
            DrawText("controls", 30, 142, 16, RAYWHITE);
            DrawText("WASD move   Mouse aim   Space jump   R reload", 30, 168, 14, Color{210, 220, 235, 255});
            DrawText("Left mouse fire   Enter/C capture mouse   Esc release", 30, 190, 14, Color{210, 220, 235, 255});
            DrawText("Network hotkeys", 30, 220, 14, Color{190, 210, 230, 255});
            DrawText("1/2 in latency   3/4 in jitter", 30, 242, 14, Color{190, 210, 230, 255});
            DrawText("5/6 out latency  7/8 out jitter   / stats", 30, 262, 14, Color{190, 210, 230, 255});
            DrawText("0 drop all packets for 3s", 30, 282, 14, Color{190, 210, 230, 255});
            const int stats_width = 470;
            const int stats_x = GetScreenWidth() - stats_width - 18;
            if (show_latency_stats) {
                DrawRectangle(stats_x, 18, stats_width, 194, Color{12, 14, 18, 170});
                DrawText("network stats   / hide", stats_x + 12, 28, 16, RAYWHITE);
                DrawText(
                    TextFormat(
                        "in latency %.0f ms [1/2]  jitter %.0f ms [3/4]  queued %zu",
                        incoming_link.settings.latency_ms,
                        incoming_link.settings.jitter_ms,
                        incoming_link.size()),
                    stats_x + 12,
                    56,
                    14,
                    Color{190, 210, 230, 255});
                DrawText(
                    TextFormat(
                        "out latency %.0f ms [5/6]  jitter %.0f ms [7/8]  queued %zu",
                        outgoing_link.settings.latency_ms,
                        outgoing_link.settings.jitter_ms,
                        outgoing_link.size()),
                    stats_x + 12,
                    78,
                    14,
                    Color{190, 210, 230, 255});
                DrawText(
                    TextFormat(
                        "interp delay current %.2f  target %u  desired %u",
                        client.continuous_interpolation_frames_behind(),
                        timing.target_interpolation_buffer_frames,
                        timing.desired_interpolation_buffer_frames),
                    stats_x + 12,
                    100,
                    14,
                    Color{210, 220, 235, 255});
                DrawText(
                    TextFormat(
                        "latency %.1f frames  jitter %.1f",
                        timing.latency_frames,
                        timing.jitter_frames),
                    stats_x + 12,
                    122,
                    14,
                    Color{210, 220, 235, 255});
                DrawText(
                    TextFormat(
                        "buffer current %u  capacity %zu",
                        client.current_interpolation_buffer_frames(),
                        client.options().interpolation_buffer_capacity_frames),
                    stats_x + 12,
                    144,
                    14,
                    Color{210, 220, 235, 255});
                DrawText(
                    TextFormat(
                        "prediction lead current %.2f  target %u  measured %.1f  scale %.2f",
                        client.continuous_prediction_frames_ahead(),
                        timing.target_prediction_lead_frames,
                        timing.measured_prediction_lead_frames,
                        timing.prediction_time_dilation),
                    stats_x + 12,
                    164,
                    14,
                    Color{210, 220, 235, 255});
                DrawText(
                    packet_drop_remaining_seconds > 0.0
                        ? TextFormat("packet drop active %.1fs remaining [0]", packet_drop_remaining_seconds)
                        : "packet drop inactive [0]",
                    stats_x + 12,
                    186,
                    14,
                    packet_drop_remaining_seconds > 0.0
                        ? Color{255, 170, 120, 255}
                        : Color{190, 210, 230, 255});
            } else {
                DrawRectangle(stats_x, 18, 126, 28, Color{12, 14, 18, 170});
                DrawText("/ show stats", stats_x + 12, 26, 14, Color{190, 210, 230, 255});
            }
        } else {
            draw_kill_cam_overlay();
        }
        if (local_combat.dead != 0U) {
            const char* message = TextFormat("you died, respawning in %.1f", local_combat.respawn_remaining);
            const int font_size = 26;
            const int width = MeasureText(message, font_size);
            DrawRectangle(
                cx - width / 2 - 18,
                cy + 34,
                width + 36,
                44,
                Color{12, 14, 18, 210});
            DrawText(message, cx - width / 2, cy + 44, font_size, Color{255, 230, 230, 255});
        }
        EndDrawing();
    }

    UnloadSound(shot_sound);
    UnloadSound(hit_confirm_sound);
    UnloadSound(took_damage_sound);
    UnloadSound(death_sound);
    CloseAudioDevice();
    CloseWindow();
#ifdef KAGE_SYNC_ENABLE_TRACING
    client.close_trace();
#endif
    close_socket(socket);
}

}  // namespace fps
