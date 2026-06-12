# Phase 5 — Kinematic bodies + kinematic bones (animation drives physics)

**Status:** NOT STARTED

## Goal

Add the **Kinematic** motion type — a body the simulation does not push but *pushes back from*, moved
explicitly each step via `BodyInterface::MoveKinematic` so its swept motion imparts correct contact
velocity to the dynamic bodies it hits — and the **kinematic-bone mode**: per-bone kinematic bodies that
**follow the animated pose** each step so a moving character's limbs shove the world around. This is the
README's **binding mode (b)** — animation → physics, one-way — and the first phase that touches the
skeleton at all. The headline result: **a falling box bounces off a walking character's animated arm**,
because the arm's per-bone kinematic body sweeps through the box's space and `MoveKinematic` hands Jolt
the velocity to resolve the contact.

Scope is deliberately narrow:

- Kinematic motion for **free bodies** (a `RigidbodyComponent` whose `motionType == Kinematic`): the
  authored/scripted transform drives the body via `MoveKinematic`, the body never integrates under
  gravity or contacts, but dynamic bodies collide off it correctly.
- The **kinematic-bone mode**: a rig opts in, each driven bone gets a kinematic body auto-fit from a
  capsule, and **each step** — *after* `tickAnimation` has written the pose, *before* the Jolt
  `Update` — we read each bone's world transform and `MoveKinematic` the matching body to track it.
- Establishing the exact **ordering seam** inside `simTick` so the read happens at the one correct point.

**Out of this phase** (do not bleed in): no pose **write-back** (physics → animation is binding mode (c),
deferred to the ragdoll phases 8–9); the `PoseBuffer.override_`/`weight` blend layer stays untouched here
— this is one-way, animation wins, physics only *reads* the skeleton. No `CharacterVirtual` (phase 6). No
constraints between bone bodies (that is the ragdoll's joint graph, phases 8–9) — the bone bodies are
**independent kinematic colliders** that happen to follow the pose, nothing links them yet.

## What exists to build on

- **The step seam already exists and is the read point.** `tickPlay` (`scene_edit_play.cpp:180`) clamps
  `dt` to `PlayMaxDelta` (`:196`) and invokes `ctx.simTick(activeScene(ctx), dt)` (`:200-202`). The Host
  composes that closure (today only scripts: `host.cppm:719-732`). Phases 1–4 of this plan have already
  rewritten it so physics steps *inside* `simTick` against a fixed `PlayFixedStep` accumulator.
- **Animation runs strictly before the sim seam, every frame.** In the host `onUpdate`
  (`host.cppm:860`), `se::tickAnimation(state->animation, activeScene, dt, animMode)` (`host.cppm:887`)
  runs, *then* `se::tickPlay(*state->editor, dt)` (`host.cppm:890`). The comment at `host.cppm:882-884`
  is explicit: animation runs "before scripts so a script can still override a bone the same frame." So
  by the time `simTick` (and thus physics) runs, **the pose for this frame is already written** — the
  bone bodies can read a fresh pose. This is the load-bearing ordering for the whole phase.
- **`tickAnimation` writes `PoseOverrideComponent`, not the bone transforms.** Its contract
  (`animation.cppm:117-123`): "writing a PoseOverrideComponent onto each driven bone … Never writes a
  bone's TransformComponent." `PoseOverrideComponent` (`scene.cppm:122-127` — `translation`/`rotation`/
  `scale`) is runtime-only, never serialized.
- **The composed bone world transform is what physics reads.** `localMatrix` (`scene.cppm:712-720`)
  prefers `PoseOverrideComponent` over `TransformComponent` when present, so it returns the *animated*
  local. `updateWorldTransforms` (`scene.cppm:774`) walks roots-first composing world matrices into
  `WorldTransformComponent`, and `worldMatrix`/`worldTranslation`/`worldRotation`
  (`scene.cppm:743-767`) read that cache (composing on a miss). **Critical gotcha:** `updateWorldTransforms`
  runs in the host `onUi` pass inside `renderScene` (the same pass that calls `jointMatrices`,
  `scene.cppm:811`), which is *after* `onUpdate` — so during `simTick` the cached
  `WorldTransformComponent` is **one frame stale**. The bone read in this phase must therefore compose
  the world matrix itself (`composeWorldMatrix`, `scene.cppm:724`) from the fresh `PoseOverrideComponent`,
  not trust the cache. See §4.
- **The skeleton is addressable.** `SkinnedMeshComponent` (`scene.cppm:78-85`) holds `bones` (ordered
  joint uuids), `inverseBind`, and the runtime `boneHandles` cache (resolved by `relinkHierarchy`,
  `scene.cppm:690-706`). `BoneComponent` (`scene.cppm:67-72`) tags each joint entity. So "for each driven
  bone, get its world transform" is `worldMatrix(scene, Entity{ skin.boneHandles[i] })` once composition
  is fresh.
- **Per-bone collider metadata is already reserved and serialized.** `BonePhysics` (`scene.cppm:153-168`
  — `shapeHalfExtents`, `mass`, `joint`, `swingTwistLimits`, `drive*`) and `BonePhysicsComponent`
  (`scene.cppm:172-175` — `std::vector<BonePhysics> bones`, parallel to `SkinnedMeshComponent.bones`)
  exist, serialize through the component path, and are inert. **This phase consumes `shapeHalfExtents`**
  as the per-bone capsule/box collider size (auto-fit on add per §2); the `joint`/limit/drive fields stay
  unused until the ragdoll phases.
- **The play duplicate owns the world's lifetime.** `enterPlay` (`scene_edit_play.cpp:75`) duplicates the
  scene into `playScene`; `stopPlay` (`:143`) discards it. The `PhysicsWorld` built on the Edit→Playing
  edge of `onPlayStateChanged` (`scene_edit_context.cppm:220`) — set up in this plan's earlier phases —
  dies with that duplicate on stop; bone bodies are part of it, so no manual teardown of skeleton bodies
  is needed.
- **Jolt facts:** `BodyInterface::MoveKinematic(BodyID, RVec3 targetPos, Quat targetRot, float dt)`
  computes the linear+angular velocity that moves the body to the target over `dt` and integrates it as a
  kinematic sweep — the correct API (never teleport via `SetPosition`, which gives a contact velocity of
  zero and the box would not bounce). `EMotionType::Kinematic` set in `BodyCreationSettings`.

## Work

### 1. Free kinematic bodies via `MoveKinematic`

The `RigidbodyComponent` (added in this plan's phase 1, living in `Saffron.Scene`) already carries the
motion-type enum. Honor `Kinematic` end to end:

- **Body creation** (`Saffron.Physics` impl unit): when the entity's `RigidbodyComponent.motionType ==
  Kinematic`, set `bodySettings.mMotionType = JPH::EMotionType::Kinematic` and use the standard
  *moving* object layer (it must collide against both static and dynamic — it is a mover). Mass settings
  are ignored by Jolt for kinematic bodies; leave `mGravityFactor` irrelevant.
- **Per-step drive** for free kinematic bodies whose transform changed (scripted or animated at the
  entity level): read the entity world transform and call `MoveKinematic`. Add to the `PhysicsWorld`
  surface:

```cpp
namespace se
{
    /// Drive every Kinematic body toward its entity's current world transform over `dt`,
    /// using BodyInterface::MoveKinematic so the swept motion imparts contact velocity to the
    /// dynamic bodies it touches (never SetPosition — a teleport gives zero contact velocity).
    /// Called once per fixed step, after the pose/scene is current, before stepPhysics.
    void driveKinematicBodies(PhysicsWorld& world, Scene& scene, f32 dt);
}
```

The internal loop resolves each kinematic entity's `BodyID` from the `PhysicsWorld`'s
`entity → BodyID` map (built in phase 1), composes its world position+rotation, and calls
`bodyInterface.MoveKinematic(id, toJolt(pos), toJolt(rot), dt)`.

### 2. Opt a rig into the kinematic-bone mode + auto-fit bone capsules

Add a small component in `Saffron.Scene` (recipe: declare in `scene.cppm` + serde body in
`emitSceneSerde()` in `tools/gen-control-dto/gen.ts` → `bun run tools/gen-control-dto/gen.ts` + register
once in `scene_edit_components.cpp` `registerBuiltinComponents`):

```cpp
namespace se
{
    // Opt a SkinnedMesh rig into kinematic-bone physics: each listed joint gets a kinematic
    // body that follows the animated pose each step, so the moving character shoves the world.
    // One-way (animation -> physics); no pose write-back (that is the ragdoll). Collider sizes
    // come from BonePhysicsComponent.bones[i].shapeHalfExtents, auto-fit on add (editable after).
    struct KinematicBonesComponent
    {
        bool enabled = true;
        std::vector<i32> driven;  // indices into SkinnedMeshComponent.bones; empty = every joint
    };
}
```

**Auto-fit on add** (the locked decision §3 of the README): when a `KinematicBonesComponent` is added to a
rig, fit a capsule per driven bone into `BonePhysicsComponent.bones[i].shapeHalfExtents`. v1 fit: capsule
radius ≈ a fraction of the bone's rest length to its child joint (or a small default for leaf joints),
half-height ≈ that bone length. This happens in the add-component path (a control hook in `Saffron.Control`
when `KinematicBonesComponent` is the added type, mirroring how the auto-fit collider hook from this plan's
collider phase works) so the metadata is filled the moment the component appears, then hand-editable in the
inspector via the generic field renderer.

### 3. Build the bone bodies when physics starts

Inside `PhysicsWorld` construction-from-scene (the Edit→Playing build, phase 1), after walking
`RigidbodyComponent`/`ColliderComponent` entities, walk rigs:

```cpp
namespace se
{
    /// For each rig with an enabled KinematicBonesComponent, create one Kinematic body per
    /// driven joint (capsule from BonePhysics.shapeHalfExtents, RotatedTranslated to the bone's
    /// local collider frame), registered in the moving object layer, and record
    /// (jointEntityUuid -> BodyID) so the per-step follow can address them. The bodies are
    /// independent colliders — no constraints link them (that is the ragdoll phase).
    void buildBoneBodies(PhysicsWorld& world, Scene& scene);
}
```

Shapes are built `CapsuleShapeSettings → Create()` wrapped in a `RotatedTranslatedShapeSettings` for the
per-bone local offset (capsules are Y-up in Jolt; bones are not), exactly as the collider phase cooks free
shapes. Bone bodies join the same `entity → BodyID` map keyed by the **joint entity uuid** so they tear
down with the world on stop.

### 4. The follow step — the ordering seam (the heart of this phase)

Each fixed step, between "pose is current" and "Jolt integrates," push every bone body to its bone's world
transform:

```cpp
namespace se
{
    /// Move every kinematic bone body toward its joint's current animated world transform over
    /// `dt` via MoveKinematic, so the swept limb imparts contact velocity. Composes the bone
    /// world matrix from the fresh PoseOverrideComponent (composeWorldMatrix), NOT the cached
    /// WorldTransformComponent, which is one frame stale during simTick (renderScene refreshes
    /// it later, in onUi). One-way: reads the skeleton, never writes it.
    void followBoneBodies(PhysicsWorld& world, Scene& scene, f32 dt);
}
```

Internally, for each driven joint `i`: `Entity bone{ skin.boneHandles[i] }`; `glm::mat4 world =
composeWorldMatrix(scene, bone)` (`scene.cppm:724` — composes from `localMatrix`, which prefers the
`PoseOverrideComponent` `tickAnimation` just wrote); decompose to position+rotation (reuse
`worldTranslation`/`worldRotation`, `scene.cppm:752-767`, or decompose `world` directly); then
`bodyInterface.MoveKinematic(boneBodyId, toJolt(pos), toJolt(rot), dt)`.

**The composed step order** (the Host wires this into `simTick`, the phase-1 composition extended):

```cpp
state->editor->simTick = [state](se::Scene& scene, se::f32 dt)
{
    // 1. tickAnimation has ALREADY run this frame (host.cppm:887) before tickPlay -> simTick.
    //    The PoseOverrideComponents on driven bones are current. We do NOT call it here.
    se::followBoneBodies(state->physics, scene, dt);     // pose -> kinematic bone bodies (this phase)
    se::driveKinematicBodies(state->physics, scene, dt); // free kinematic bodies -> their entities
    se::stepPhysics(state->physics, scene, dt);          // PhysicsSystem::Update + dynamic write-back (earlier phase)
    if (state->scriptVmActive) { /* tickScripts ... (host.cppm:725) */ }
};
```

Note the read happens **per fixed step** inside the physics accumulator, with the same `dt` passed to
`MoveKinematic` and `Update`, so the swept velocity Jolt derives matches the integration step exactly. The
pose is sampled once per *frame* by `tickAnimation`; if the accumulator runs multiple fixed steps in a
frame, the bone target is the same each sub-step (the pose does not re-advance mid-frame), which is correct
for a kinematic follow — the body converges to the held target.

### 5. A control command to drive/inspect it

Per the keep-scriptable rule, add a `set-kinematic-bones` command in `Saffron.Control`
(`control_commands_scene.cpp` sibling) — `{ entity, enabled }` toggling
`KinematicBonesComponent.enabled` on the selected/named rig and bumping `sceneVersion`. The generic
add-component / set-component-field commands already cover the `driven` list and the auto-fit
`shapeHalfExtents`, so this command only needs the enable toggle (the one bit of physics-specific runtime
state). Add the matching `se` verb.

## Validation (done criteria)

- `make engine` green; `make prepare-for-commit` (clang-format + clang-tidy) clean for the new files.
- `bun run check` clean (the regenerated `@saffron/protocol` picks up `KinematicBonesComponent` and the
  new command DTO from `control_dto.cppm` / the scene serde).
- Manual: a rigged character playing a walk/idle clip with a `KinematicBonesComponent` + a dynamic box
  dropped at arm height — **the box is knocked away by the swinging arm**, and a free `Kinematic` platform
  driven by a script pushes dynamic boxes that rest on it (the contact-velocity proof). A teleported
  (`SetPosition`) body would *not* do either — that is the regression this phase guards against.
- `make e2e`: extend the physics suite (sibling to `rig-query.test.ts` / `rig-preview.test.ts`) —
  import the rig fixture, add `KinematicBonesComponent`, enter play, step several fixed ticks with a
  dynamic body positioned in the limb's swept path, and assert via the body-query command (from the
  raycast/query phase) that the dynamic body's position **changed** (it was hit) and the bone bodies' own
  transforms tracked the joints. Assert a validation-clean log.
- `docs/`: update `docs/content/explanations/physics/_index.md` hub with a "kinematic bodies & bone
  following" row, and add `docs/content/explanations/physics/kinematic-bones.md` — the three binding modes
  (a static / b animation→physics / c physics→animation), why `MoveKinematic` not teleport, the
  `tickAnimation`-before-`simTick` ordering, and that write-back (mode c) is the ragdoll phase.

## Notes / gotchas

- **`MoveKinematic`, never `SetPosition`.** A teleport leaves the body's velocity at zero, so a dynamic
  body it overlaps is resolved as a static penetration push with no momentum — the box would *ooze* off
  the arm, not get *hit*. `MoveKinematic` derives the velocity from `(target − current)/dt`, which is the
  whole point of this phase. State this in the docs page.
- **The world cache is stale inside `simTick` — compose, do not read the cache.** `updateWorldTransforms`
  runs in `renderScene` (host `onUi`), *after* `onUpdate`/`tickPlay`. So `worldMatrix` reading
  `WorldTransformComponent` (`scene.cppm:743-748`) returns last frame's pose during the step. Use
  `composeWorldMatrix` (`scene.cppm:724`), which recomputes from `localMatrix` (and thus the fresh
  `PoseOverrideComponent`). This is the single most likely source of a one-frame lag bug.
- **One-way only — keep the blend layer untouched.** This phase must not write `PoseOverrideComponent`,
  `PoseBuffer.override_`, or `weight`. Animation is authoritative for the skeleton; physics reads it.
  Resisting the urge to "just nudge the bone back from a hard contact" here is what keeps mode (b) clean
  and leaves mode (c) — the ragdoll's `DriveToPoseUsingMotors` / `GetPose` write-back — a separate, later
  build (phases 8–9) that plugs into the *reserved* `override_`/`weight` seam, not this code path.
- **Bone bodies are independent colliders, not a ragdoll.** No `FixedConstraint`/`SwingTwistConstraint`
  links them, and they read `shapeHalfExtents` but ignore `BonePhysics.joint`/`swingTwistLimits`/`drive*`.
  Those fields stay reserved for the ragdoll. A rig with a `KinematicBonesComponent` is a *moving
  collision proxy*, which is exactly what "the world reacts to a walking character" needs and all it needs.
- **Leaf joints have no child to measure length against** for the auto-fit; fall back to a small fixed
  capsule (e.g. the parent bone's radius, a short half-height) so fingers/toes still get a body rather than
  a degenerate zero-extent shape (Jolt rejects a zero-radius capsule).
- **Determinism holds.** `MoveKinematic` + the fixed-step accumulator (phase 1, `PlayFixedStep`) keep the
  follow bit-exact across machines under the `CROSS_PLATFORM_DETERMINISTIC` build — the bone target is a
  pure function of the (deterministic) sampled pose, and the same `dt` feeds both `MoveKinematic` and
  `Update`.
