# physics plan

This plan brings a **physics engine** to SaffronEngine, end to end: rigid bodies (static / kinematic /
dynamic) with box / sphere / capsule / convex-hull / mesh **colliders**, **collision layers** + sensor
**triggers** + contact events, **kinematic** bodies that follow the animated skeleton, a **character
controller**, **raycast / shapecast scene queries**, and — last — a **powered ragdoll** built from the
per-bone metadata the rig already carries, that blends physics against animation per bone. The middleware
is **Jolt** (jrouwe/JoltPhysics, MIT) — the same engine that ships in Horizon Forbidden West and that
Godot adopted as its default 3D backend.

The reference points throughout are **UE5 Chaos** (its `UPrimitiveComponent` body-instance model,
`UPhysicsAsset` per-skeleton ragdoll definition, and the Physical Animation Component's per-bone Physics
Blend Weight) and **Unity** (the `Rigidbody` / `Collider` split, the layer collision matrix, the
`CharacterController`, and the Ragdoll Wizard that auto-fits capsules per bone). We copy the *split
component model*, the *layer matrix*, the *auto-fit-on-add* ergonomics, and the *per-bone physics blend*;
we drop PhysX/Chaos themselves for Jolt, and we drop the editor wizards in favour of an automatic fit.

## Why this is mostly extension, not new machinery

The engine was **designed with physics-shaped holes already reserved** — the animation plan's last phase
(`plans/animations/phase-13-foot-ik-and-physics-ahead.md`, COMPLETED) deliberately authored the seams a
ragdoll plugs into, inert. This is not a from-zero subsystem; the lifecycle hooks, the step tick, the pose
hand-off layer, and the per-skeleton ragdoll schema all exist and are correct — the gaps are *the Jolt
world, body creation from components, write-back, and the consumers*.

What already exists (verified against the code):

- **The per-skeleton ragdoll definition is already serialized and inert.** `BonePhysics`
  (`scene.cppm:153`) carries `shapeHalfExtents`, `mass`, `enum Joint { Fixed, Hinge, SwingTwist, Free }`,
  `swingTwistLimits` (radians), and the PD `driveStiffness`/`driveDamping`/`driveMaxForce` motor gains.
  `BonePhysicsComponent` (`scene.cppm:172`) is a `std::vector<BonePhysics>` parallel to
  `SkinnedMeshComponent.bones`. Its own comment: *"Reserved per-bone metadata for the eventual Jolt
  powered-ragdoll (UE-PhAT / Jolt-RagdollSettings shaped) … Authored once, mapped 1:1 later."* This is
  the `UPhysicsAsset` / Jolt `RagdollSettings` shape, already on disk — Phase 8 reads it, does not invent
  it.
- **The pose hand-off layer is reserved.** `PoseBuffer` (`animation.cppm:36`) is `{ local, override_,
  weight }`; `override_` is annotated *"where external producers (IK/physics) write"* and `weight` is the
  *"inert per-bone blend layer (v1 leaves it 0, meaning pure animation)."* `PoseDelta` (`animation.cppm:47`)
  is *"the same delta-pose machinery a physics handoff (ragdoll) uses."* `AnimationRuntime.lastPose`
  (`animation.cppm:114`) is snapshotted every tick so *"the eventual physics handoff finite-differences it
  for per-bone velocities so a ragdoll take-over does not pop. No consumer yet — reserved."* The ragdoll
  is *just another `override_`/`weight` producer*, exactly like the foot IK that already ships.
- **The lifecycle seam exists.** `SceneEditContext::onPlayStateChanged` (`scene_edit_context.cppm:220`) is
  a `SubscriberList<PlayState>` annotated *"the physics/scripting lifecycle seam."* The script VM already
  rides it: `host.cppm:694-714` subscribes and flips `scriptVmActive` on `Edit→Playing` / `Playing→Edit`.
  The physics world is built and torn down on the same edge, a sibling of the script VM.
- **The deterministic step tick exists.** `PlayFixedStep = 1/60` (`scene_edit_context.cppm:172`) and
  `PlayMaxDelta = 1/3` (`:173`, the hitch clamp) are already the constants `tickPlay` uses.
- **The simulation seam exists and is SDL-free.** `SceneEditContext::simTick` is a
  `std::function<void(Scene&, f32)>` (`scene_edit_context.cppm:224`) *"The Host points it at the script
  runtime"* — today `host.cppm:719` installs the Lua tick into it. Physics steps in the **same** seam; the
  Host composes physics-then-scripts into it.
- **The frame ordering already lands gameplay before rendering.** In `onUpdate` the Host runs
  `se::tickAnimation(...)` (writes pose overrides from clips) then `se::tickPlay(*editor, dt)` (runs
  `simTick`) — `host.cppm:882-890`, comment: animation runs *"before scripts so a script can still
  override a bone the same frame."* Later `renderScene` (host `onUi`) calls `updateWorldTransforms`
  (`scene.cppm:774`) → `jointMatrices` (`scene.cppm:811`) which feed the compute-skinning prepass. So
  physics reading/writing transforms + the pose belongs in `onUpdate`, **before** `renderScene`, all one
  frame — **no new render pass**.
- **The play scene is a throwaway duplicate.** `enterPlay` duplicates the scene via
  `sceneToJson`/`sceneFromJson` into `editor->playScene`; `stopPlay` discards it
  (`scene_edit_play.cpp`, `sceneedit/AGENTS.md`: *"Play mode has no undo — the discard IS the restore."*).
  A Jolt world built against the play duplicate dies with the duplicate on stop — no manual reset of
  authored data, no snapshot/restore.
- **Adding a serialized, editor-visible component is a fixed recipe.** Per `scene/AGENTS.md`: (1) declare
  the struct + `*ToJson`/`*FromJson` forward decls in `scene.cppm`; (2) edit the serde **body** inside
  `emitSceneSerde()` in `tools/gen-control-dto/gen.ts` (`scene_component_serde.generated.cpp` is
  do-not-hand-edit — regenerated by `bun run tools/gen-control-dto/gen.ts`); (3) register **once** in
  `scene_edit_components.cpp` `registerBuiltinComponents` via `registerComponent<C>(…)`. The inspector is
  generic (protocol-driven `fieldRenderer`), so fields appear automatically from the regenerated
  `se-types.ts`. `BonePhysicsComponent` already exists — the new `RigidbodyComponent`/`ColliderComponent`
  follow the same recipe.

What is missing — and what this plan builds:

1. **A `Saffron.Physics` module** wrapping Jolt. No module, no Jolt dependency, no world.
2. **The two motion components.** No `RigidbodyComponent` / `ColliderComponent` (only the *ragdoll*
   schema exists). No body creation from components, no transform write-back.
3. **Shapes, layers, triggers, queries, a character controller.** None exist.
4. **The ragdoll runtime.** `BonePhysicsComponent` is authored-but-inert — nothing reads it, no Jolt
   `Ragdoll`, no `DriveToPoseUsingMotors`, and `lastPose` has no consumer.

## The one load-bearing decision

**Physics steps in the existing `simTick` seam at `PlayFixedStep` and writes transforms / pose-overrides
*before* `renderScene`/skinning — the same frame, with no new render pass.** A fixed-step accumulator
(clamped by `PlayMaxDelta`) advances `PhysicsSystem::Update` inside the seam the Host already drives;
dynamic bodies write their world transform back to `TransformComponent` and the ragdoll writes into
`PoseBuffer.override_`/`weight` — both upstream of `updateWorldTransforms` → `jointMatrices` → the
compute-skinning prepass that every pass reads. Because the world is built against the **play duplicate**
on `Edit→Playing` and discarded on `Playing→Edit`, there is no authored-data reset; because it rides the
existing seam and frame ordering, rendering, the render graph, and the palette upload are untouched.

The decision that shapes everything after is **how physics reaches the bones**. It is the *same* reserved
blend layer the foot IK already uses: the ragdoll is one more producer writing `override_[i]` + `weight[i]`
per bone, and the Phase-3 evaluator's `final[i] = blend(local[i], override_[i], weight[i])` mixes physics
against animation with **no change to the sampling graph**. This is UE5's Physics Blend Weight model, and
it is why the ragdoll (Phases 8–9) is mechanical rather than a rewrite: the on-ramp shipped with the
animation plan. Reading physics into the bones any other way — overwriting bone `TransformComponent`s
directly — would dirty the scene and discard the per-bone blend that active ragdoll (motor-driven, partial
limp, hit-and-recover) requires.

## What "done" looks like

- A new **`Saffron.Physics`** module (DAG edge `Physics → {Core, Geometry, Scene, Animation}`), classic
  `#include` (it wraps Jolt's heavy C++ headers, so — like `scene`/`geometry`/`animation` — it does **NOT**
  `import std`), with a `Saffron.Physics:Types` partition (POD enums / `PhysicsMaterial` / layer ids + an
  opaque `PhysicsWorld` handle) and **one** `.cpp` impl unit that is the only TU including `<Jolt/…>`. Only
  `Saffron.Host` imports it (the Script rule, mirrored).
- **Jolt vendored** via `FetchContent` through `saffron_third_party`, pinned to a release tag, `GIT_SHALLOW
  ON`, samples/tests/viewer/perf off, `OVERRIDE_CXX_FLAGS OFF` (honour the libc++/`gnu++26` preset),
  `DOUBLE_PRECISION OFF`, and **`CROSS_PLATFORM_DETERMINISTIC ON`** (locked — see below).
- A **`PhysicsWorld`** (wrapping the Jolt `PhysicsSystem` + `BodyInterface` + `TempAllocatorImpl` +
  `JobSystemThreadPool` + the entity↔`BodyID` maps + a fixed-step accumulator) owned by `HostState`
  (`host.cppm:48`), a sibling of `animation` / `script`. Built on the `Edit→Playing` edge of
  `onPlayStateChanged` and destroyed on `→Edit`, mirroring `scriptVmActive`. Stepped inside `simTick`.
- The two **split** components on `Saffron.Scene` (so they serialize + register via the recipe):
  **`RigidbodyComponent`** (motion type, mass, linear/angular damping, `gravityFactor`, per-axis
  position+rotation locks, collision-layer index) and **`ColliderComponent`** (shape enum
  `Box|Sphere|Capsule|ConvexHull|Mesh`, dimensions, source-mesh `Uuid` for hull/mesh, local offset,
  `PhysicsMaterial{friction, restitution}`, `isSensor`). `Saffron.Physics` only *consumes* them. A
  **collider alone is a static body**; a rigidbody's motion type wins.
- **Shapes auto-fit on add** to the entity mesh AABB (editable after in the inspector); ragdoll bone
  colliders auto-fit a capsule per bone on skinned import, then hand-editable.
- A usable rigidbody engine: dynamic boxes fall and rest on static floors, shapes roll/bounce with
  material differences, a **layer collision matrix** + **sensor triggers** + contact events reach scripts,
  **kinematic** bodies follow the animated skeleton, a **character controller** walks, and
  **raycast/shapecast** queries answer from `se` and Lua.
- A **powered ragdoll** built from the reserved `BonePhysicsComponent` (per-bone body + shape +
  constraint mapped from `BonePhysics.joint`), writing per-bone results into `PoseBuffer.override_`/`weight`
  before skinning — pure limp on command, and a motor-driven **active ragdoll** that blends toward the
  animation target and recovers (hit reactions), driven by the same `weight[i]` layer.
- Scriptable from `se`, covered by `tests/e2e/physics*.test.ts`, documented under `docs/content/`.

## Design decisions (locked)

The engine owner chose these — honour them, do not relitigate.

1. **Split `RigidbodyComponent` + `ColliderComponent`, collider-alone = static.** Two components, not
   one fused body: `RigidbodyComponent` owns motion (type `Static|Kinematic|Dynamic`, mass, damping,
   `gravityFactor`, per-axis locks, layer index); `ColliderComponent` owns geometry + material + sensor
   flag. **Rule:** a `ColliderComponent` *without* a `RigidbodyComponent` is implicitly a **static** body
   (floors/walls = one component); *with* a `RigidbodyComponent`, its motion type wins. Both structs live
   in `Saffron.Scene` (so they serialize + register via the recipe); `Saffron.Physics` only consumes them.
   This is the Unity `Rigidbody`/`Collider` model.
2. **Ragdoll is last (Phases 8–9).** Phases 1–7 deliver a complete rigidbody engine — falling boxes,
   shapes, layers/triggers, kinematic bodies + bone-following, a character controller, raycasts — *before*
   any rig binding. The ragdoll is built on top of the rigidbody machinery and the pose seam, not before.
3. **Auto-fit shapes on add.** Adding a `ColliderComponent` fits the shape to the entity mesh AABB
   automatically (editable after in the inspector). Ragdoll bone colliders auto-fit a capsule per bone on
   skinned import, then hand-editable. Not manual-only, not a separate "fit" button — this is Unity's
   Ragdoll-Wizard ergonomic, made the default everywhere.
4. **`CROSS_PLATFORM_DETERMINISTIC ON` from Phase 1.** The owner wants bit-exact-across-machines
   simulation for future lockstep networking / replay, so the Jolt cache var is set in the bootstrap
   phase, not retrofitted. It carries a modest perf cost (noted, accepted). Single precision
   (`DOUBLE_PRECISION OFF`) is still fine — cross-platform determinism is float-deterministic, not a
   double-precision feature.

## Explicitly OUT (deferred)

Vehicles / wheeled-vehicle constraints; soft bodies / cloth; fluids; destruction / fracture; continuous
collision detection tuning beyond Jolt's defaults; a physics **debug-draw** render pass (collider
wireframes overlay via the existing native overlay if needed, not a new pass); async physics on a separate
thread from the sim tick; networked rollback / prediction (the determinism flag *enables* it, this plan
does not build it); scene-graph-parenting-aware joints between authored entities beyond the ragdoll;
`SkeletonMapper` (coarse ragdoll ↔ render-rig retargeting); per-shape compound authoring in the editor
beyond the auto-fit + ragdoll compound. Each extends the same `PhysicsWorld` + component seams.

## Reuse vs build

**Reuse:** the reserved `BonePhysics`/`BonePhysicsComponent` ragdoll schema (`scene.cppm:153`/`:172`) +
its component serde + registration; the `PoseBuffer.override_`/`weight` blend layer + `PoseDelta` +
`AnimationRuntime.lastPose` (`animation.cppm:36`/`:47`/`:114`) — the ragdoll's exact write target; the
`onPlayStateChanged` lifecycle seam + the `scriptVmActive` build/tear-down pattern
(`host.cppm:694-714`); the `simTick` `std::function` seam + `PlayFixedStep`/`PlayMaxDelta` + the
play-duplicate-discarded-on-stop model; the `tickAnimation`→`tickPlay`→`renderScene` frame ordering
(`host.cppm:882-890`); `updateWorldTransforms`/`jointMatrices`/`worldMatrix` for reading and writing bone
transforms; the `.smesh` vertex stream in `Saffron.Geometry` (convex-hull / mesh-shape cooking source);
`registerComponent<T>` + `registerBuiltinComponents` + the `gen.ts` component-serde codegen; the
`registerCommand<Params,Result>` + DTO codegen + version-stamped reconcile poll + `se` CLI + the e2e
harness (`tests/e2e`, mirroring `rig-preview.test.ts` / `rig-query.test.ts`); the generic
add-component / set-component-field commands (`control_commands_scene.cpp`) that already drive arbitrary
registered components; the native overlay path if a debug-draw toggle is wanted.

**Build new:** `Saffron.Physics` (the `:Types` partition + the single Jolt-including `.cpp`); the Jolt
`FetchContent` entry under `saffron_third_party`; the `PhysicsWorld` (system + body interface + temp
allocator + job system + entity↔`BodyID` maps + accumulator + the three `BroadPhaseLayer`/`ObjectLayer`
filter interfaces + a `ContactListener`); `RigidbodyComponent` + `ColliderComponent` + serde +
registration + auto-fit; the body-creation-from-components pass + transform write-back; shape cooking
(box/sphere/capsule from dims, convex-hull/mesh from `.smesh` vertices); the layer matrix + sensor/trigger
contact-event drain to scripts; kinematic `MoveKinematic` + per-bone kinematic bodies that follow the
pose; the `CharacterVirtual` controller; `NarrowPhaseQuery` raycast/shapecast + the control command + Lua
API; the Jolt `Skeleton`/`RagdollSettings`/`Ragdoll` build from `BonePhysicsComponent` + constraint
mapping + write-back into `override_`/`weight`; `DriveToPoseUsingMotors` + the per-bone weight blend +
ragdoll auto-fit-on-import + per-bone editing commands; the physics control commands + `se` verbs; the
e2e tests + docs pages.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`). Mark a phase
`COMPLETED` only when validation-clean (`make engine` + `make prepare-for-commit`; wire/runtime phases also
`make e2e`; component phases also `bun run check`); delete a phase file only *after* it is `COMPLETED` and
merged. Per the root `AGENTS.md` "keep current" rule, **every** phase ends by adding/updating its
`docs/content/` explanation page (+ a hub `_index.md` row), its `tests/e2e` coverage, and its `se`/control
command in the same change. **Phase 1 also updates the module DAG + the module list in the root
`AGENTS.md`** (`Saffron.Physics` is listed in "Not yet" at `AGENTS.md:272` today — that line moves to
"Built" as the plan lands).

## Phases

| Phase | What | Status |
|---|---|---|
| [1 — `Saffron.Physics` module + Jolt vendoring + world lifecycle](phase-01-module-and-world-bootstrap.md) | new `Saffron.Physics` module (DAG edge, classic-include, `:Types` partition); Jolt vendored via FetchContent (`CROSS_PLATFORM_DETERMINISTIC` on, samples off); a `PhysicsWorld` owned by `HostState`, created on `Edit→Playing` and destroyed on `→Edit` via `onPlayStateChanged`. Builds in isolation; entering/stopping play allocates+frees a Jolt world, validation-clean. | NOT STARTED |
| [2 — `RigidbodyComponent` + `ColliderComponent` + a falling box](phase-02-rigidbody-collider-and-falling-box.md) | the two split components (struct + `gen.ts` serde + registration), the fixed-step accumulator wired into the `simTick` seam, body creation from components on play, dynamic transform write-back to `TransformComponent`; collider-alone = static. A dynamic box dropped on a static floor falls and rests; e2e asserts its Y settles. | NOT STARTED |
| [3 — collision shapes + materials + auto-fit](phase-03-collision-shapes-and-materials.md) | Sphere, Capsule, ConvexHull and Mesh shapes (hull/mesh cooked from the entity `.smesh` vertices via `Saffron.Geometry`), `PhysicsMaterial` friction/restitution, and auto-fit-to-AABB when a `ColliderComponent` is added. A capsule and a sphere roll/bounce with material differences. | NOT STARTED |
| [4 — collision layers + sensors/triggers + contact events](phase-04-collision-layers-and-triggers.md) | a fixed `ObjectLayer`/`BroadPhaseLayer` set + collision matrix (`RigidbodyComponent.layer` indexes it), the three Jolt filter interfaces, sensor bodies (`ColliderComponent.isSensor`), and a `ContactListener` draining overlap/contact events to scripts over the control plane. An object passes through a sensor and fires a script-visible event. | NOT STARTED |
| [5 — kinematic bodies + bone-following](phase-05-kinematic-bodies-and-bone-following.md) | the Kinematic motion type via `MoveKinematic`, and per-bone kinematic bodies that **follow** the animated pose each step so the world reacts to a moving character (binding mode b). A box bounces off a walking character's animated arm. | NOT STARTED |
| [6 — character controller (Jolt `CharacterVirtual`)](phase-06-character-controller.md) | a capsule character via Jolt `CharacterVirtual` + `CharacterVirtualSettings` and a control command to move it; the character walks and steps over the floor scene (binding mode a — capsule represents the character, animation independent). | NOT STARTED |
| [7 — scene queries: raycast / shapecast + script API](phase-07-scene-queries-raycast.md) | `NarrowPhaseQuery` raycast + shapecast exposed as a control command and a Lua script API; `se raycast` from the CLI returns the hit body + point + normal + distance. | NOT STARTED |
| [8 — ragdoll from the reserved `BonePhysicsComponent`](phase-08-ragdoll-from-bone-physics.md) | build a Jolt `Ragdoll` + `Skeleton` from the reserved `BonePhysicsComponent` (shapes + per-joint constraints), simulate it, and write per-bone results back into `PoseBuffer.override_`/`weight` before skinning (binding mode c, pure ragdoll). A clip-driven character goes limp and collapses realistically on command. | NOT STARTED |
| [9 — active ragdoll (motor-driven blend) + auto-fit authoring](phase-09-active-ragdoll-and-pose-blend.md) | `DriveToPoseUsingMotors` pulling the ragdoll toward the animation target, the per-bone `weight` layer blending physics vs animation (partial/active ragdoll, hit reactions that recover), plus auto-fit of `BonePhysicsComponent` on skinned import + per-bone editing over the control plane. A hit blends a limb to physics and recovers to the animation. | NOT STARTED |

## Sequencing

Strictly dependency-ordered, lowest-risk first, each phase independently shippable.

**1** is the bootstrap: the module + Jolt vendoring + the `PhysicsWorld` lifecycle on the
`onPlayStateChanged` edge. It builds and a Jolt world allocates/frees on play with **no bodies yet** — the
pure plumbing, validation-clean in isolation.

**2 is the milestone where a box visibly falls.** It adds the two split components and wires the fixed-step
accumulator into the `simTick` seam with dynamic transform write-back *before* `renderScene` — the
load-bearing decision made concrete. Everything after Phase 2 is **either a new body type or a consumer of
that same step + write-back seam**: shapes (3) and layers/triggers (4) flesh out *what* bodies are and
*how they interact*; kinematic + bone-following (5), the character controller (6), and queries (7) add
*producers and readers* of the world; the ragdoll (8) and active ragdoll (9) are the rig binding built on
top, routing physics through the reserved `PoseBuffer.override_`/`weight` layer.

**3–4** depend only on 2 (shapes and the layer matrix are body authoring). **5–7** depend on 2 (and 3 for
shapes); they are independent of each other once 2–3 land, so they can run in parallel. **8 depends on 2–5**
(it needs dynamic bodies, constraints, and the kinematic-following pattern) plus the COMPLETED animation
plan's blend layer; **9 builds on 8.** The 8→9 order is itself dependency-ordered: pure passive ragdoll
(physics overwrites the pose) must work before motor-driven blending (physics *negotiates* with the pose
via `weight`).

A reasonable **first visible slice** is **1 + 2** (a dynamic box falls onto a static floor and rests — the
proof the world steps in `simTick` and writes transforms before rendering, before any shape variety or
interaction). A reasonable **first demo-able slice** is **1–3 + 6** (varied shapes resting on a floor with
material differences, plus a capsule **character** you walk over the scene). The ragdoll headline is
**+8 + 9**; full gameplay interactivity (triggers, queries, bone-following) is **+4 + 5 + 7**.
