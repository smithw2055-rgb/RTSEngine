# Engine G3 Closure — Content, Presentation, Network and Script Integration

Engine G3 Closure turns the authoritative Projectile, Ability, Status Effect and
Squad AI primitives into integration surfaces that a game, editor or online
runtime can consume without bypassing deterministic authority.

## G3ContentCodec

`RTSEngine/Rts/G3Content.h` provides a bounded, versioned and canonical binary
format for:

- projectile definitions;
- status-effect definitions;
- ability definitions and ordered effects;
- unit and building projectile bindings.

The codec validates IDs and cross-references, sorts definitions into canonical
order, computes a deterministic content hash, and can wrap the payload in the
existing `CookedAsset` binary envelope. It deliberately uses `AssetType::Binary`
with a G3-specific magic and schema version so existing asset enum values and
manifests are not renumbered.

`G3ContentCodec::apply()` installs a validated bundle before the G3 session
configuration is frozen. A future visual editor or command-line cooker can use
the same codec rather than inventing a second runtime format.

## G3PresentationBridge

`RTSEngine/RtsPresentation/G3PresentationBridge.h` projects G3 authority into:

- synthetic projectile presentation entities;
- stable presentation events for launch, impact, casting and status changes;
- one-frame ability telegraph descriptors from `AbilityCastStarted`;
- visible status descriptors with remaining duration.

The bridge uses presentation domain `4` without changing the existing public
event-domain enum. Hidden entities, projectiles and events are filtered through
the observed team's authoritative visibility state. Friendly projectiles remain
visible to their owning team.

The bridge does not mutate the simulation. GPU trail meshes, decals, telegraph
geometry and event-to-VFX/audio bindings remain presentation-backend choices.

## RtsG3LockstepSession

`RTSEngine/Rts/G3Lockstep.h` transports base RTS commands and G3 ability
commands in a shared deterministic frame while retaining their existing command
types. It provides:

- peer ownership and sequence assignment;
- input delay and prediction through the existing lockstep coordinator;
- G3 archive checkpoints and rollback replay;
- authoritative G3 hash exchange and desync monitoring;
- reconnect snapshots containing future frames and the complete G3 archive.

No legacy `TickCommand` enum value is added or renumbered. Ability commands stay
explicit and are submitted through `RtsG3GameSession::submitAbility()`.

## RtsG3ScriptExtension

`RTSEngine/RtsScripting/G3ScriptExtension.h` registers the `Engine.G3`
RealScript module:

- `CastSelf(caster, abilityId)`;
- `CastEntity(caster, abilityId, target)`;
- `CastPoint(caster, abilityId, x, y)`;
- `HasStatus(entity, statusId)`;
- `StatusStacks(entity, statusId)`.

The extension must be installed before compiling or loading a ScriptBundle so
its functions participate in the stable Host API hash.

Ability calls are collected inside an explicit scope. The host commits the scope
only after a script callback succeeds, or discards it after failure. Committing
creates normal deterministic `AbilityCommand` objects for the next selected
Tick; scripts never mutate health, projectile or status state directly.

Per-team command sequences are versioned, hashable and serializable through the
extension state API.

## Validation

The closure test set covers:

- canonical content encode/decode, cooked wrapping and application;
- ability commands through lockstep plus reconnect restoration;
- Fog-safe projectile, event, telegraph and status projection;
- RealScript Host API registration, scoped ability submission, status queries
  and extension-state persistence.

Existing G3, combat, simulation persistence and RS2 script tests remain the
compatibility boundary.
