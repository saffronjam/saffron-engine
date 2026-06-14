# Phase 8 — Ragdoll built from the reserved `BonePhysicsComponent` (physics drives animation)

**Status:** COMPLETED

## Goal

Build a Jolt `Ragdoll` + `Skeleton` from the **already-serialized, already-inert** `BonePhysicsComponent`
(`scene.cppm:172`) — its per-bone shapes, masses, and joint types — simulate it inside the existing
fixed-step `simTick` seam, and write each step's body transforms **back** into the rig's pose so the
skinning prepass renders the *simulated* skeleton. This is the inversion of phases 1–7: there a rigidbody
read the scene's transform and moved a body; here the **body drives the bone**. A clip-driven character is
told to go limp, the animation stops contributing, and it collapses to the floor under gravity and its
joint limits — landing through the **same** `PoseBuffer` blend-layer seam the foot-IK solver already uses
(`animation.cpp:702-709`), the seam the README's "one load-bearing decision" reserved for exactly this.

This is **binding mode c (pure ragdoll)**: `weight[i] = 1` on every ragdoll-driven bone, so the body
transform wins outright and the clip is ignored. Motor-driven blending (the limp-to-active spectrum, UE's
Physics Blend Weight) is Phase 9 — here the take-over is total, with one subtlety: the bodies start with
the rig's current per-bone **velocities** (finite-differenced from `lastPose`) so the switch from animation
to physics does not pop.

## What exists to build on

- **The ragdoll schema is already in the scene, serialized, and inert.** `BonePhysics` (`scene.cppm:153`)
  carries `glm::vec3 shapeHalfExtents` (`:155`), `f32 mass` (`:156`), an `enum class Joint : u8 { Fixed,
  Hinge, SwingTwist, Free }` (`:157-163`, default `SwingTwist`), `glm::vec3 swingTwistLimits` in radians
  (`:164`), and the PD gains `driveStiffness`/`driveDamping`/`driveMaxForce` (`:165-167`).
  `BonePhysicsComponent { std::vector<BonePhysics> bones; }` (`scene.cppm:172`) is a **parallel array to
  `SkinnedMeshComponent.bones`** (`scene.cppm:82`) — index `i` of one is the same joint as index `i` of the
  other. The struct comment is explicit: *"the reserved physics metadata above … Serialized through the
  component path; inert."* Phase 13 of the animation plan authored and registered it; this phase is its
  **first consumer**. The `Joint`/`swingTwistLimits`/`drive*` fields map 1:1 onto Jolt constraints (§2).
- **The pose write-back seam is `PoseOverrideComponent`** (`scene.cppm:122`), the runtime-only per-bone
  local TRS that `localMatrix` (`scene.cppm:712-720`) prefers over the authored `TransformComponent`, so
  the rest pose stays pristine and the play duplicate's authored data is never touched. The animation
  evaluator already writes it per joint via `emplace_or_replace<PoseOverrideComponent>` in its bone loop
  (`animation.cpp:711-726`). The ragdoll write-back is **structurally identical** — same component, same
  loop, same `boneHandles[i]` addressing — only the *source* of the TRS differs (a Jolt body, not a clip).
- **The skinning chain that consumes it is untouched.** `renderScene` (host `onUi`, `host.cppm:942`) runs
  `updateWorldTransforms` (`scene.cppm:774`) then `jointMatrices` (`scene.cppm:811`), which compose
  `worldMatrix(bones[i]) * inverseBind[i]` (`scene.cppm:826-827`) into the GPU palette. Because
  `updateWorldTransforms` reads each bone's local through `localMatrix` (`scene.cppm:781`), writing
  `PoseOverrideComponent` on a bone *is* feeding the compute-skinning prepass. **No new render pass.**
- **`AnimationRuntime.lastPose`** (`animation.cppm:114`) — `std::unordered_map<u64, std::vector<JointPose>>`,
  the previous frame's final local pose per rig — is **snapshotted every tick** at `animation.cpp:730`
  (`runtime.lastPose[key] = finalLocal;`) and described verbatim as: *"the eventual physics handoff
  finite-differences it for per-bone velocities so a ragdoll take-over does not pop. No consumer yet —
  reserved."* This phase is that consumer (§4).
- **The tick ordering is fixed and correct for this.** Per frame in the host `onUpdate` (`host.cppm:887-890`):
  `tickAnimation(...)` writes `PoseOverrideComponent`s from clips first, **then** `tickPlay(*editor, dt)`
  runs `simTick`. `tickPlay` clamps `dt` to `PlayMaxDelta` and substitutes `PlayFixedStep` for a `step`
  (`scene_edit_play.cpp:194-202`; constants `scene_edit_context.cppm:172-173`). The Phase 1 `PhysicsWorld`
  is stepped inside that `simTick` composition. So a ragdoll-driven bone's override is written *after*
  animation's for the same frame — physics wins the frame, exactly the layering this phase needs — and it
  all lands before `renderScene` reads it the same frame.
- **The lifecycle hook is `onPlayStateChanged`** (`scene_edit_context.cppm:220`, *"the physics/scripting
  lifecycle seam"*). The Phase 1 `PhysicsWorld` is built on `Edit→Playing` and torn down on `→Edit`,
  mirroring the script VM (`host.cppm:694-718`). A ragdoll lives inside that world: `AddToPhysicsSystem` on
  enable, dies with the world on stop — and because `enterPlay` duplicates the scene and `stopPlay` discards
  it (`scene_edit_play.cpp:75-104`, `:143-152`), there is **no manual reset** of the collapsed pose.
- **The blend-layer precedent is live.** `applyFootIk` (called at `animation.cpp:705-708`) is the existing
  proof that an external producer mixes into `finalLocal` before the bone write — the ragdoll is the second
  such producer, but a *replacing* one (weight 1), not a *mixing* one.
- **e2e siblings to mirror:** `tests/e2e/foot-ik.test.ts` and `tests/e2e/rig-query.test.ts` already boot a
  headless engine, import `tests/e2e/fixtures/leg.gltf`, and assert bone world positions over the control
  plane — the exact shape of the collapse assertion in §7.

## Work

### 1. Map `BonePhysicsComponent` → Jolt `RagdollSettings` (in `Saffron.Physics`, the only Jolt TU)

In the `Saffron.Physics:Types` partition, a POD handle keyed by the rig's `IdComponent` uuid (same keying
as `AnimationRuntime`'s maps), plus the spawn entry point:

```cpp
/// One live ragdoll: a Jolt Ragdoll instance whose parts mirror SkinnedMeshComponent.bones 1:1,
/// owned by the PhysicsWorld and added to its PhysicsSystem. boneToPart[i] indexes the Jolt
/// skeleton/part for bone i (a part may be absent if BonePhysics[i] has a zero shape).
struct RagdollInstance
{
    u64 rig = 0;                       // the rig mesh entity's IdComponent uuid
    std::vector<i32> boneToPart;       // SkinnedMeshComponent.bones index -> ragdoll part (-1 = no body)
    // Jolt's Ragdoll + RagdollSettings are RefConst; held in the .cpp behind this opaque handle.
};

/// Build a Jolt Ragdoll from the rig's BonePhysicsComponent + SkinnedMeshComponent and add it to
/// the world, seeding each body at the rig's current world bone transform (so it spawns *on* the
/// animated pose, not at the origin) and with the per-bone velocity from `seedVelocities` (§4).
/// No-op + Err if the rig has no BonePhysicsComponent or the parallel arrays disagree in length.
auto enableRagdoll(PhysicsWorld& world, Scene& scene, Entity rig,
                   const std::vector<JointPose>& seedVelocities) -> Result<void>;

/// Remove the rig's ragdoll from the PhysicsSystem and forget it. Idempotent.
void disableRagdoll(PhysicsWorld& world, u64 rig);
```

Inside the impl `.cpp` (the sole `<Jolt/...>` includer), construction walks the parallel arrays:

```cpp
// for bone i in [0, skin.bones.size()):
//   const BonePhysics& bp = phys.bones[i];
//   skip if bp.shapeHalfExtents == 0 (a non-simulated joint — e.g. fingers)
//   shape: a CapsuleShape sized from bp.shapeHalfExtents (radius = min extent,
//          half-height = max extent - radius), or a BoxShape when the extents are
//          balanced; wrapped in a RotatedTranslatedShape for the bone's local offset.
//   part:  RagdollSettings::Part { mShape, mPosition/mRotation = bind-pose world of bone i,
//          mMotionType = Dynamic, mMassPropertiesOverride from bp.mass }.
//   constraint to the PARENT bone's part (walk SkinnedMeshComponent up via boneHandles /
//   RelationshipComponent), chosen by bp.joint:
//     Fixed      -> FixedConstraintSettings
//     Hinge      -> HingeConstraintSettings (limits from swingTwistLimits.x)
//     SwingTwist -> SwingTwistConstraintSettings (mNormalHalfConeAngle / mPlaneHalfConeAngle
//                   from swingTwistLimits.x/.y, mTwistMinAngle/mTwistMaxAngle from .z)
//     Free       -> PointConstraintSettings (positional only, no angular limit)
// RagdollSettings::Stabilize() once after all parts/constraints are set, then ->CreateRagdoll(),
// AddToPhysicsSystem(EActivation::Activate). The drive* gains (driveStiffness/Damping/MaxForce)
// are parsed into the constraint motor settings but left *off* this phase (motors engage in Phase 9).
```

The Jolt `Skeleton` (joint hierarchy) is built from the bone parent chain so a future
`DriveToPoseUsingMotors(SkeletonPose)` (Phase 9) has the mirror it needs; here only the part bodies +
constraints are used.

### 2. Auto-fit ragdoll capsules on import (the locked auto-fit decision)

Per the locked "auto-fit shapes on add" decision, a rig imported with bones gets a **capsule per bone
auto-fitted** into `BonePhysicsComponent.bones[i].shapeHalfExtents` at spawn time, then hand-editable in
the inspector (the fields are already serialized). The fit is geometric and lives in `Saffron.Physics` (it
needs no Jolt): for bone `i`, size the capsule along the segment from bone `i`'s world position to its
child's, radius a fraction of that length — a leaf bone (no child) gets a small sphere-ish capsule. This
runs once on import (extend the rig-spawn path in `Saffron.Assets`), writing the parallel array so a
freshly imported character is ragdoll-ready without manual authoring. Do **not** add a separate "fit"
button — fit-on-import, edit-after.

### 3. Step + write-back: bodies drive `PoseOverrideComponent` (the inversion)

The `PhysicsWorld` step (Phase 1, inside `simTick`) already runs `PhysicsSystem::Update`. Immediately after
`Update`, for every `RagdollInstance` in the world, read each part's world transform and convert it back to
a **local** `JointPose` for the bone, then write the blend layer:

```cpp
/// After PhysicsSystem::Update: for each live ragdoll, read GetRagdollState / per-part
/// GetWorldTransform, convert body world transform -> bone LOCAL TRS (divide out the parent
/// bone's world matrix, exactly the inverse of jointMatrices' composition), and write
/// PoseOverrideComponent on boneHandles[i] with weight 1 (pure ragdoll). Mirrors the
/// animation evaluator's bone loop (animation.cpp:711-726) — same component, same addressing.
void writeRagdollPoses(PhysicsWorld& world, Scene& scene);
```

The local-TRS conversion is the inverse of `jointMatrices` (`scene.cppm:826-827`): a body's world matrix
`W_i`, the parent bone's world matrix `W_parent` (read via `worldMatrix`, `scene.cppm:743`), give the
bone's local `L_i = inverse(W_parent) * W_i`, decomposed into translation/quaternion/scale and emplaced as
`PoseOverrideComponent` on `skin.boneHandles[i]`. Because `localMatrix` (`scene.cppm:712`) already prefers
`PoseOverrideComponent`, the next `updateWorldTransforms`/`jointMatrices` (same frame, in `renderScene`)
renders the collapsed skeleton with **zero rendering changes**.

`writeRagdollPoses` runs from the Host's `simTick` composition **after** `tickAnimation` and the physics
`Update`, so on a ragdoll-active bone the physics override overwrites the animation override for the frame
— the clip is effectively silenced on those bones (binding mode c). Bones with no part (zero shape) keep
their animation override, so a partial ragdoll (e.g. limp upper body, scripted root) composes naturally.

### 4. Seed body velocities from `lastPose` so the take-over does not pop (consume the reserved field)

On `enableRagdoll`, each part's initial linear+angular velocity comes from finite-differencing the rig's
last two final poses — `runtime.lastPose[rig]` (`animation.cppm:114`, written at `animation.cpp:730`)
against the current frame's `finalLocal` — divided by the frame `dt`. The Host passes this in as
`seedVelocities` (computed in the `simTick` composition where both the `AnimationRuntime` and the
`PhysicsWorld` are in scope — both are `HostState` siblings, `host.cppm:53` and the Phase 1 addition):

```cpp
// In the Host simTick, on the enable-ragdoll edge for rig `e`:
//   const auto& prev = state->animation.lastPose[rigUuid];   // last frame's final local pose
//   const auto& curr = /* this frame's finalLocal, re-derivable from the rest+clip */;
//   per bone: linearVel  = (worldPos_curr - worldPos_prev) / dt;
//             angularVel = quatToAngularVelocity(rot_prev, rot_curr, dt);
//   se::enableRagdoll(*state->physics, scene, e, velocities);
```

Without this seed a character mid-stride snaps to zero velocity and visibly jolts; with it the limbs carry
their animated momentum into the simulation. This is the gameplay analogue of the rendering motion-vector
seed (animation Phase 8) and the whole reason `lastPose` was reserved.

### 5. The enable/disable trigger + control command (keep-scriptable rule)

Ragdoll is enabled imperatively (a "go limp" event) — there is no per-frame component flag deciding it;
enabling is a state transition. Add one control command, mirroring the existing `set-rigidbody` /
`raycast` physics commands from earlier phases and the `registerCommand<Params,Result>` pattern:

- **`enable-ragdoll {entity} {enabled:bool}`** — on `true`, looks up the rig in the play scene, computes
  the velocity seed (§4), and calls `enableRagdoll`; on `false`, `disableRagdoll` and the bones fall back
  to their animation overrides next frame (the clip resumes driving them). Only meaningful in `Playing`
  (there is no `PhysicsWorld` in Edit). The `se` CLI gets a matching verb + formatter.

A "death-like event" (e.g. a script calling this command, or a future health system) is just a caller of
`enable-ragdoll` — no special engine path.

### 6. Module wiring

`Saffron.Physics` already exists from Phase 1 with the edge `Physics → {Core, Geometry, Scene, Animation}`
and is imported only by `Saffron.Host`. This phase adds **no new module** — it adds `RagdollInstance` to
the `:Types` partition, `enableRagdoll`/`disableRagdoll`/`writeRagdollPoses` to the impl `.cpp` (the sole
Jolt includer), and consumes `BonePhysicsComponent`/`SkinnedMeshComponent` (Scene), `JointPose`/`PoseBuffer`
(Animation), and `worldMatrix`/`PoseOverrideComponent` (Scene). The `Animation` edge — already declared in
Phase 1 *because of this phase* — is now load-bearing (the `JointPose` velocity seed type crosses it).

### 7. e2e: collapse under gravity (mirror `foot-ik.test.ts`)

`tests/e2e/ragdoll.test.ts` boots the headless engine, imports the rigged `leg.gltf` fixture, enters Play,
reads a foot/leaf bone's world Y over the control plane (`get-rig` / a transform query, as
`rig-query.test.ts` does), fires `enable-ragdoll {entity} true`, steps several frames, and asserts:
- the leaf bone's world Y **drops** (the limb falls under gravity) and then **settles** at the ground
  layer height (the floor collider from the earlier phases stops it) rather than falling through;
- with `enable-ragdoll {entity} false` re-issued, the clip resumes (the bone tracks the animation again);
- the validation log is clean (no Jolt assertion, no constraint-stabilization warning).

## Validation (done criteria)

- `make engine` green with the ragdoll mapping + write-back compiling; `make prepare-for-commit` clean
  (clang-format + clang-tidy) for the new `Saffron.Physics` code.
- `make e2e` green including the new `tests/e2e/ragdoll.test.ts` (collapse-then-settle, resume-on-disable,
  validation-clean log), and `bun run check` if the `enable-ragdoll` DTO changes the protocol.
- Manual headless run: a clip-driven `leg.gltf`, on `enable-ragdoll`, collapses realistically and rests on
  the floor; on disable, the clip resumes from the live pose without a hard snap (the velocity seed proves
  out visually as no first-frame jolt at enable time).
- `docs/`: add `docs/content/explanations/physics/ragdoll.md` — physics-drives-animation as the inversion
  of rigidbodies, the `BonePhysicsComponent`→`RagdollSettings` map (cite UE PhAT / Jolt `RagdollSettings`),
  binding mode c (`weight = 1`) and the `PoseOverrideComponent` write-back through `localMatrix`, and the
  `lastPose` velocity seed; add the row to `docs/content/explanations/physics/_index.md`.

## Notes / gotchas

- **Quaternion convention.** `glm::quat` is `(w, x, y, z)` (`scene.cppm:125`) while Jolt's `Quat` is
  `(x, y, z, w)`; convert at the boundary in both directions (the bind-pose seed in §1 and the write-back
  decompose in §3). The skin import already swaps for bind matrices — keep the conversion local to the
  Jolt TU so the rest of the engine only ever sees `glm`.
- **Local vs. world is the whole bug surface.** Jolt bodies live in **world** space; `PoseOverrideComponent`
  is **local** (`scene.cppm:122`, fed straight into `localMatrix`). The §3 conversion *must* divide out the
  parent bone's world matrix, the exact inverse of `jointMatrices`' `worldMatrix(bone) * inverseBind`
  composition (`scene.cppm:826-827`). Getting this wrong renders the ragdoll in world space layered on top
  of the parent transform — limbs fly off. The CPU self-test in `scene.cppm:1559` (`jointMatrices` produces
  `worldBone * inverseBind`) is the reference identity to invert against.
- **Pure ragdoll only — no motors this phase.** Parse `drive*` into the constraint motor settings but leave
  the motors disabled and `weight = 1` everywhere. The limp-to-active blend (`DriveToPoseUsingMotors`
  reading the PD gains, `weight ∈ (0,1)`, UE Physics Blend Weight) is Phase 9; do not bleed it in here.
- **Write-back order is load-bearing.** `writeRagdollPoses` must run **after** `tickAnimation` (host
  `onUpdate`, `host.cppm:887`) and after `PhysicsSystem::Update`, all inside the same `simTick` composition,
  so the physics override replaces the animation override for the frame. If it ran before `tickAnimation`,
  the clip would overwrite the ragdoll and the body would never show. Compose it explicitly; do not rely on
  entt iteration order.
- **`Stabilize()` before create.** Jolt's `RagdollSettings::Stabilize()` adjusts part masses so a long
  chain (spine→neck→head) does not explode; call it once after all parts/constraints are set and before
  `CreateRagdoll`. Skipping it on a software (llvmpipe) build still "works" but jitters — keep it.
- **The play duplicate is the reset.** The ragdoll is added to the per-Play `PhysicsWorld`; `stopPlay`
  discards the play scene (`scene_edit_play.cpp:152`) and the world is torn down on `→Edit`
  (`onPlayStateChanged`, Phase 1). There is **no** manual restore of the authored pose — the discard *is*
  the restore, exactly as the README states for play mode. Do not add a "reset ragdoll" path.
- **Zero-shape bones are intentional skips,** not errors. A `BonePhysics` with `shapeHalfExtents == 0`
  (e.g. fingers, twist bones) gets no body and keeps its animation override — the parallel-array length
  must still match `SkinnedMeshComponent.bones` (the auto-fit in §2 fills every slot; a hand-edit may
  zero some), so validate length equality and skip per-bone on zero, never on a length mismatch.
