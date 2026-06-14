# Phase 6 — Character controller (Jolt `CharacterVirtual`)

**Status:** COMPLETED

## Goal

Give the engine a **walking capsule character**: a `CharacterControllerComponent` (capsule dimensions +
movement params) driven by a Jolt `CharacterVirtual`, stepped inside the existing `simTick` seam against
the physics world Phases 1–5 stood up, and a **`move-character`** control command (+ `se` verb) that feeds
it a desired horizontal velocity each frame. The capsule walks across the floor scene from Phase 2,
**steps over** small obstacles and **slides** along walls, and writes its resolved position back into the
entity-root `TransformComponent` so the visible mesh follows. This is **binding mode A**: the capsule *is*
the character's collision proxy; any `AnimationPlayerComponent` on the same entity (or a child) plays
**independently on top** — the controller only positions the root, it does not read or drive the pose.

Scope is exactly: the component, the `CharacterVirtual` integration in `simTick`, and the move command +
verb. **Out of scope** (and must not bleed in): animation coupling beyond positioning the root (no
root-motion extraction, no locomotion blend, no foot IK against the controller — those are the animation
plan's blend-layer producers, Phase 13 there); gameplay input mapping (the command *is* the input seam for
now); and ragdoll (Phases 8–9 here — a `CharacterVirtual` is a kinematic proxy, not a `Ragdoll`).

## What exists to build on

- **The physics world + fixed-step seam (Phases 1–5).** `PhysicsWorld` (Jolt `PhysicsSystem` +
  `BodyInterface` + `TempAllocator` + `JobSystem` + the `Entity`↔`BodyID` maps) is owned by `HostState` in
  `host.cppm:48` (a sibling of `se::AnimationRuntime animation` `host.cppm:53` and `se::ScriptHost script`
  `:54`), built on the `Edit → Playing` edge of `onPlayStateChanged` and torn down on `→ Edit`, mirroring
  the script VM lifecycle subscription at `host.cppm:694-718`. It is **stepped inside `simTick`**
  (`host.cppm:719`), which `tickPlay` invokes with a dt already clamped to `PlayMaxDelta`
  (`scene_edit_play.cpp:196`); the physics world runs its own fixed `PlayFixedStep` accumulator
  (`scene_edit_context.cppm:172`) — the same `1/60` deterministic tick. A `CharacterVirtual` is updated
  *inside that same accumulated step*, so this phase adds no new seam, only a consumer of the existing one.
- **The play-scene duplicate is the controller's lifetime.** `enterPlay` duplicates the authored scene into
  `editor->playScene` via `sceneToJson`/`sceneFromJson` and `stopPlay` discards it (`scene_edit_play.cpp`,
  `scene_edit_context.cppm:215` — `std::optional<Scene> playScene`); the `CharacterVirtual` is created
  against bodies in that duplicate and dies with it on stop. No manual reset of the authored capsule pose.
- **The capsule shape already exists.** The Phase 2/3 `ColliderComponent` carries
  `Shape::Capsule` + dimensions and auto-fits to the entity mesh AABB on add; the controller **reuses that
  same capsule** (radius + half-height) rather than introducing a second capsule field. A character entity
  is therefore a `TransformComponent` + a `ColliderComponent` (capsule) + a `CharacterControllerComponent`
  — and notably **no `RigidbodyComponent`** (a `CharacterVirtual` is not a rigid body; it is the
  recommended kinematic-sweep controller, Jolt `CharacterVirtual`/`CharacterVirtualSettings`).
- **The root transform the capsule drives.** `TransformComponent` (`scene.cppm:41` — `translation`,
  `scale`, `rotation` Euler XYZ radians) is the per-entity local transform; `updateWorldTransforms`
  (`scene.cppm`, walks roots-first each frame) composes `WorldTransformComponent` (`scene.cppm:60`) from it
  before `renderScene` (host `onUi`). Writing the capsule's resolved world position back into the root
  `TransformComponent` *inside `simTick`* (before `renderScene`) makes the mesh follow the same frame — the
  identical pattern animation uses, which runs `tickAnimation` (`host.cppm:887`) before `tickPlay`
  (`host.cppm:890`).
- **The per-entity-action command pattern.** `set-foot-ik` (`control_commands_animation.cpp:381`) is the
  exact precedent: `registerCommand<SetFootIkParams, FootIkResult>`, a helper that `resolveEntity`s the
  `EntitySelector` and fetches/creates the component on `activeScene(ctx.sceneEdit)`, and a
  `…Version += 1` bump. `SetFootIkParams` (`control_dto.cppm:1379` — `EntitySelector entity` +
  `std::optional<…>` fields) and `Vec3` (`control_dto.cppm:42`) are the DTO shapes to copy. Read-back of a
  world position uses `WorldTransformResult` (`control_dto.cppm:1400`) / `get-world-transform`, which the
  e2e test asserts against.
- **The `se` verb path.** `tools/se/source/main.cpp` coerces `--key value` / `--key=value` into
  `params[key]` (`buildParams`, `:87-124`); no per-command code is needed for a new verb beyond an optional
  text formatter branch in `printResult` (`:127`). The Phase 5 e2e sibling tests
  (`tests/e2e/rig-preview.test.ts` / `rig-query.test.ts`, and `foot-ik.test.ts`) are the harness templates.

## Work

### 1. `CharacterControllerComponent` on `Saffron.Scene`

Dumb data — like every other component, it carries no Jolt type (Jolt is consumed only in the
`Saffron.Physics` impl TU). Add to `scene.cppm` near `ColliderComponent` (the recipe: declare the struct +
its `*ToJson`/`*FromJson` forward decls; edit the serde body in `emitSceneSerde()` in
`tools/gen-control-dto/gen.ts` and regenerate; register once in
`scene_edit_components.cpp::registerBuiltinComponents`).

```cpp
/// Marks an entity as a walking capsule character driven by a Jolt CharacterVirtual.
/// The capsule itself is the entity's ColliderComponent (Shape::Capsule) — this carries only
/// the movement parameters. The controller drives the entity-root TransformComponent each
/// physics step; any AnimationPlayerComponent plays independently on top (binding mode A).
struct CharacterControllerComponent
{
    f32 maxSpeed = 4.0f;             // horizontal walk speed cap, m/s — the move-character target is clamped to it
    f32 maxSlopeAngle = 0.785398f;   // radians (~45°); steeper ground is treated as a wall, not floor
    f32 maxStepHeight = 0.3f;        // ledges/stairs up to this are stepped over (Jolt WalkStairs sweep)
    f32 gravityFactor = 1.0f;        // scales the world gravity applied to the character each step
    // Runtime state (serialize as zero): the desired horizontal velocity move-character writes, and the
    // vertical velocity the controller integrates for falling/stepping. Reset to 0 on enterPlay.
    glm::vec3 desiredVelocity{ 0.0f };
    f32 verticalVelocity = 0.0f;
    bool onGround = false;           // last step's ground state — read by the move command's result + se/UI
};
```

The runtime fields follow the `AnimationPlayerComponent` precedent (`scene.cppm:91` — `time`/`pingForward`
are "runtime state, serialize as …"): they exist on the dumb struct so the controller is self-contained,
but the serde writes their authoring defaults so they never pollute the saved scene. Bump `SceneVersion`
and add a `sceneFromJson` migration branch only if a default here changes an *existing* component's
on-disk shape — a brand-new component does not require it (extend `runSceneSerializationSelfTest`
regardless, per the scene `AGENTS.md`).

### 2. `CharacterController` in `Saffron.Physics` (wraps `CharacterVirtual`)

A POD handle/types in `Saffron.Physics:Types`, the Jolt object created only in the impl TU. The
`PhysicsWorld` gains an `Entity → CharacterVirtual` map alongside its `Entity → BodyID` map (a
`CharacterVirtual` is a sweep object, not a body, so it lives in its own table):

```cpp
namespace se
{
    /// Per-character runtime owned by PhysicsWorld; one per entity with a CharacterControllerComponent.
    /// Opaque to every consumer — the Jolt CharacterVirtual + its settings live in the impl unit only.
    struct CharacterController;  // defined in physics.cpp; held by PhysicsWorld as Ref<CharacterController>

    /// Create a CharacterVirtual for `entity` from its capsule ColliderComponent + CharacterControllerComponent.
    /// Capsule radius/half-height come from the collider's auto-fit dimensions; the rest from the controller.
    auto addCharacter(PhysicsWorld& world, Entity entity, const Scene& scene) -> Result<void>;

    /// Advance every character one fixed step: apply gravity, fold in desiredVelocity, run the Jolt
    /// CharacterVirtual::ExtendedUpdate (stick-to-floor + WalkStairs), and resolve collisions/sliding.
    void stepCharacters(PhysicsWorld& world, f32 fixedDt);

    /// Read each character's resolved world position back so the Host can write the entity-root transform.
    auto characterPosition(const PhysicsWorld& world, Entity entity) -> std::optional<glm::vec3>;
}
```

Implementation notes (the Jolt facts to honour):
- Build the capsule via `CapsuleShapeSettings(halfHeight, radius).Create()` → `RefConst<Shape>`, matching
  the collider's auto-fit dims; offset to the capsule's standing origin with `RotatedTranslatedShape`
  if the collider local offset is non-zero.
- `CharacterVirtualSettings`: `mShape`, `mMaxSlopeAngle = maxSlopeAngle`, supporting volume / up = world up.
  Create the `CharacterVirtual` against the `PhysicsSystem` from Phase 1.
- Per step: `verticalVelocity += gravity * gravityFactor * fixedDt` (zeroed when grounded), build a desired
  velocity `vec3(desiredVelocity.x, verticalVelocity, desiredVelocity.z)` clamped to `maxSpeed`
  horizontally, then `ExtendedUpdate(fixedDt, gravity, settings{maxStepHeight as WalkStairs step up}, …)`
  using the world's `TempAllocator`. Read `GetGroundState()` into `onGround` and `GetPosition()` for
  write-back. Collision/wall sliding is what `CharacterVirtual` does natively — no manual resolution.
- Filtering reuses the Phase 4 `ObjectLayer`/`BroadPhaseLayer` interfaces; the character collides against
  the same layers the static floor/walls live on.

### 3. Step characters inside `simTick`, write the root transform back

The Host composes physics into the existing `simTick` lambda (`host.cppm:719`), which already runs inside
`tickPlay`'s clamped dt. Within the world's fixed-step accumulator loop (the one that calls
`PhysicsSystem::Update`), call `stepCharacters(world, PlayFixedStep)` **before** the rigidbody readback so
character and dynamic bodies resolve against the same sub-step. After the accumulator drains, for each
entity with a `CharacterControllerComponent`, write `characterPosition` into the entity-root
`TransformComponent.translation` on `activeScene` (the play duplicate). This lands **before** `renderScene`
in host `onUi`, so the mesh follows the same frame — exactly as the rigidbody write-back does and as
animation does ahead of it.

> Ordering against scripts: `tickAnimation` (`host.cppm:887`) runs before `tickPlay` (`:890`), and scripts
> run inside `simTick`. Physics steps inside `simTick` too. Run **physics after the script tick** in the
> `simTick` composition so a script issuing a `move-character`-equivalent set (or writing `desiredVelocity`)
> is consumed by the same frame's physics step — the mirror of "animation before scripts so a script can
> still override a bone."

### 4. `move-character` control command + DTO + `se` verb

DTO (copy the `SetFootIkParams` shape, `control_dto.cppm:1379`):

```cpp
struct MoveCharacterParams
{
    EntitySelector entity;
    Vec3 velocity;                 // desired world-space horizontal velocity (m/s); y is ignored — gravity owns vertical
    std::optional<bool> jump;      // optional impulse: set verticalVelocity to the jump speed this step
};

struct MoveCharacterResult
{
    Vec3 position;                 // resolved world position after the write-back
    bool onGround;                 // GetGroundState() from the last step
};
```

Register in `control_commands_scene.cpp` (the character is scene state; physics is a Host-owned consumer of
it), following `set-foot-ik` (`control_commands_animation.cpp:381`):

```cpp
registerCommand<MoveCharacterParams, MoveCharacterResult>(
    reg, "move-character",
    "move-character {entity, velocity:{x,y,z}, jump?} — set a character's desired walk velocity",
    [](EngineContext& ctx, const MoveCharacterParams& params) -> Result<MoveCharacterResult>
    {
        auto entity = resolveEntity(ctx, params.entity);
        if (!entity)
        {
            return Err(entity.error());
        }
        Scene& scene = activeScene(ctx.sceneEdit);
        if (!hasComponent<CharacterControllerComponent>(scene, *entity))
        {
            return Err(std::string{ "entity has no CharacterController" });
        }
        CharacterControllerComponent& c = getComponent<CharacterControllerComponent>(scene, *entity);
        c.desiredVelocity = glm::vec3(params.velocity.x, 0.0f, params.velocity.z);
        // jump applies on the next physics step inside simTick; the result reads the *current* state.
        // …set a jump flag the controller consumes; bump the version stamp like set-foot-ik…
        return MoveCharacterResult{ /* current position */, c.onGround };
    });
```

`move-character` only writes the desired velocity onto the component (the inert seam); the actual sweep
happens in the next `simTick` physics step, identical to how `set-foot-ik` only flips component fields and
the evaluator consumes them next frame. Add the DTO to `gen.ts` (with a fixture or skip reason for the
contract test) and commit all five regenerated outputs. The `se` verb is then
`se move-character <entity> --velocity '{"x":2,"y":0,"z":0}'` with no extra CLI code (positional entity +
`--velocity` JSON literal coerce through `buildParams`); add a `printResult` branch only if a prettier text
line is wanted.

## Validation (done criteria)

- `make engine` green; the new module TU and the `CharacterControllerComponent` serde compile, and
  `make e2e`'s headless boot logs **zero validation errors**.
- `make prepare-for-commit` clean (clang-format + clang-tidy) across the new/changed files.
- `bun run check` regenerates `@saffron/protocol` (the new `MoveCharacterParams`/`MoveCharacterResult` reach
  `se-types.ts`) and typechecks; `bun run tools/gen-control-dto/gen.ts` outputs are committed.
- `make e2e`: a new `tests/e2e/character-controller.test.ts` (mirroring `foot-ik.test.ts` /
  `rig-query.test.ts`) that imports the floor fixture + a capsule character, enters Play, calls
  `move-character` with a +X velocity over several `settle`d frames, and asserts via `get-world-transform`
  that the character's **world X advanced** while its **world Y stayed on the floor** (it walked, did not
  sink or fly); a second case raises a small step in front and asserts the character **stepped up** (world
  Y rose by ≤ `maxStepHeight`) rather than stopping; a third asserts `onGround` is true while walking.
- `docs/`: add `docs/content/explanations/physics/character-controller.md` — the `CharacterVirtual` kinematic-
  sweep model, binding mode A (capsule positions the root, animation plays on top), the `move-character`
  seam, and how it differs from a dynamic rigidbody. Add the hub `_index.md` row.

## Notes / gotchas

- **`CharacterVirtual`, not `Character` or a kinematic rigidbody.** Jolt's `CharacterVirtual` is the
  recommended controller: it sweeps the shape and resolves penetration/sliding without being a simulated
  body, which is precisely the "walks and steps over the floor, slides along walls" behaviour wanted here.
  A `Character` (the body-backed variant) and the Phase 5 kinematic-rigidbody path are deliberately *not*
  reused — a character is its own thing. State this in the docs so no one later "unifies" them.
- **No `RigidbodyComponent` on a character.** Per the locked split, a `ColliderComponent` without a
  `RigidbodyComponent` is an implicit static body — but a `CharacterControllerComponent` overrides that:
  the capsule is owned by the `CharacterVirtual`, not registered as a static body. The Phase 4 world-build
  must skip making a static body for a collider that also has a `CharacterControllerComponent`.
- **Binding mode A means the controller never touches the pose.** It writes only the entity-root
  `TransformComponent.translation`; rotation/animation are independent. Resist adding root-motion or
  locomotion blend here — that coupling is binding mode B and belongs to the animation plan's blend-layer
  producer model (the `override_`/`weight` seam), not to the physics controller. Keep this phase's surface
  to *positioning*.
- **The write-back is local-space.** The character entity is expected to be a scene root (its
  `TransformComponent` local == world). If a character is parented, decompose the resolved world position
  into the parent frame before writing — but v1 assumes a root character and may assert/log if parented,
  rather than silently mis-placing it. Say so in the docs.
- **Determinism holds.** The character steps at `PlayFixedStep` inside the same accumulator as the rest of
  the world, so the Phase 1 `CROSS_PLATFORM_DETERMINISTIC` guarantee extends to it — provided
  `move-character` velocities are applied on a fixed boundary (they are: the command writes the component;
  the step reads it), not interpolated per render frame.
- The capsule auto-fit comes from the Phase 2/3 `ColliderComponent`; do not add a second radius/half-height
  on `CharacterControllerComponent`. One source of truth for the capsule, editable in the inspector, per
  the locked auto-fit decision.
