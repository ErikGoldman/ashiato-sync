#include "app.hpp"

#include "game/components.hpp"
#include "game/constants.hpp"
#include "game/jobs.hpp"
#include "game/math.hpp"
#include "game/schema.hpp"
#include "net.hpp"
#include "rendering.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    kage::sync::ReplicationClient client(client_options);
#ifdef KAGE_SYNC_ENABLE_TRACING
    std::unique_ptr<kage::sync::KTraceDirectoryWriter> trace_writer = make_trace_writer(config);
    if (trace_writer != nullptr) {
        client.set_tracer(&trace_writer->tracer());
    }
#endif
    register_game_jobs(registry, client);

    SocketHandle socket = make_udp_socket(0);
    const sockaddr_in server_address = make_address(config.host, config.port);

    InitWindow(1280, 720, "kage-sync FPS");
    InitAudioDevice();
    EnableCursor();
    SetTargetFPS(120);
    Sound shot_sound = make_tone(180.0f, 0.09f, 0.45f);
    Sound hit_sound = make_tone(520.0f, 0.12f, 0.35f);

    FpsInput current_input{};
    MouseLookState look;
    const kage::sync::SimulatedLinkSettings initial_link_settings{
        std::max(0.0, config.latency_ms),
        std::max(0.0, config.jitter_ms),
        0.0};
    ClientLinkSimulator incoming_link{initial_link_settings, 0x13572468U};
    ClientLinkSimulator outgoing_link{initial_link_settings, 0x24681357U};
    double link_time_seconds = 0.0;
    std::vector<WallParticle> particles;
    particles.reserve(256);
    std::unordered_map<std::uint64_t, std::uint8_t> previous_dead;
    bool show_latency_stats = true;
    double packet_drop_remaining_seconds = 0.0;
    client.set_packet_sender([&packet_drop_remaining_seconds, &outgoing_link, &link_time_seconds](const kage::sync::BitBuffer& packet) {
        if (packet_drop_remaining_seconds <= 0.0) {
            (void)outgoing_link.enqueue(0, packet, link_time_seconds);
        }
    });

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        link_time_seconds += static_cast<double>(dt);
        packet_drop_remaining_seconds =
            std::max(0.0, packet_drop_remaining_seconds - static_cast<double>(dt));
        current_input = read_player_input(current_input, look);
        if (IsKeyPressed(KEY_SLASH)) {
            show_latency_stats = !show_latency_stats;
        }
        if (IsKeyPressed(KEY_ZERO)) {
            packet_drop_remaining_seconds = 3.0;
        }
        adjust_link_settings(incoming_link, outgoing_link);
        (void)client.set_input(registry, current_input);

        const bool dropping_packets = packet_drop_remaining_seconds > 0.0;
        kage::sync::BitBuffer received;
        while (receive_packet(socket, received)) {
            if (!dropping_packets) {
                (void)incoming_link.enqueue(0, received, link_time_seconds);
            }
        }
        if (dropping_packets) {
            incoming_link.deliver_ready(link_time_seconds, [](int, const kage::sync::BitBuffer&) {});
        } else {
            incoming_link.deliver_ready(link_time_seconds, [&client](int, const kage::sync::BitBuffer& packet) {
                client.receive_packet(packet);
            });
        }
        (void)client.tick(registry, dt);
        if (dropping_packets) {
            outgoing_link.deliver_ready(link_time_seconds, [](int, const kage::sync::BitBuffer&) {});
        } else {
            outgoing_link.deliver_ready(link_time_seconds, [socket, server_address](int, const kage::sync::BitBuffer& packet) {
                send_packet(socket, server_address, packet);
            });
        }

        ecs::Entity local_entity;
        const kage::sync::ClientId local_client_id = registry.get<kage::sync::SyncSettings>().local_client;
        registry.view<const kage::sync::NetworkOwner>().each([&local_entity, local_client_id](ecs::Entity entity, const kage::sync::NetworkOwner& owner) {
            if (owner.client == local_client_id) {
                local_entity = entity;
            }
        });

        update_effects(registry, dt);
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
        bool has_local = false;
        if (local_entity && registry.alive(local_entity)) {
            if (const FpsTransform* transform = registry.try_get<FpsTransform>(local_entity)) {
                has_local = true;
                camera.position = eye_position(*transform);
                camera.target = add(camera.position, forward_from_angles(transform->yaw, transform->pitch));
                if (const FpsCombatState* combat = registry.try_get<FpsCombatState>(local_entity)) {
                    local_combat = *combat;
                }
                if (const FpsShotEffect* shot = registry.try_get<FpsShotEffect>(local_entity)) {
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
        for (const kage::sync::DisplayInterpolationSample& sample : client.display_interpolation_frame(registry).entities) {
            FpsTransform transform{};
            if (!sample.try_get_display_value(registry, transform)) {
                continue;
            }
            const FpsVisual* visual = registry.try_get<FpsVisual>(sample.local_entity);
            const FpsCombatState* combat = registry.try_get<FpsCombatState>(sample.local_entity);
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
            if (registry.contains<FpsHitEffect>(sample.local_entity)) {
                color = Color{255, 80, 80, 255};
            }
            draw_capsule(Vector3{transform.x, transform.y, transform.z}, *visual, color);
            draw_third_person_gun(transform);
            if (registry.contains<FpsShotEffect>(sample.local_entity)) {
                const Vector3 muzzle = third_person_muzzle_position(transform);
                DrawSphere(muzzle, 0.12f, Color{255, 230, 80, 220});
            }
        }
        for (const WallParticle& particle : particles) {
            const float alpha = std::clamp(particle.seconds / 0.35f, 0.0f, 1.0f);
            Color particle_color = particle.color;
            particle_color.a = static_cast<unsigned char>(alpha * static_cast<float>(particle.color.a));
            DrawSphere(particle.position, 0.035f, particle_color);
        }
        registry.view<const FpsSurfaceHitEffect>().each([](ecs::Entity, const FpsSurfaceHitEffect& effect) {
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

        registry.view<FpsShotEffect>().each([&shot_sound](ecs::Entity entity, FpsShotEffect& effect) {
            if (effect.sound_played == 0U) {
                PlaySound(shot_sound);
                effect.sound_played = 1;
            }
            (void)entity;
        });
        registry.view<FpsHitEffect>().each([&hit_sound, local_entity](ecs::Entity entity, FpsHitEffect& effect) {
            if (entity == local_entity && effect.sound_played == 0U) {
                PlaySound(hit_sound);
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

        DrawRectangle(18, 18, 250, 86, Color{12, 14, 18, 190});
        DrawText(TextFormat("health %d", local_combat.health), 30, 30, 20, RAYWHITE);
        DrawText(TextFormat("ammo %u / %d", local_combat.ammo, magazine_size), 30, 56, 20, RAYWHITE);
        DrawText(TextFormat("client %llu", static_cast<unsigned long long>(registry.get<kage::sync::SyncSettings>().local_client)), 30, 82, 14, Color{190, 200, 215, 255});
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
    UnloadSound(hit_sound);
    CloseAudioDevice();
    CloseWindow();
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (trace_writer != nullptr) {
        trace_writer->close();
    }
#endif
    close_socket(socket);
}

}  // namespace fps
