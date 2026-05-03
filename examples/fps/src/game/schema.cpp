#include "game/schema.hpp"

#include "game/constants.hpp"
#include "game/cues.hpp"
#include "game/sync_traits.hpp"

namespace fps {

SyncSchema define_schema(ecs::Registry& registry) {
    const ecs::Entity transform = kage::sync::register_sync_component<FpsTransform>(registry, "FpsTransform");
    const ecs::Entity velocity = kage::sync::register_sync_component<FpsVelocity>(registry, "FpsVelocity");
    const ecs::Entity combat = kage::sync::register_sync_component<FpsCombatState>(registry, "FpsCombatState");
    const ecs::Entity visual = kage::sync::register_sync_component<FpsVisual>(registry, "FpsVisual");
    const ecs::Entity owner = kage::sync::register_sync_component<kage::sync::NetworkOwner>(registry, "kage.sync.NetworkOwner");
    const ecs::Entity no_simulate = registry.component<kage::sync::NoSimulate>();
    kage::sync::register_sync_component<FpsInput>(registry, "FpsInput");
    registry.register_component<FpsShotEffect>("FpsShotEffect");
    registry.register_component<FpsHitEffect>("FpsHitEffect");
    registry.register_component<FpsSurfaceHitEffect>("FpsSurfaceHitEffect");
    registry.register_component<FpsHitConfirmSuppression>("FpsHitConfirmSuppression");
    registry.register_component<FpsTransformHistory>("FpsTransformHistory");
    registry.register_component<FpsServerFrame>("FpsServerFrame");
    registry.register_component<BotBrain>("BotBrain");
    (void)kage::sync::set_display_interpolated(registry, transform);
    (void)kage::sync::register_sync_cue<ShotCue>(registry);
    (void)kage::sync::register_sync_cue<SurfaceHitCue>(registry);
    (void)kage::sync::register_sync_cue<PlayerHitCue>(registry);
    (void)kage::sync::register_sync_cue<HitConfirmCue>(registry);
    (void)kage::sync::set_client_input_component<FpsInput>(registry);
    return SyncSchema{
        kage::sync::define_archetype(
            registry,
            kage::sync::SyncArchetypeDesc{
                "FpsCharacter",
                {{no_simulate, kage::sync::ReplicationAudience::All}},
                {
                {transform, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate},
                {velocity, kage::sync::ReplicationAudience::All},
                {combat, kage::sync::ReplicationAudience::All},
                {visual, kage::sync::ReplicationAudience::All},
                {owner, kage::sync::ReplicationAudience::All},
                }})};
}

ecs::Entity spawn_character(
    ecs::Registry& registry,
    const SyncSchema& schema,
    Vector3 position,
    Color color,
    kage::sync::ClientId owner) {
    const ecs::Entity entity = registry.create();
    registry.add<FpsTransform>(entity, FpsTransform{position.x, position.y, position.z, 0.0f, 0.0f});
    registry.add<FpsVelocity>(entity, FpsVelocity{});
    registry.add<FpsCombatState>(entity, FpsCombatState{});
    registry.add<FpsVisual>(
        entity,
        FpsVisual{
            capsule_radius,
            capsule_height,
            color.r,
            color.g,
            color.b,
            255});
    registry.add<FpsInput>(entity, FpsInput{});
    if (registry.get<kage::sync::SyncSettings>().role == kage::sync::SyncRole::Server) {
        registry.add<FpsTransformHistory>(entity, FpsTransformHistory{});
    }
    if (owner != kage::sync::invalid_client_id) {
        (void)kage::sync::set_owner(registry, entity, owner);
    }
    registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{schema.character});
    return entity;
}

}  // namespace fps
