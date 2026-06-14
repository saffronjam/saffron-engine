# Phase 2 — `RigidbodyComponent` + `ColliderComponent` + a box falling on a floor

**Status:** COMPLETED

## Goal

Land the two **split physics components** (`RigidbodyComponent` + `ColliderComponent`) all the way through the component recipe, wire a **fixed-step accumulator** into the `simTick` seam the Host composes, build **Jolt bodies from the components** on the Edit→Playing edge, and **write dynamic body world transforms back** into each entity's `TransformComponent` every step. The visible result: a dynamic box dropped above a static floor falls under gravity and comes to rest on it. An e2e test boots a headless engine, enters play, runs a few hundred ticks, and asserts the box's Y stops descending and settles at a plausible rest height.

This is the first *moving* physics — Phase 1 stood up `Saffron.Physics` (the module, the Jolt link, the `PhysicsWorld` handle owned by `HostState`, the lifecycle subscription on `onPlayStateChanged`, startup/teardown of the Jolt `Factory`/`PhysicsSystem`/job system). Phase 2 turns that empty world into one that has bodies and steps. **Box shape only**; every other shape (sphere/capsule/hull/mesh) is Phase 3, and the character controller, layers/triggers, kinematic bone-following, and ragdoll are later. Keep this phase to the smallest end-to-end slice that proves the component → body → step → write-back loop.

## What exists to build on

- **The component recipe is fixed and documented** (`engine/source/saffron/scene/AGENTS.md`, "Adding or changing a component"): (1) declare the struct + its `*ToJson`/`*FromJson` forward decls in `scene.cppm`; (2) edit the serde **body** inside `emitSceneSerde()` in `tools/gen-control-dto/gen.ts` (the committed `scene_component_serde.generated.cpp` is regenerated, never hand-edited — `gen.ts:2309` `emitSceneSerde()`); (3) register **once** in `registerBuiltinComponents` (`engine/source/saffron/sceneedit/scene_edit_components.cpp:18`) via `registerComponent<C>(…)`. Miss step 3 and the component silently never serializes nor reaches the editor.
- **The recently-added `BonePhysicsComponent` is the closest serde precedent** for a nested-data physics component: struct at `scene.cppm:153` (`BonePhysics`) / `:172` (`BonePhysicsComponent`); serde body `gen.ts:2807-2860` (`bonePhysicsComponentToJson`/`FromJson`, including the `Joint` enum string table and the `jsonF32Or`/`vec3FromJson` helpers); registration `scene_edit_components.cpp:121`. The `FootIkComponent` serde (`gen.ts:2772`) shows the enabled-flag + nested-array shape. `RigidbodyComponent`/`ColliderComponent` each carry an enum + scalars + a `vec3` + (for the collider) a `Uuid` and a nested `PhysicsMaterial` — copy these patterns exactly.
- **`registerComponent<C>(reg, "Name", onAdd, toJson, fromJson, removable)`** — the `onAdd` lambda is where the **auto-fit** hook lives (it runs when a component is added). The trailing `bool` is `removable` (`scene_edit_components.cpp:108-122` pass `true`). The generic `add-component` control command calls `row->addDefault(...)` (`control_commands_scene.cpp:300`), which routes through `registerComponent`'s default-construct + `onAdd`; `set-component` routes through `row->deserialize` (`:344`). So both the editor "add component" button and the wire path are covered for free once registered.
- **`SceneVersion = 3`** (`scene.cppm:1020`), bumped in `doc["version"]` at save (`:1149`) with migration branches gated `version < 1 || version > SceneVersion` in `sceneFromJson` (`:1171`). Adding two new components with sensible defaults does **not** change the on-disk shape of an existing scene (absent components simply don't appear), so **do not bump `SceneVersion`** — and there is nothing to add to `runSceneSerializationSelfTest` (`scene.cppm:1254`) unless a default would alter a round-trip.
- **`TransformComponent`** (`scene.cppm:41`): `translation`, `scale`, `rotation` (Euler XYZ radians — the editor edits these directly). **This is what the write-back targets.** `WorldTransformComponent` (`scene.cppm:60`) is the cached world `mat4`, overwritten each frame by `updateWorldTransforms` (`scene.cppm:774`); it is runtime-only and unregistered, so physics never writes it directly — it writes the local `TransformComponent` and lets `updateWorldTransforms` recompose. `worldTranslation`/`worldRotation` (`scene.cppm:752`/`:758`) read the cached world transform — use these to seed a body's **initial** world pose at creation, and `glm::eulerAngles`/`glm::quat` to convert between the body's quaternion and the component's Euler `rotation` (the existing decompose helpers in `scene.cppm` GMF include `glm/gtx/matrix_decompose.hpp`).
- **The `simTick` seam is a `std::function<void(Scene&, f32)>` on `SceneEditContext`** (`scene_edit_context.cppm:224`, annotated "The Host points it at the script runtime; std-only here"). It is invoked from `tickPlay` (`scene_edit_play.cpp:200-203`) with `activeScene(ctx)` and the clamped `dt`, **only while Playing or stepping**. `PlayFixedStep = 1/60` (`scene_edit_context.cppm:172`); `PlayMaxDelta = 1/3` (`:173`, the hitch clamp). A stepped frame forces `dt = PlayFixedStep` (`scene_edit_play.cpp:194`).
- **The Host owns the seam composition** (`host.cppm`). Today `state->editor->simTick` is a lambda that runs *only scripts* (`host.cppm:719-731`); it is cleared to `nullptr` on teardown (`:981`). The lifecycle subscription on `onPlayStateChanged` (`host.cppm:694-718`) starts the script VM on `Edit→Playing` and stops it on `Playing→Edit` — **the exact pattern the `PhysicsWorld` build/teardown mirrors**. `HostState` (`host.cppm:48-60`) already holds `AnimationRuntime animation` (`:53`) and `ScriptHost script` (`:54`) as siblings; **`PhysicsWorld physics` is added beside them in Phase 1**, and this phase fills the simTick composition to step it.
- **Per-frame ordering in `onUpdate`** (`host.cppm:882-890`): `se::tickAnimation(...)` writes pose overrides **before** `se::tickPlay(*editor, dt)` runs `simTick`. So physics, stepping inside `simTick`, runs **after** animation and **before** `renderScene` (the `onUi` closure at `host.cppm:921`, which calls `updateWorldTransforms` → `jointMatrices`). Everything happens in one frame; **no new render pass, no graph change** — physics is a CPU producer into `TransformComponent`, consumed by the existing world-transform recompose.
- **Play uses a throwaway scene duplicate** (`scene_edit_play.cpp:75-106` `enterPlay`: `sceneToJson`/`sceneFromJson` into `ctx.playScene`; `:143-163` `stopPlay`: `ctx.playScene.reset()`). "The discard *is* the restore." A `PhysicsWorld` built against the play duplicate's entities dies with it on stop; **no manual reset of authored transforms** — they were never written.
- **The generic `add-component`/`set-component` commands already drive arbitrary registered components** (`control_commands_scene.cpp:282`/`:330`). So a box and a floor can be assembled over the wire with no physics-specific command. The keep-scriptable rule still wants a physics-specific command — this phase adds a small **`physics-state`** read command (see §6); the richer `raycast`/`set-rigidbody` verbs are later phases.
- **e2e harness shape** (`tests/e2e/rig-query.test.ts`): `Engine.boot({...})` → `engine.call<T>(cmd, params)` → `engine.settle()` → assert, with a final `engine.validationErrors()` toEqual `[]` test. `SAFFRON_AUTO_EMPTY_PROJECT: "1"` boots an empty project. This is the template for `tests/e2e/physics-falling-box.test.ts`.

## Work

### 1. Declare `RigidbodyComponent` + `ColliderComponent` in `Saffron.Scene`

In `scene.cppm`, near the other renderable/physics components (the `BonePhysics` block at `:153` is the natural neighbour), add both structs. They live in `Saffron.Scene` so they serialize and register through the recipe; `Saffron.Physics` only *consumes* them.

```cpp
/// A simulated body. Motion type decides how the solver treats it: Dynamic moves under forces,
/// Kinematic is script/animation-driven (infinite mass, pushes dynamics), Static never moves.
/// A ColliderComponent WITHOUT a RigidbodyComponent is an implicit Static body (floors/walls);
/// with one present, this motion type wins. Per-axis locks freeze a DOF on a Dynamic body.
struct RigidbodyComponent
{
    enum class Motion : u8
    {
        Static,
        Kinematic,
        Dynamic
    } motion = Motion::Dynamic;
    f32 mass = 1.0f;             // kg; ignored for Static/Kinematic
    f32 linearDamping = 0.05f;   // per-second velocity decay
    f32 angularDamping = 0.05f;
    f32 gravityFactor = 1.0f;    // 0 = float, 1 = full gravity
    glm::bvec3 lockPosition{ false };  // freeze X/Y/Z translation
    glm::bvec3 lockRotation{ false };  // freeze X/Y/Z rotation
    i32 collisionLayer = 0;            // index into the (Phase 4) layer table; 0 = the default moving layer
};

/// The collision geometry for an entity. Box-only this phase (Sphere/Capsule/ConvexHull/Mesh
/// are Phase 3). Dimensions are interpreted per-shape; auto-fit to the entity mesh AABB on add,
/// editable after. `offset` places the shape in the body's local space (Jolt RotatedTranslatedShape).
struct PhysicsMaterial
{
    f32 friction = 0.5f;     // 0 = ice, 1 = rubber
    f32 restitution = 0.0f;  // bounciness 0..1
};

struct ColliderComponent
{
    enum class Shape : u8
    {
        Box,
        Sphere,
        Capsule,
        ConvexHull,
        Mesh
    } shape = Shape::Box;
    glm::vec3 halfExtents{ 0.5f };  // Box: half-size; (radius/height for other shapes — Phase 3)
    Uuid sourceMesh;                // ConvexHull/Mesh cook source; 0 = none (Box ignores it)
    glm::vec3 offset{ 0.0f };       // local-space shape centre offset
    PhysicsMaterial material;
    bool isSensor = false;          // trigger volume: reports overlaps, no contact response (Phase 4)
};
```

Add the four forward declarations beside the existing component serde decls (`scene.cppm:978`/`:1004` region):

```cpp
auto rigidbodyComponentToJson(const RigidbodyComponent& c) -> nlohmann::json;
auto rigidbodyComponentFromJson(RigidbodyComponent& c, const nlohmann::json& j) -> Result<void>;
auto colliderComponentToJson(const ColliderComponent& c) -> nlohmann::json;
auto colliderComponentFromJson(ColliderComponent& c, const nlohmann::json& j) -> Result<void>;
```

> `glm::bvec3` serializes as three bools; `glm/glm.hpp` is already in the `scene.cppm` GMF (`:6`). The `enum class … : u8` style and the in-struct default initializers match `BonePhysics` (`scene.cppm:157`) and `AnimationPlayerComponent::Wrap` (`scene.cppm:96`).

### 2. Write the serde bodies in `gen.ts`

Add `rigidbodyComponentToJson`/`FromJson` and `colliderComponentToJson`/`FromJson` to the `emitSceneSerde()` template literal in `tools/gen-control-dto/gen.ts` (after the `bonePhysicsComponent*` block, `gen.ts:2860`). Mirror that block's structure: a local `motionName`/`shapeName` switch + a `*FromName` lambda for the two enums, `vec3ToJson`/`vec3FromJson` for the vectors, `jsonF32Or`/`jsonBoolOr` for scalars, and `WireUuid`/the `u64FromJson` helper (`gen.ts:2344`) for `sourceMesh` (ids cross the wire as **decimal strings**, per the control AGENTS rule — never JSON numbers). The `glm::bvec3` locks serialize as a 3-bool object `{x,y,z}`.

```cpp
auto rigidbodyComponentToJson(const RigidbodyComponent& c) -> nlohmann::json
{
    auto motionName = [](RigidbodyComponent::Motion m) -> const char*
    {
        switch (m)
        {
            case RigidbodyComponent::Motion::Static: return "static";
            case RigidbodyComponent::Motion::Kinematic: return "kinematic";
            case RigidbodyComponent::Motion::Dynamic: return "dynamic";
        }
        return "dynamic";
    };
    return nlohmann::json{ { "motion", motionName(c.motion) },
                          { "mass", c.mass },
                          { "linearDamping", c.linearDamping },
                          { "angularDamping", c.angularDamping },
                          { "gravityFactor", c.gravityFactor },
                          { "lockPosition", bvec3ToJson(c.lockPosition) },
                          { "lockRotation", bvec3ToJson(c.lockRotation) },
                          { "collisionLayer", c.collisionLayer } };
}
```

(`bvec3ToJson`/`bvec3FromJson` are small new helpers in the serde namespace, beside the existing `vec3ToJson`.) The collider serde follows the same shape: `shape` string, `halfExtents` vec3, `sourceMesh` decimal-string uuid, `offset` vec3, a nested `material` object `{friction,restitution}`, and `isSensor` bool. Then run `bun run tools/gen-control-dto/gen.ts` and commit all five generated outputs (the scene serde, the C++ DTO serde, `se-types.ts`, OpenRPC, manifest). The editor inspector picks up both components automatically from the regenerated `se-types.ts` (the generic protocol-driven `fieldRenderer`).

### 3. Register both — with the auto-fit `onAdd` hook

In `registerBuiltinComponents` (`scene_edit_components.cpp`, after the `BonePhysicsComponent` registration at `:121`):

```cpp
registerComponent<RigidbodyComponent>(
    reg, "Rigidbody", [](Scene&, Entity) {}, rigidbodyComponentToJson, rigidbodyComponentFromJson, true);

// Auto-fit the box to the entity's mesh AABB on add (editable after in the inspector).
// A collider with no Rigidbody is an implicit static body — floors are this one component.
registerComponent<ColliderComponent>(
    reg, "Collider",
    [](Scene& scene, Entity e) { fitColliderToMesh(scene, e); },
    colliderComponentToJson, colliderComponentFromJson, true);
```

`fitColliderToMesh(Scene&, Entity)` is a small new helper (in `Saffron.Scene` or `Saffron.SceneEdit`, wherever the mesh-asset AABB is reachable — the `AssetCatalog`/mesh bounds): look up the entity's `MeshComponent`/`SkinnedMeshComponent` mesh, read its local-space AABB, set `halfExtents = 0.5 * (max - min)` and `offset = 0.5 * (max + min)` (the centre). With no mesh, leave the `0.5` default. This makes "add a Collider" produce a correctly-sized box with no manual entry, per the locked auto-fit decision.

### 4. The `PhysicsWorld` body-creation pass (on Edit→Playing)

Phase 1 created the `onPlayStateChanged` subscription that builds the empty `PhysicsWorld` on `Edit→Playing` and tears it down on `Playing→Edit` (mirroring the script VM hook at `host.cppm:694-718`). Phase 2 fills the **build** step with a walk of the play scene that turns components into Jolt bodies. In `Saffron.Physics` (the `.cpp` impl unit — the **only** TU that includes `<Jolt/...>`):

```cpp
/// Walk `scene` for every ColliderComponent (+ optional RigidbodyComponent), create a Jolt body
/// per entity, and record the entity<->BodyID mapping. A collider without a rigidbody becomes a
/// Static body; with one, its Motion maps to EMotionType {Static,Kinematic,Dynamic}.
void populatePhysicsWorld(PhysicsWorld& world, Scene& scene);

/// Step the world by `dt` using a fixed-step accumulator (PlayFixedStep, PlayMaxDelta-clamped),
/// then write each Dynamic body's world transform back into its entity TransformComponent.
void stepPhysics(PhysicsWorld& world, Scene& scene, f32 dt);
```

`populatePhysicsWorld` (this phase, Box only):
- `forEach<ColliderComponent>(scene, …)`. Read the entity's `worldTranslation`/`worldRotation` (`scene.cppm:752`/`:758`) for the body's initial pose (the play scene's `updateWorldTransforms` has run, so the caches are warm — if not, `worldMatrix` composes on a miss, `scene.cppm:743`).
- Build a Jolt `BoxShapeSettings(Vec3(halfExtents))` → `.Create()` → `ShapeResult` → `RefConst<Shape>`; if `offset != 0`, wrap in `RotatedTranslatedShape`. Check the `ShapeResult` (`std::expected` idiom isn't Jolt's — use `result.IsValid()` and log on error).
- Motion: `RigidbodyComponent::Motion` → `JPH::EMotionType`; **collider-alone defaults to `EMotionType::Static`**. Fill `BodyCreationSettings` (shape, position, rotation, motion type, `mIsSensor` from the collider, `mFriction`/`mRestitution` from the material, mass/damping/gravityFactor from the rigidbody for Dynamic). Position/rotation locks → `mAllowedDOFs` on the body creation settings.
- `bodyInterface.CreateAndAddBody(settings, activation)` → `BodyID`; store both `entity → BodyID` and `BodyID → entity` in the maps on `PhysicsWorld`.

`stepPhysics` (the step + write-back):
- Accumulate `dt` (already `PlayMaxDelta`-clamped by `tickPlay`, `scene_edit_play.cpp:196`) into a `f32 accumulator` on `PhysicsWorld`; while `accumulator >= PlayFixedStep`, call `physicsSystem.Update(PlayFixedStep, /*collisionSteps*/1, tempAllocator, jobSystem)` and subtract. Fixed substeps keep the sim deterministic and decoupled from frame rate (and honour the Phase-1 `CROSS_PLATFORM_DETERMINISTIC` choice). One `Update` per substep.
- After stepping, write back: for each `Dynamic` body, read `bodyInterface.GetPositionAndRotation(id, pos, rot)`, convert the world pose into the entity's **local** `TransformComponent` (this phase: bodies are roots, so world == local; if the entity has a parent, factor out the parent world matrix — but keep Phase 2 to root entities and note the parented case as Phase-3+ follow-up). Set `translation` from `pos` and `rotation = glm::eulerAngles(rot)` (the component stores Euler XYZ radians, `scene.cppm:45`). `updateWorldTransforms` (run later in `renderScene`) recomposes the cached world matrix from the written local — physics never writes `WorldTransformComponent`.

### 5. Compose `simTick` = step physics, then scripts (the order this phase establishes)

In the Host's `simTick` lambda (`host.cppm:719`), prepend the physics step before the existing script tick. **Physics steps first, then scripts** — so a script that reads a body's post-step transform or applies an impulse sees this frame's settled physics, matching the "animation before scripts so a script can still override a bone" precedent (`host.cppm:882-887`). State the order here because it is the contract every later physics/script interaction depends on:

```cpp
state->editor->simTick = [state](se::Scene& scene, se::f32 dt)
{
    if (state->physicsActive)
    {
        se::stepPhysics(state->physics, scene, dt);  // fixed-accumulator step + dynamic write-back
    }
    if (!state->scriptVmActive)
    {
        return;
    }
    // … existing tickScripts(...) body unchanged …
};
```

`populatePhysicsWorld(state->physics, se::activeScene(*state->editor))` is called from the `onPlayStateChanged` subscription on the `Edit→Playing` branch (beside `startScripts`, `host.cppm:701`); the `Playing→Edit` branch tears the world's bodies down (Phase 1 owns the Jolt-system teardown; this phase clears the body maps + removes bodies via `bodyInterface.RemoveBody`/`DestroyBody`). `state->physicsActive` is a `HostState` bool mirroring `scriptVmActive` (`host.cppm:56`). Animation still runs before `tickPlay` (`host.cppm:887`), so the per-frame order is **animation → (physics → scripts) → renderScene**, all one frame.

### 6. A read-only `physics-state` control command (keep-scriptable)

Add one command so the running physics is inspectable from `se` and the e2e harness, per the keep-scriptable rule. Follow the control authoring workflow (`engine/source/saffron/control/AGENTS.md`): declare `PhysicsStateResult` in `control_dto.cppm`, add it + a fixture to `gen.ts`, register in `control_commands_scene.cpp` (or a new `control_commands_physics.cpp` if it grows — for one command, `_scene.cpp` is fine):

```cpp
struct PhysicsStateResult
{
    bool active = false;   // a PhysicsWorld exists (Playing/Paused)
    i32 bodyCount = 0;     // bodies in the world
    i32 dynamicCount = 0;  // of those, Dynamic
};
```

`registerCommand<EmptyParams, PhysicsStateResult>(reg, "physics-state", "physics world summary", …)` reads counts off `state->physics` (reached through the `EngineContext`/control handles the way the script-status command does, `control_commands_scene.cpp:1007`). The richer `raycast` and `set-rigidbody` (impulse/velocity) verbs are deferred to later phases (raycast lands with the query phase). Run `bun run tools/gen-control-dto/gen.ts` and commit the five outputs; add a `se` verb formatter.

### 7. The falling-box e2e (this phase's gate)

New `tests/e2e/physics-falling-box.test.ts`, mirroring `rig-query.test.ts`'s structure. Assemble the scene over the wire from an empty project (`SAFFRON_AUTO_EMPTY_PROJECT: "1"`):
- Create a **floor** entity, `add-component Collider`, `set-component` it to a thin wide box at the origin (no Rigidbody → implicit Static).
- Create a **box** entity at e.g. `y = 5`, `add-component Collider` (auto-fits) + `add-component Rigidbody` (defaults to Dynamic).
- `enter-play`; loop `step` (the `StepParams` command, `control_commands_scene.cpp:978`) a few hundred fixed ticks (or `play` + `settle` for a wall-clock window), polling `get-world-transform` (`control_commands_scene.cpp:758`, `WorldTransformResult`) on the box.
- Assert the box Y **decreases** early (it falls), then **stops decreasing** and **settles** at ≈ `floorTopY + boxHalfExtent` within a tolerance, and **does not tunnel** below the floor. Assert `physics-state` reports `active`, `bodyCount == 2`, `dynamicCount == 1`. Final test: `engine.validationErrors()` toEqual `[]`.

Stepping via the `Paused`+`step` path forces `dt = PlayFixedStep` (`scene_edit_play.cpp:194`), so the test is deterministic frame-for-frame — the cleanest assertion target.

## Validation (done criteria)

- `make engine` green: `Saffron.Physics` compiles with the new body-creation/step code; `Saffron.Scene` compiles with the two new components; the generated serde compiles.
- `make prepare-for-commit` clean (clang-format + clang-tidy) over the new/changed files.
- `bun run check` clean: the regenerated `se-types.ts` typechecks; `RigidbodyComponent`/`ColliderComponent` appear in the editor inspector via the generic field renderer with no hand-written editor code.
- `make e2e`: `tests/e2e/physics-falling-box.test.ts` passes — the box falls, settles on the static floor at the expected rest height, doesn't tunnel, and the run is validation-clean. The contract test (`bun run tools/gen-control-dto/gen.ts` + `git diff --exit-code`) stays green with the new `physics-state` fixture.
- `docs/`: add `docs/content/explanations/physics/rigidbody-and-collider.md` — the split-component model (Rigidbody = motion/mass/damping/locks; Collider = shape/material/sensor), the collider-alone-is-static rule, auto-fit on add, and the fixed-step-accumulator + write-back loop running inside `simTick` before scripts. Add its row to the physics hub `_index.md` (Phase 1 created the hub).

## Notes / gotchas

- **Write the local `TransformComponent`, never `WorldTransformComponent`.** The cached world matrix is overwritten every frame by `updateWorldTransforms` (`scene.cppm:774`, runs in `renderScene`); writing it directly is wasted and gets clobbered. Convert the body's world pose to the entity's local transform on write-back. Phase 2 scopes bodies to **root** entities (world == local) and leaves the parented-body local-rebase for a later phase — note this in the docs page rather than half-implementing it.
- **Euler round-trip.** `TransformComponent.rotation` is Euler XYZ radians (`scene.cppm:45`); a Jolt body is a quaternion. Convert with `glm::quat`↔`glm::eulerAngles`. Gimbal degeneracy is acceptable for v1 settling; if a tumbling box jitters at rest, that is the conversion, not the solver — flag it but don't redesign the component (the editor edits Euler directly, so the component stays Euler).
- **`stopPlay` discards the play scene; the `PhysicsWorld` bodies die with it.** Tear bodies down on `Playing→Edit` (remove from the body interface, clear the maps) so a second `enterPlay` repopulates cleanly. There is **no** authored-transform reset — authored transforms were never written, per "the discard is the restore" (`scene_edit_play.cpp:143-163`). Mirror exactly the script VM start/stop lifecycle (`host.cppm:694-718`).
- **Determinism touchpoint.** Step at the **fixed** `PlayFixedStep` substep, not the raw frame `dt`; the accumulator decouples sim from frame rate. This is what makes the e2e reproducible and honours the Phase-1 `CROSS_PLATFORM_DETERMINISTIC` decision — do not call `physicsSystem.Update` with a variable `dt`.
- **`<Jolt/...>` stays in one TU.** Only the `Saffron.Physics` `.cpp` impl unit includes Jolt headers (the Phase-1 module-shape rule); the `:Types` partition and `PhysicsWorld` handle are Jolt-free POD/opaque so the rest of the engine (and `host.cppm`) never sees a Jolt type. Keep `populatePhysicsWorld`/`stepPhysics` as the seam.
- **Box only.** Resist adding sphere/capsule here even though Jolt makes it one line each — Phase 3 introduces the shape switch + the hull/mesh cook from `.smesh` vertices, and the e2e for those. This phase's job is the loop, proven by one shape.
