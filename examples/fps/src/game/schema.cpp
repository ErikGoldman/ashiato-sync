#include "game/schema.hpp"

#include "game/constants.hpp"
#include "game/cues.hpp"
#include "game/sync_traits.hpp"

namespace fps {

SyncSchema define_schema(ashiato::Registry& registry) {
    const ashiato::Entity transform = ashiato::sync::register_sync_component<FpsTransform>(registry, "FpsTransform");
    const ashiato::Entity velocity = ashiato::sync::register_sync_component<FpsVelocity>(registry, "FpsVelocity");
    const ashiato::Entity combat = ashiato::sync::register_sync_component<FpsCombatState>(registry, "FpsCombatState");
    const ashiato::Entity death = ashiato::sync::register_sync_component<FpsDeathInfo>(registry, "FpsDeathInfo");
    const ashiato::Entity visual = ashiato::sync::register_sync_component<FpsVisual>(registry, "FpsVisual");
    const ashiato::Entity unique_player_id = ashiato::sync::register_sync_component<FpsUniquePlayerId>(registry, "FpsUniquePlayerId");
    const ashiato::Entity owner = ashiato::sync::register_sync_component<ashiato::sync::NetworkOwner>(registry, "ashiato.sync.NetworkOwner");
    const ashiato::Entity no_simulate = registry.component<ashiato::sync::NoSimulate>();
    const ashiato::Entity stunned = registry.register_component<FpsStunned>("FpsStunned");
    registry.register_component<FpsKillCamTarget>("FpsKillCamTarget");
    ashiato::sync::register_sync_component<FpsInput>(registry, "FpsInput");
    registry.register_component<FpsShotEffect>("FpsShotEffect");
    registry.register_component<FpsHitEffect>("FpsHitEffect");
    registry.register_component<FpsSurfaceHitEffect>("FpsSurfaceHitEffect");
    registry.register_component<FpsHitConfirmSuppression>("FpsHitConfirmSuppression");
    registry.register_component<FpsTransformHistory>("FpsTransformHistory");
    registry.register_component<FpsServerFrame>("FpsServerFrame");
    registry.register_component<FpsStunState>("FpsStunState");
    registry.register_component<BotBrain>("BotBrain");
    (void)ashiato::sync::set_fractional_tick_sampled(registry, transform);
    (void)ashiato::sync::register_sync_cue<ShotCue>(registry);
    (void)ashiato::sync::register_sync_cue<SurfaceHitCue>(registry);
    (void)ashiato::sync::register_sync_cue<PlayerHitCue>(registry);
    (void)ashiato::sync::register_sync_cue<HitConfirmCue>(registry);
    (void)ashiato::sync::register_sync_cue<PlayerDeathCue>(registry);
    (void)ashiato::sync::set_client_input_component<FpsInput>(registry);
    return SyncSchema{
        ashiato::sync::define_archetype(
            registry,
            ashiato::sync::SyncArchetypeDesc{
                "FpsCharacter",
                {
                    {no_simulate, ashiato::sync::ReplicationAudience::All},
                    {stunned, ashiato::sync::ReplicationAudience::All},
                },
                {
                {transform, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate},
                {velocity, ashiato::sync::ReplicationAudience::All},
                {combat, ashiato::sync::ReplicationAudience::All},
                {death, ashiato::sync::ReplicationAudience::All},
                {visual, ashiato::sync::ReplicationAudience::All},
                {unique_player_id, ashiato::sync::ReplicationAudience::All},
                {owner, ashiato::sync::ReplicationAudience::All},
                }})};
}

ashiato::Entity spawn_character(
    ashiato::Registry& registry,
    const SyncSchema& schema,
    Vector3 position,
    Color color,
    ashiato::sync::ClientId owner) {
    const ashiato::Entity entity = registry.create();
    registry.add<FpsTransform>(entity, FpsTransform{position.x, position.y, position.z, 0.0f, 0.0f});
    registry.add<FpsVelocity>(entity, FpsVelocity{});
    registry.add<FpsCombatState>(entity, FpsCombatState{});
    registry.add<FpsDeathInfo>(entity, FpsDeathInfo{});
    registry.add<FpsVisual>(
        entity,
        FpsVisual{
            capsule_radius,
            capsule_height,
            color.r,
            color.g,
            color.b,
            255,
            0});
    registry.add<FpsInput>(entity, FpsInput{});
    registry.add<FpsUniquePlayerId>(entity, FpsUniquePlayerId{entity.value});
    if (registry.get<ashiato::sync::SyncSettings>().role == ashiato::sync::SyncRole::Server) {
        registry.add<FpsTransformHistory>(entity, FpsTransformHistory{});
    }
    if (owner != ashiato::sync::invalid_client_id) {
        (void)ashiato::sync::set_owner(registry, entity, owner);
    }
    registry.add<ashiato::sync::Replicated>(entity, ashiato::sync::Replicated{schema.character});
    return entity;
}

}  // namespace fps
