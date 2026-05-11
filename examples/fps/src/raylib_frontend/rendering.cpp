#include "rendering.hpp"

#include "game/constants.hpp"
#include "game/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rlgl.h>

namespace fps {

Sound make_tone(float frequency, float seconds, float volume) {
    constexpr int sample_rate = 22050;
    const int sample_count = static_cast<int>(static_cast<float>(sample_rate) * seconds);
    std::vector<std::int16_t> samples(static_cast<std::size_t>(sample_count));
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        const float envelope = std::max(0.0f, 1.0f - t / seconds);
        samples[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(std::sin(t * frequency * 6.2831853f) * envelope * volume * 32767.0f);
    }
    Wave wave{};
    wave.frameCount = static_cast<unsigned int>(sample_count);
    wave.sampleRate = sample_rate;
    wave.sampleSize = 16;
    wave.channels = 1;
    wave.data = samples.data();
    return LoadSoundFromWave(wave);
}

void draw_capsule(Vector3 base, const FpsVisual& visual, Color color) {
    const Vector3 bottom{base.x, base.y + visual.radius, base.z};
    const Vector3 top{base.x, base.y + visual.height - visual.radius, base.z};
    DrawCylinderEx(bottom, top, visual.radius, visual.radius, 18, color);
    DrawSphere(bottom, visual.radius, color);
    DrawSphere(top, visual.radius, color);
}

void draw_character_body(Vector3 base, const FpsVisual& visual, Color color) {
    if (visual.style == 1U) {
        const Vector3 center{base.x, base.y + visual.height * 0.5f, base.z};
        const Vector3 size{visual.radius * 2.0f, visual.height, visual.radius * 2.0f};
        DrawCubeV(center, size, color);
        DrawCubeWiresV(center, size, Color{180, 245, 255, 230});
        return;
    }
    draw_capsule(base, visual, color);
}

void draw_arena() {
    DrawCube(Vector3{0.0f, -0.03f, 0.0f}, arena_half_x * 2.0f, 0.06f, arena_half_z * 2.0f, Color{48, 52, 58, 255});
    DrawCubeWires(Vector3{0.0f, arena_height * 0.5f, 0.0f}, arena_half_x * 2.0f, arena_height, arena_half_z * 2.0f, Color{125, 132, 145, 255});
    for (const CoverBox& box : g_cover_boxes) {
        DrawCubeV(box.center, box.size, Color{84, 90, 98, 255});
        DrawCubeWiresV(box.center, box.size, Color{145, 154, 168, 255});
    }
    DrawGrid(16, 1.0f);
}

void draw_viewmodel_gun(float shot_seconds) {
    Camera3D view_camera{};
    view_camera.position = Vector3{0.0f, 0.0f, 0.0f};
    view_camera.target = Vector3{0.0f, 0.0f, 1.0f};
    view_camera.up = Vector3{0.0f, 1.0f, 0.0f};
    view_camera.fovy = 70.0f;
    view_camera.projection = CAMERA_PERSPECTIVE;

    const float recoil = std::clamp(shot_seconds / 0.12f, 0.0f, 1.0f);
    const float z = 1.04f - recoil * 0.08f;
    const float y = -0.46f - recoil * 0.015f;
    const float x = -0.4f;

    rlDisableDepthTest();
    BeginMode3D(view_camera);
    DrawCubeV(Vector3{x, y, z}, Vector3{0.22f, 0.16f, 0.58f}, Color{44, 48, 54, 255});
    DrawCubeWiresV(Vector3{x, y, z}, Vector3{0.22f, 0.16f, 0.58f}, Color{120, 130, 145, 255});
    DrawCubeV(Vector3{x, y + 0.095f, z + 0.04f}, Vector3{0.18f, 0.08f, 0.42f}, Color{28, 30, 35, 255});
    DrawCubeV(Vector3{x, y - 0.14f, z - 0.08f}, Vector3{0.12f, 0.25f, 0.16f}, Color{35, 32, 30, 255});
    DrawCubeV(Vector3{x, y - 0.02f, z + 0.36f}, Vector3{0.12f, 0.11f, 0.24f}, Color{22, 24, 28, 255});
    DrawCubeV(Vector3{x, y - 0.055f, z + 0.55f}, Vector3{0.07f, 0.07f, 0.26f}, Color{18, 20, 24, 255});
    DrawCubeV(Vector3{x - 0.085f, y - 0.19f, z - 0.1f}, Vector3{0.08f, 0.12f, 0.28f}, Color{78, 64, 48, 255});
    DrawCubeV(Vector3{x - 0.19f, y - 0.18f, z - 0.18f}, Vector3{0.16f, 0.13f, 0.32f}, Color{90, 74, 54, 255});
    DrawCubeV(Vector3{x + 0.015f, y - 0.065f, z + 0.18f}, Vector3{0.08f, 0.09f, 0.08f}, Color{20, 22, 26, 255});
    if (recoil > 0.0f) {
        const unsigned char alpha = static_cast<unsigned char>(180.0f * recoil);
        DrawCubeV(Vector3{x, y - 0.015f, z + 0.73f}, Vector3{0.13f, 0.13f, 0.13f}, Color{255, 210, 80, alpha});
        DrawCubeV(Vector3{x, y - 0.015f, z + 0.83f}, Vector3{0.08f, 0.08f, 0.11f}, Color{255, 120, 35, alpha});
    }
    EndMode3D();
    rlEnableDepthTest();
}

void draw_third_person_gun(const FpsTransform& transform, const FpsVisual& visual) {
    const Vector3 origin = eye_position(transform);
    const float yaw_degrees = transform.yaw * RAD2DEG;
    const float pitch_degrees = transform.pitch * RAD2DEG;

    rlPushMatrix();
    rlTranslatef(origin.x, origin.y - 0.22f, origin.z);
    rlRotatef(yaw_degrees, 0.0f, 1.0f, 0.0f);
    rlRotatef(-pitch_degrees, 1.0f, 0.0f, 0.0f);
    if (visual.style == 1U) {
        DrawCubeV(Vector3{0.0f, -0.05f, 0.18f}, Vector3{0.12f, 0.12f, 0.22f}, Color{22, 48, 56, 255});
        DrawCubeWiresV(Vector3{0.0f, -0.05f, 0.18f}, Vector3{0.12f, 0.12f, 0.22f}, Color{120, 235, 255, 255});
        DrawCubeV(Vector3{-0.06f, -0.05f, 0.38f}, Vector3{0.035f, 0.035f, 0.26f}, Color{110, 230, 255, 255});
        DrawCubeV(Vector3{0.06f, -0.05f, 0.38f}, Vector3{0.035f, 0.035f, 0.26f}, Color{110, 230, 255, 255});
        DrawSphere(Vector3{0.0f, -0.05f, 0.54f}, 0.045f, Color{120, 235, 255, 210});
        rlPopMatrix();
        return;
    }
    DrawCubeV(Vector3{0.22f, -0.08f, 0.34f}, Vector3{0.16f, 0.12f, 0.5f}, Color{40, 44, 50, 255});
    DrawCubeWiresV(Vector3{0.22f, -0.08f, 0.34f}, Vector3{0.16f, 0.12f, 0.5f}, Color{120, 130, 145, 255});
    DrawCubeV(Vector3{0.22f, -0.17f, 0.12f}, Vector3{0.09f, 0.2f, 0.12f}, Color{68, 56, 42, 255});
    DrawCubeV(Vector3{0.22f, -0.1f, 0.66f}, Vector3{0.08f, 0.08f, 0.28f}, Color{22, 24, 28, 255});
    DrawCubeV(Vector3{0.22f, -0.02f, 0.26f}, Vector3{0.12f, 0.07f, 0.3f}, Color{26, 28, 32, 255});
    rlPopMatrix();
}

void draw_stunned_effect(const FpsTransform& transform, const FpsVisual& visual) {
    const Vector3 center{transform.x, transform.y + visual.height * 0.55f, transform.z};
    const float radius = visual.height * 0.62f;
    DrawSphere(center, radius, Color{90, 210, 255, 45});
    DrawSphereWires(center, radius, 16, 12, Color{150, 235, 255, 190});
    DrawCircle3D(
        Vector3{center.x, center.y + visual.height * 0.4f, center.z},
        visual.radius * 1.8f,
        Vector3{1.0f, 0.0f, 0.0f},
        90.0f,
        Color{190, 245, 255, 220});
}

Vector3 third_person_muzzle_position(const FpsTransform& transform, const FpsVisual& visual) {
    const Vector3 eye = eye_position(transform);
    if (visual.style == 1U) {
        return add(
            Vector3{eye.x, eye.y - 0.27f, eye.z},
            scale(forward_from_angles(transform.yaw, transform.pitch), 0.62f));
    }
    return add(
        add(
            Vector3{eye.x, eye.y - 0.32f, eye.z},
            scale(flat_right(transform.yaw), 0.22f)),
        scale(forward_from_angles(transform.yaw, transform.pitch), 0.8f));
}

void update_effects(ashiato::Registry& registry, float dt) {
    std::vector<ashiato::Entity> remove_shot;
    std::vector<ashiato::Entity> remove_hit;
    std::vector<ashiato::Entity> remove_surface;
    std::vector<ashiato::Entity> remove_suppressions;
    registry.view<FpsShotEffect>().each([dt, &remove_shot](ashiato::Entity entity, FpsShotEffect& effect) {
        effect.seconds -= dt;
        if (effect.seconds <= 0.0f) {
            remove_shot.push_back(entity);
        }
    });
    registry.view<FpsHitEffect>().each([dt, &remove_hit](ashiato::Entity entity, FpsHitEffect& effect) {
        effect.seconds -= dt;
        if (effect.seconds <= 0.0f) {
            remove_hit.push_back(entity);
        }
    });
    registry.view<FpsSurfaceHitEffect>().each([dt, &remove_surface](ashiato::Entity entity, FpsSurfaceHitEffect& effect) {
        for (WallParticle& particle : effect.particles) {
            particle.seconds -= dt;
            particle.position = add(particle.position, scale(particle.velocity, dt));
            particle.velocity.y -= 4.0f * dt;
        }
        effect.particles.erase(
            std::remove_if(effect.particles.begin(), effect.particles.end(), [](const WallParticle& particle) {
                return particle.seconds <= 0.0f;
            }),
            effect.particles.end());
        if (effect.particles.empty()) {
            remove_surface.push_back(entity);
        }
    });
    registry.view<FpsHitConfirmSuppression>().each([dt, &remove_suppressions](ashiato::Entity entity, FpsHitConfirmSuppression& suppressions) {
        for (auto entry = suppressions.entries.begin(); entry != suppressions.entries.end();) {
            entry->seconds -= dt;
            if (entry->seconds <= 0.0f) {
                entry = suppressions.entries.erase(entry);
            } else {
                ++entry;
            }
        }
        if (suppressions.entries.empty()) {
            remove_suppressions.push_back(entity);
        }
    });
    for (ashiato::Entity entity : remove_shot) {
        registry.remove<FpsShotEffect>(entity);
    }
    for (ashiato::Entity entity : remove_hit) {
        registry.remove<FpsHitEffect>(entity);
    }
    for (ashiato::Entity entity : remove_surface) {
        registry.remove<FpsSurfaceHitEffect>(entity);
    }
    for (ashiato::Entity entity : remove_suppressions) {
        registry.remove<FpsHitConfirmSuppression>(entity);
    }
}

FpsInput read_player_input(
    FpsInput previous,
    MouseLookState& look,
    ashiato::sync::SyncFrame display_target_frame) {
    previous.move_x = 0.0f;
    previous.move_y = 0.0f;
    if (!IsWindowFocused()) {
        if (look.captured) {
            EnableCursor();
        }
        look.captured = false;
        look.skip_delta_frames = 2;
        look.last_delta = Vector2{};
        previous.yaw = look.yaw;
        previous.pitch = look.pitch;
        return previous;
    }

    if (!look.captured) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_C)) {
            DisableCursor();
            look.captured = true;
            look.skip_delta_frames = 2;
            look.pitch = 0.0f;
            look.last_delta = Vector2{};
        }
        previous.yaw = look.yaw;
        previous.pitch = look.pitch;
        return previous;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        EnableCursor();
        look.captured = false;
        look.skip_delta_frames = 2;
        look.last_delta = Vector2{};
        previous.yaw = look.yaw;
        previous.pitch = look.pitch;
        return previous;
    }

    if (IsKeyDown(KEY_A)) {
        previous.move_x += 1.0f;
    }
    if (IsKeyDown(KEY_D)) {
        previous.move_x -= 1.0f;
    }
    if (IsKeyDown(KEY_W)) {
        previous.move_y += 1.0f;
    }
    if (IsKeyDown(KEY_S)) {
        previous.move_y -= 1.0f;
    }

    const Vector2 delta = GetMouseDelta();
    look.last_delta = delta;
    if (look.skip_delta_frames > 0) {
        --look.skip_delta_frames;
    } else {
        const float dx = std::clamp(delta.x, -45.0f, 45.0f);
        const float dy = std::clamp(delta.y, -45.0f, 45.0f);
        look.yaw -= dx * 0.0032f;
        look.pitch = clamp_pitch(look.pitch - dy * 0.0032f);
    }

    if (IsKeyDown(KEY_LEFT)) {
        look.yaw += 2.2f * fixed_dt;
    }
    if (IsKeyDown(KEY_RIGHT)) {
        look.yaw -= 2.2f * fixed_dt;
    }
    if (IsKeyDown(KEY_UP)) {
        look.pitch = clamp_pitch(look.pitch + 2.2f * fixed_dt);
    }
    if (IsKeyDown(KEY_DOWN)) {
        look.pitch = clamp_pitch(look.pitch - 2.2f * fixed_dt);
    }

    if (IsKeyPressed(KEY_HOME)) {
        look.pitch = 0.0f;
    }

    previous.yaw = look.yaw;
    previous.pitch = look.pitch;
    if (IsKeyPressed(KEY_SPACE)) {
        ++previous.jump_seq;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        ++previous.fire_seq;
        previous.shot_interpolation_frame = display_target_frame;
    }
    if (IsKeyPressed(KEY_R)) {
        ++previous.reload_seq;
    }
    return previous;
}

void spawn_wall_particles(std::vector<WallParticle>& particles, Vector3 point, Vector3 normal) {
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(i) * 2.399f;
        Vector3 scatter{std::cos(a) * 0.8f, 0.4f + 0.07f * static_cast<float>(i), std::sin(a) * 0.8f};
        particles.push_back(WallParticle{point, add(scale(normal, 1.8f), scatter), 0.35f});
    }
}

void spawn_death_particles(std::vector<WallParticle>& particles, Vector3 point, Color color) {
    for (int i = 0; i < 28; ++i) {
        const float a = static_cast<float>(i) * 2.399f;
        const float ring = 1.0f + 0.05f * static_cast<float>(i % 5);
        Vector3 velocity{
            std::cos(a) * ring * 2.0f,
            1.4f + 0.11f * static_cast<float>(i % 9),
            std::sin(a) * ring * 2.0f};
        const unsigned char shade = static_cast<unsigned char>(std::min<int>(255, 40 + (i * 19) % 120));
        Color particle_color{
            static_cast<unsigned char>(std::min<int>(255, color.r + shade / 3)),
            static_cast<unsigned char>(std::min<int>(255, color.g + shade / 3)),
            static_cast<unsigned char>(std::min<int>(255, color.b + shade / 3)),
            255};
        particles.push_back(WallParticle{point, velocity, 0.75f, particle_color});
    }
}

void adjust_link_settings(ClientLinkSimulator& incoming_link, ClientLinkSimulator& outgoing_link) {
    auto adjust = [](double& value, double delta, double maximum) {
        value = std::clamp(value + delta, 0.0, maximum);
    };
    if (IsKeyPressed(KEY_ONE)) {
        adjust(incoming_link.settings.latency_ms, 25.0, 500.0);
    }
    if (IsKeyPressed(KEY_TWO)) {
        adjust(incoming_link.settings.latency_ms, -25.0, 500.0);
    }
    if (IsKeyPressed(KEY_THREE)) {
        adjust(incoming_link.settings.jitter_ms, 10.0, 500.0);
    }
    if (IsKeyPressed(KEY_FOUR)) {
        adjust(incoming_link.settings.jitter_ms, -10.0, 500.0);
    }
    if (IsKeyPressed(KEY_FIVE)) {
        adjust(outgoing_link.settings.latency_ms, 25.0, 500.0);
    }
    if (IsKeyPressed(KEY_SIX)) {
        adjust(outgoing_link.settings.latency_ms, -25.0, 500.0);
    }
    if (IsKeyPressed(KEY_SEVEN)) {
        adjust(outgoing_link.settings.jitter_ms, 10.0, 500.0);
    }
    if (IsKeyPressed(KEY_EIGHT)) {
        adjust(outgoing_link.settings.jitter_ms, -10.0, 500.0);
    }
}

}  // namespace fps
