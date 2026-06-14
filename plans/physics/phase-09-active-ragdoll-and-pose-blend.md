# Phase 9 — Active ragdoll (motor-driven blend) + auto-fit ragdoll authoring

**Status:** COMPLETED

## Goal

Close the loop on the README's "one load-bearing decision": a ragdoll that is **driven toward the
animation target** by Jolt constraint motors, mixed against the animation through the **same per-bone
`PoseBuffer` blend layer** foot IK already proves out, so a hit can blow a limb to physics and have it
recover to the pose. Concretely: feed the per-frame animation target pose into the Phase-8 ragdoll via
`Ragdoll::DriveToPoseUsingMotors` (reading each bone's PD gains from `BonePhysics`), drive the per-bone
`weight[i]` so upper-body can go physics while lower-body stays animated (**partial / active ragdoll**),
**auto-fit** a `BonePhysicsComponent` capsule per bone at skinned import (then hand-editable — the locked
auto-fit decision), and add per-bone editing + an active-ragdoll/blend command over the control plane and
`se`. This is the **fullest binding mode** and the last physics phase: after it, ragdoll, hit reactions,
and animation all share one pose path.

This phase depends on Phase 8 having built the **passive** ragdoll (the `Ragdoll`/`RagdollSettings`/
`Skeleton` instance, write-back of bodies → bone poses through `override_`/`weight` at full physics
weight) and on the rigidbody/world machinery from Phases 1–7. It does not add a render pass — it runs
inside the existing `simTick` seam, before `renderScene`, in one frame.

## What exists to build on

- **The blend layer is the only seam and it already admits an external producer.** `PoseBuffer`
  (`animation.cppm:36`) carries `local` (sampled animation), `override_` ("external producers (IK/physics)
  write", `animation.cppm:39`), and `weight` (`animation.cppm:40`, "the inert per-bone blend layer (v1
  leaves it 0, meaning pure animation)"). The blend primitive is `blendJoint(base, over, weight)`
  (`animation.cpp:173`) — `glm::mix` on T/S, `glm::normalize(glm::slerp(...))` on R. Foot IK already
  writes `override_`/`weight` per chain via `applyFootIk` (`animation.cpp:221`) and the comment at
  `animation.cpp:702` names ragdoll as the next producer of "the same `override_`/`weight` blend layer."
  **Ragdoll-vs-animation blend reuses this verbatim — no new mixing code.**
- **The PD gains are already authored, per bone, inert.** `BonePhysics` (`scene.cppm:153`) carries
  `driveStiffness`, `driveDamping`, `driveMaxForce` (`scene.cppm:165-167`, "PD motor gains"), `joint`
  (`scene.cppm:157`, `Fixed|Hinge|SwingTwist|Free`), `swingTwistLimits` (`scene.cppm:164`, radians),
  `shapeHalfExtents` (`scene.cppm:155`) and `mass` (`scene.cppm:156`). `BonePhysicsComponent`
  (`scene.cppm:172`) is a `std::vector<BonePhysics> bones` parallel to `SkinnedMeshComponent.bones`
  (`scene.cppm:82`). The verbatim comment (`scene.cppm:150-152`): "Reserved per-bone metadata for the
  eventual Jolt powered-ragdoll (UE-PhAT / Jolt-`RagdollSettings` shaped) ... Authored once, mapped 1:1
  later." Phase 8 maps it to `RagdollSettings`; this phase reads `drive*` into the **constraint motor
  settings** that `DriveToPoseUsingMotors` uses, and auto-fits `shapeHalfExtents` at import.
- **The animation target pose is computed every frame, before the sim seam.** `tickAnimation`
  (`animation.cppm:123`, `host.cppm:887`) samples `local`, applies foot IK, and emits a
  `PoseOverrideComponent` per driven bone — and it runs **before** `tickPlay` (`host.cppm:890`) so a
  later sim producer overrides the same frame (`host.cppm:882-884`). So at the moment the ragdoll motors
  step, the rig's animated local pose is in hand: `applyFootIk` builds it in a `finalLocal` vector
  (`animation.cpp:220`), which is exactly the per-joint target a `SkeletonPose` needs.
- **The velocity hand-off seed is already snapshotted.** `AnimationRuntime.lastPose` (`animation.cppm:114`)
  is written every tick (`animation.cpp:730`, `runtime.lastPose[key] = finalLocal;`) with the verbatim
  reservation "the eventual physics handoff finite-differences it for per-bone velocities so a ragdoll
  take-over does not pop. No consumer yet — reserved." **This phase is that consumer** when blowing a
  bone from animation (weight 0) to physics (weight 1).
- **`spawnSkinnedModel` is where auto-fit attaches.** `spawnSkinnedModel` (`assets.cppm:4806`) walks
  `result.nodes`, tags joints with `BoneComponent` (`assets.cppm:4848`), builds `skin.bones`
  (`assets.cppm:4864`) + `skin.inverseBind` (`assets.cppm:4865`), and conditionally attaches an
  `AnimationPlayerComponent` for rigs that ship clips (`assets.cppm:4870-4876`). It calls
  `relinkHierarchy(scene)` last (`assets.cppm:4878`) to resolve `boneHandles`. **Auto-fit slots in right
  before that `relinkHierarchy`**, after `skin.bones` is final, mirroring the player-attach block.
- **Foot IK is the per-component command template.** `control_commands_animation.cpp` defines
  `footIkOf` (`:148`, resolve entity + attach the component if absent), `footIkState` (`:138`, struct →
  DTO), `get-foot-ik`/`set-foot-ik` (`:369`/`:381`), and bumps `ctx.sceneEdit.animationVersion`
  (`:399`). The DTOs `SetFootIkParams`/`GetFootIkParams`/`FootIkResult` are `control_dto.cppm:1379-1396`.
  The generic per-field edit is `set-component-field` (`control_commands_scene.cpp:1196`) which already
  merges one field into any registered component — so editing a `BonePhysics` field needs **no new
  generic command**, only an index into the array (see §4).
- **The component recipe is fixed and `BonePhysicsComponent` already rides it.** It is declared
  (`scene.cppm:172`), has serde in `emitSceneSerde()` in `gen.ts`, and is registered in
  `registerBuiltinComponents` (`scene_edit_components.cpp`) — verify all three are present (the
  animations plan reserved it in its Phase 13). If any step is missing, complete it per
  `scene/AGENTS.md`; do **not** hand-edit `scene_component_serde.generated.cpp`.
- **Lifecycle + step are owned by Phases 1/8.** The `PhysicsWorld` is built on the `Edit→Playing` edge of
  `onPlayStateChanged` (`scene_edit_context.cppm:220`), stepped in the `simTick` seam against
  `PlayFixedStep = 1/60` (`scene_edit_context.cppm:172`) clamped by `PlayMaxDelta = 1/3`
  (`:173`), and dies with the discarded play duplicate on stop (`scene_edit_play.cpp`). Active ragdoll
  is one more thing the seam does each fixed step.

## Work

### 1. Feed the animation target to the ragdoll motors (`Saffron.Physics`)

In `Saffron.Physics`, add the per-step drive call. The animated target is the rig's `finalLocal`
(post-IK) local pose; convert it to a Jolt `SkeletonPose` (joint-local TRS, in `SkinnedMeshComponent.bones`
order, the same order `RagdollSettings::mSkeleton` was built from in Phase 8) and drive:

```cpp
/// Drive every active ragdoll toward its rig's current animated pose. `targets` is keyed by
/// the mesh entity uuid; each is the post-IK local pose (SkinnedMeshComponent.bones order) the
/// animation evaluator produced this frame. Reads each bone's BonePhysics drive* gains (already
/// mapped onto the constraint motors at build) and motors the bodies toward the pose. Called
/// once per fixed step, before PhysicsSystem::Update.
struct PoseTarget { u64 entity; std::vector<JointPose> local; };  // JointPose from Saffron.Animation
void driveRagdollsToPose(PhysicsWorld& world, const std::vector<PoseTarget>& targets);
```

Inside, for each ragdoll: fill a `JPH::SkeletonPose` from the `PoseTarget.local` (set the joint matrices
from TRS), then `ragdoll->DriveToPoseUsingMotors(skeletonPose)`. The motor strength is the per-bone
`BonePhysics.driveStiffness`/`driveDamping`/`driveMaxForce` baked into the constraint's motor settings
at build (`SwingTwistConstraintSettings`/`HingeConstraintSettings`/`SixDOFConstraintSettings` motor
frequency + damping, max torque) in Phase 8; **active vs passive is just whether motors are enabled.**
A `BonePhysics.joint == Free` bone has no motor (pure passive limb).

> A fully-passive ragdoll (Phase 8) leaves motors off; a fully-active one motors every bone to the pose
> at frequency from the gains. **This phase makes the motor drive real** and exposes the gains/enable.

### 2. Per-bone physics weight: animation ↔ ragdoll partial blend

The write-back from physics to the pose is already the foot-IK shape (`override_[i]` + `weight[i]`,
blended by `blendJoint`). Phase 8 wrote `weight[i] = 1` for every ragdoll bone (full physics). Active
ragdoll makes `weight` **per-bone and dynamic**:

```cpp
/// Per-bone target physics weight for a rig: 0 = pure animation, 1 = pure physics. Authored
/// regions (upper-body physics, lower-body animated) plus transient hit ramps live here; the
/// evaluator slerps toward it so a limb eases between animation and ragdoll without a pop.
struct RagdollBlend
{
    std::vector<f32> target;   // per bone, indexed like SkinnedMeshComponent.bones
    std::vector<f32> current;  // smoothed toward target each tick (the eased blend weight)
    f32 rate = 6.0f;           // weight units/sec the smoothing approaches `target` at
};
```

The evaluator (Phase 8's physics write-back path) sets `pose.weight[i] = blend.current[i]` and the read
of the body transform stays in `override_[i]`. The Phase-3 blend (`blendJoint`, `animation.cpp:173`)
then mixes per joint — **no change to the mixing code**, exactly the foot-IK pattern (`animation.cpp:702`
already anticipates this). Smoothing reuses the existing `tau`/`smoothstep` idiom (foot IK ramps the same
way, `animation.cpp:702-725`).

**The pop guard (the reserved `lastPose` consumer):** when a bone's `current` weight crosses from ~0 to
physics (a hit, see §5), seed the body's linear/angular velocity from the finite difference of
`AnimationRuntime.lastPose` (`animation.cppm:114`) vs this frame's animated pose — `(pose - lastPose)/dt`
per bone, mapped to the body via `BodyInterface::SetLinearVelocity`/`SetAngularVelocity` at the take-over
instant — so the limb continues the animation's motion instead of dropping. This is the verbatim
reservation at `animation.cppm:111-114` becoming load-bearing.

### 3. Auto-fit `BonePhysicsComponent` capsules on skinned import

In `spawnSkinnedModel` (`assets.cppm:4806`), after `skin.bones`/`skin.inverseBind` are final and before
`relinkHierarchy(scene)` (`assets.cppm:4878`), attach an auto-fit `BonePhysicsComponent`:

```cpp
// Auto-fit a per-bone capsule from the rest skeleton: each bone's half-length spans toward
// its child joint, the radius a fraction of the segment (leaf bones get a small default).
// Editable after in the inspector (the locked auto-fit decision) — fit once, hand-tune later.
BonePhysicsComponent& phys = addComponent<BonePhysicsComponent>(scene, meshEntity);
phys.bones.resize(skin.bones.size());
for (std::size_t i = 0; i < skin.bones.size(); i = i + 1)
{
    BonePhysics& bp = phys.bones[i];
    bp.shapeHalfExtents = fitBoneCapsule(scene, skin, i);  // half-length along bone, radius in x/z
    bp.mass = 1.0f;
    bp.joint = BonePhysics::Joint::SwingTwist;  // sane default; pelvis/root tuned to Free/Fixed by hand
}
```

`fitBoneCapsule` measures the rest-pose distance from this joint to its child joint(s) in
`SkinnedMeshComponent.bones` (resolve child via the bone-entity hierarchy / `boneHandles`), exactly the
data the skeleton overlay already walks. This populates `shapeHalfExtents`; the `joint`/`swingTwistLimits`/
`drive*` defaults from `BonePhysics` (`scene.cppm:153`) stand until edited. Because it goes through the
component path, it **serializes for free** and round-trips with the project. (The same `fitBoneCapsule`
is what a future "re-fit ragdoll" control verb would call — out of scope here.)

> This mirrors the locked auto-fit decision for `ColliderComponent` (fit shape to mesh AABB on add): a
> ragdoll is usable the instant a rig imports, then hand-editable — not a manual per-bone authoring chore.

### 4. Per-bone editing over the control plane + an active-ragdoll command

Per-bone field edits ride the generic `set-component-field` (`control_commands_scene.cpp:1196`) once it
can address an array element — add an optional `index` to its params so
`set-component-field {entity, "BonePhysicsComponent", "bones", <idx>, field "driveStiffness", value}` edits
one bone. Add a focused active-ragdoll command for the blend/enable (the foot-IK command shape,
`control_commands_animation.cpp:381`):

```cpp
// control_dto.cppm
struct SetRagdollParams
{
    EntitySelector entity;
    std::optional<bool> active;       // motors on (active) vs off (passive limp)
    std::optional<f32> bodyWeight;    // uniform target physics weight 0..1 (whole rig)
    std::optional<i32> bone;          // optional single-bone target (with weight) for partial ragdoll
    std::optional<f32> weight;        // per-bone target physics weight when `bone` is set
};
struct RagdollResult
{
    bool active;
    f32 bodyWeight;     // mean target weight across bones
    i32 bones;          // BonePhysicsComponent.bones count
    bool present;       // is a ragdoll instance live this play session?
};
```

```cpp
// control_commands_animation.cpp (or a new control_commands_physics.cpp if Phase 1 added one)
registerCommand<SetRagdollParams, RagdollResult>(
    reg, "set-ragdoll",
    "set-ragdoll {entity, active?, bodyWeight?, bone?, weight?} — drive a rig's active ragdoll blend",
    [](EngineContext& ctx, const SetRagdollParams& params) -> Result<RagdollResult> { /* … */ });
registerCommand<GetRagdollParams, RagdollResult>(
    reg, "get-ragdoll", "get-ragdoll {entity} — the rig's ragdoll presence, active flag, and blend",
    [](EngineContext& ctx, const GetRagdollParams& params) -> Result<RagdollResult> { /* … */ });
```

Bump the right version stamp on a blend change (`animationVersion`, `scene_edit_context.cppm:217`, since
the reconcile poll already keys on it for the rig — `control_commands_animation.cpp:399` is the precedent).
Editing an authored `BonePhysics` field bumps `sceneVersion` like any component edit. Add `se set-ragdoll`/
`se get-ragdoll` verbs in `tools/se/source/main.cpp` (keep-scriptable rule). Run
`bun run tools/gen-control-dto/gen.ts` and commit all five generated outputs (per `control/AGENTS.md`).

### 5. Hit reaction = a transient localized weight ramp

A "hit" is not new machinery — it is `set-ragdoll {bone, weight: 1}` on the struck limb with a decay back
to its authored target, plus the §2 velocity seed from `lastPose` so the limb takes over mid-motion.
Provide a convenience over the same command (e.g. `bone` + a `weight` that the evaluator's smoothing
decays from 1 back to the region's authored target at `RagdollBlend.rate`) so an upper-body hit blends the
arm/shoulder to physics and recovers to the animation as the ramp runs out — the headline "a hit blends a
limb to physics and recovers" result. Partial ragdoll (upper-body physics, lower-body animated) is the
same thing with static per-region `target` weights instead of a decaying ramp.

### 6. Host composition

The Host already runs `tickAnimation` before `tickPlay` (`host.cppm:887-890`) so the animated pose exists
before the sim seam. Inside the `simTick` lambda (`host.cppm:719`) — which Phases 1/8 made compose
physics + scripts — the fixed-step body becomes: `driveRagdollsToPose(world, targets)` (§1) →
`PhysicsSystem::Update` → write each ragdoll's `GetPose`/`GetRagdollState` back into the rig's
`override_`/`weight` at `RagdollBlend.current` (§2), all before `renderScene` (host `onUi`,
`host.cppm:942`) composes the bones the same frame. No new render pass; the deformed-vertex prepass and
skinned BLAS rebuild from the animation plan consume the blended result unchanged.

## Validation (done criteria)

- `make engine` green; `make prepare-for-commit` clean (clang-format + clang-tidy) for the new
  `Saffron.Physics` drive/blend code, the `assets.cppm` auto-fit block, and the new control command.
- `bun run check` clean — the regenerated `@saffron/protocol` picks up `SetRagdollParams`/`RagdollResult`
  and the new `set-component-field` `index`, and the generic inspector renders the `BonePhysics` array
  fields automatically (protocol-driven `fieldRenderer`).
- `make e2e`: a new `tests/e2e/ragdoll-blend.test.ts` (mirroring `foot-ik.test.ts` /
  `animation-control.test.ts`) that imports a rigged fixture, enters Play, and asserts: (a) a skinned
  import auto-attached a `BonePhysicsComponent` with one `BonePhysics` per bone; (b) `set-ragdoll
  {bodyWeight: 1}` makes a struck/free bone's world Y diverge from the pure-animation pose (physics took
  over); (c) ramping `bodyWeight` back to 0 returns the bone to within epsilon of the animated pose (the
  recover); (d) a validation-clean log. Use `get-world-transform` (`control_dto.cppm:1400`,
  `WorldTransformResult`) to read the bone world position, as `foot-ik.test.ts` does for the planted foot.
- `docs/`: add `docs/content/explanations/physics/active-ragdoll.md` — the active/passive/partial spectrum,
  `DriveToPoseUsingMotors` reading the PD gains, the per-bone weight blend reusing the animation blend
  layer, auto-fit on import, and the `lastPose` velocity hand-off as the no-pop guard. Cite UE's Physical
  Animation Component / Physics Blend Weight as the destination (the animations plan already names it).
  Add the hub `_index.md` row.

## Notes / gotchas

- **The blend layer is the only path — for ragdoll too.** Physics writes `override_[i]`/`weight[i]` and
  nothing else; resist writing a bone's `TransformComponent` or `PoseOverrideComponent` directly. The
  whole point of the README's load-bearing decision is that ragdoll is "just another override-pose
  producer." Foot IK and ragdoll co-exist on the same rig by composing into `finalLocal` in producer order
  (`animation.cpp:702` runs IK after sampling; ragdoll runs after the physics step) — the per-bone
  `weight` arbitrates.
- **Joint mapping must be exact.** `BonePhysics.joint` → the Jolt constraint must match what Phase 8 built
  (`Fixed`→`FixedConstraint`, `Hinge`→`HingeConstraint`, `SwingTwist`→`SwingTwistConstraint`,
  `Free`→`PointConstraint`/no motor), and `swingTwistLimits` (`scene.cppm:164`, radians) → the
  `SwingTwistConstraintSettings` limits. `DriveToPoseUsingMotors` only affects bones whose constraint has
  motors enabled; a `Free` bone is inert under drive (the desired "limp" behavior).
- **Order in the fixed step matters.** Drive the motors *before* `PhysicsSystem::Update` (the motors are
  read during the solve), and write the pose back *after*. Run it at `PlayFixedStep` granularity, not the
  variable frame dt, or the motor frequency/damping (in Hz) won't match across machines — which also keeps
  the Phase-1 `CROSS_PLATFORM_DETERMINISTIC` guarantee intact.
- **`RagdollSettings::Stabilize()`** must have been called at build (Phase 8) or motored limbs jitter; this
  phase assumes a stabilized ragdoll and only drives it.
- **Auto-fit is a starting point, not ground truth.** A capsule fitted from joint-to-joint distance is
  crude for hands/feet/head; that is why it is hand-editable. Don't over-engineer the fit — fit, then let
  the inspector tune. The pelvis/root commonly want `Free` or a kinematic anchor, set by hand.
- **Deferred beyond this phase (out of scope):** `SkeletonMapper` (coarse physics ragdoll ↔ a denser
  render rig — Jolt supports it, but v1 maps 1:1 to `SkinnedMeshComponent.bones`); ragdoll cloth/soft
  bodies; per-bone collision groups beyond the rig's own self-collision filter; motion warping. Each is a
  further consumer of, or producer into, this same pose path.
