# Engine G3 — Projectile, Ability, Status Effect and Squad AI

Engine G3 adds an authoritative extension session around `RtsGameSession`. It preserves the existing command, diplomacy, visibility and simulation boundaries while adding canonical G3 state, archive and hashing.

## Projectile runtime

- weapon projectiles opt in through `Weapon::projectileDefinitionId`;
- non-projectile weapons retain the existing immediate-hit behavior;
- projectiles use deterministic Q16 movement and stable projectile IDs;
- homing, lifetime, hit radius, splash damage, friendly-fire policy, damage type and impact status are data driven;
- weapon fire spawns the projectile after the base combat Tick, so impact can never occur in the same Tick as launch.

## Ability runtime

- abilities use a deterministic command stream with Tick, issuer and sequence identity;
- loadouts and cooldowns are authoritative state;
- Self, Entity and Point targets are supported;
- cast time, range, ally/enemy policy and ordered effect lists are data driven;
- effects include damage, heal, status application and projectile spawn;
- Tick 0 targeting uses authoritative position, range and diplomacy because no visibility snapshot exists yet; later Ticks enforce Fog/Vision visibility.

## Status effects

- RefreshDuration, AddStacks and Independent policies;
- bounded stacks and duration;
- periodic heal or damage;
- movement, damage and armor modifiers;
- stun control consumed by movement and combat systems;
- status controls are derived from canonical status instances;
- non-stunned movement preserves the engine's legacy minimum-one-substep contract, including entities configured with zero base speed; Stun is the authoritative mechanism that suppresses movement completely.

## Squad AI

- stable squad IDs and sorted membership;
- Assault, Defend, Retreat and Hold objectives;
- line, column, box and wedge formation slots from Engine G1;
- visible hostile target selection with stable entity tie breaking;
- health-based retreat and reserved deterministic command sequences.

## Persistence and multiplayer identity

`RtsG3GameSession::encode()` nests the complete `RtsGameSessionArchive` and adds ability commands, casts, cooldowns, projectiles, status instances, bindings, squads and sequence state. `authoritativeHash()` combines the base session hash, G3 definition identity and canonical runtime state.

The G3 ability command stream is deliberately separate from the legacy `TickCommand` enum. A future G3 lockstep adapter can transport both streams without changing existing saves or silently turning `CastAbility` into an ignored base command.

## Validation

The focused G3 gate builds the new runtime together with the existing combat, combat-integration, simulation-persistence and roguelite-persistence targets. It verifies delayed projectile impact, ability cooldown and periodic status behavior, stun recovery, squad command generation, archive/hash continuity and compatibility with stationary tower-defense units before publishing source changes.
