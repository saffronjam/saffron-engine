# Phase 3 — Collision shapes (sphere/capsule/convex/mesh) + PhysicsMaterial + auto-fit

**Status:** COMPLETED

## Goal

Fill out the `ColliderComponent::Shape` enum Phase 2 introduced as `Box`-only — add **Sphere**, **Capsule**, **ConvexHull**, and **Mesh** — so a body can carry the right collision geometry for its mesh. Two shapes are analytic (sphere/capsule, sized from the entity's mesh AABB); two are **cooked from the entity's `.smesh` vertex data** (a convex hull for dynamic bodies, a full triangle mesh for static/kinematic ones). Wire **`PhysicsMaterial`** (friction/restitution, declared in Phase 2) through to Jolt body creation as observable behavior — a high-friction box stops sliding, a high-restitution one bounces. And make **auto-fit-to-AABB on add** real for every shape: adding a `ColliderComponent` sizes the shape to the entity's mesh bounds (the locked design decision), editable in the inspector after, with a purpose-built `fit-collider` command for re-fitting on demand.

This phase does **not** add a second component — it extends Phase 2's `ColliderComponent`/`PhysicsMaterial`. The body-creation/step/write-back loop, the `simTick` composition, and the `PhysicsWorld` lifecycle all exist from Phase 2 and are untouched here except for the shape-build branch inside `populatePhysicsWorld`. Layers/triggers, the character controller, kinematic bone-following, and ragdoll are later phases.

## What exists to build on

- **The `ColliderComponent::Shape` enum already lists all five values** — Phase 2 declared `{ Box, Sphere, Capsule, ConvexHull, Mesh }` in `scene.cppm` even though only `Box` was wired, with `halfExtents` (the Box half-size, reused per-shape this phase), a `Uuid sourceMesh` cook source, an `offset`, a nested `PhysicsMaterial { friction, restitution }`, and `isSensor`. The serde (`colliderComponentToJson`/`FromJson` in `emitSceneSerde()`, `gen.ts`) already round-trips the `shape` string and the nested `material` object. So **no new component, no new field, no `gen.ts` serde edit, no `bun run check` regeneration** is required just to interpret the existing enum values — this phase is engine-side shape construction plus the auto-fit helper plus one re-fit command.
- **`GpuMesh` carries a local-space AABB** — `boundsMin`/`boundsMax` (`renderer_types.cppm:230-231`), filled at upload by sweeping the vertices (`renderer_drawlist.cpp:145-150`). This is the **cheap auto-fit source**: it is already computed for every mesh, and the thumbnail framing reads exactly these fields (`renderer_thumbnail.cpp:212-213`) — so sphere radius / capsule dims / box half-extents all derive from `(boundsMax - boundsMin)` with no extra GPU work and no vertex re-read.
- **CPU vertices are NOT retained on `GpuMesh`** — it holds only Vulkan buffer handles + the AABB, no `std::vector<Vertex>` (`renderer_types.cppm:220-232`). So **ConvexHull/Mesh cooking must re-read the mesh from disk**, not from `GpuMesh`. The model is `loadAnimationClipAsset` (`assets.cppm:2899`): a catalog lookup + a bytes read decoded into a CPU type. The bytes→`Mesh` decoder is `loadMeshFromBytes` (`assets.cppm:2684`, used inside `loadMeshAsset`'s GPU upload path). `Mesh.vertices` / `Vertex.position` are at `geometry.cppm:54-59` / `:38`; `Mesh.indices` is the triangle list. A new **`loadMeshCpuAsset(AssetServer&, Uuid) -> Result<Mesh>`** beside `loadMeshAsset` (`assets.cppm:4734`) is the clean addition — catalog lookup (`entry->type == AssetType::Mesh`) + `loadProjectAssetBytes` + `loadMeshFromBytes`, no GPU upload, no cache entry (cooking is a one-shot on Edit→Playing).
- **The cook source resolves through the entity's mesh** — `MeshComponent.mesh` is the `Uuid` (`scene.cppm:180`); `loadMeshAsset` (`assets.cppm:4734`) is the existing resolve-to-GPU path the new CPU sibling mirrors. Auto-fit reads `MeshComponent` (or `SkinnedMeshComponent`) to find the mesh, then the GPU mesh's AABB; cooking reads the same mesh id through `loadMeshCpuAsset`. The collider's `sourceMesh` defaults to that mesh on add (so re-fit and cook stay self-contained).
- **Auto-fit hooks the add-component path** — the generic `add-component` command calls `row->addDefault(...)` (`control_commands_scene.cpp:300`), which routes through `registerComponent`'s default-construct + `onAdd`. Phase 2 registered `ColliderComponent` with an `onAdd` lambda that calls `fitColliderToMesh(scene, e)` (the `scene_edit_components.cpp` registration). This phase makes that helper shape-aware (size the *right* shape, not just a box). Field edits after add already go through `set-component-field` (`control_commands_scene.cpp:1196`), so manual override of any fitted dimension is covered for free; the **new `fit-collider` command** is the explicit re-fit verb (keep-scriptable).
- **`transformMatrix` is `T·R·S`** (`scene.cppm:328-334`) — auto-fit works in the mesh's **local space** (the raw AABB), and the body transform carries the entity scale, exactly as in Phase 2's write-back. Do not bake scale into the cooked points or the fitted extents; Jolt scales the shape via the body transform / a `ScaledShape` if needed. Keeping fit in local space means a re-fit after a transform change stays stable.
- **Shape construction lives in the one Jolt TU** — `populatePhysicsWorld` (Phase 2, in the `Saffron.Physics` `.cpp` impl unit — the **only** TU that includes `<Jolt/...>`) already builds a `BoxShapeSettings` and wraps it in a `RotatedTranslatedShape` when `offset != 0`. This phase replaces the single Box branch with a `switch (collider.shape)`; everything else in that function (motion mapping, `BodyCreationSettings`, the entity↔`BodyID` maps) is unchanged.
- **e2e harness shape** (`tests/e2e/rig-preview.test.ts` / `rig-query.test.ts`): `Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" })` → `engine.call<T>(cmd, params)` → `engine.settle()` → assert, with a closing `engine.validationErrors()` toEqual `[]` test. Phase 2's `tests/e2e/physics-falling-box.test.ts` is the directly adjacent template; this phase adds `tests/e2e/physics-shapes.test.ts`.

## Work

### 1. Make `fitColliderToMesh` shape-aware (auto-fit, the locked decision)

Phase 2's `fitColliderToMesh(Scene&, Entity)` set a box from the AABB. Generalize it so adding **any** shape produces a correctly-sized collider with no manual entry. It reads the entity's mesh AABB (the `GpuMesh.boundsMin`/`boundsMax` the thumbnail path uses, `renderer_thumbnail.cpp:212-213`), then fits whichever `shape` the collider currently holds. It runs from the registered `onAdd` (`scene_edit_components.cpp`) and from the new `fit-collider` command (§4).

```cpp
/// Size the collider's shape to the entity's mesh local-space AABB, in mesh-local space.
/// Auto-runs on add (the locked decision) and on the fit-collider command. With no mesh,
/// the fields keep their defaults. Never bakes the entity transform's scale in — the body
/// transform carries scale, so a re-fit after scaling stays consistent.
void fitColliderToMesh(AssetServer& assets, Renderer& renderer, Scene& scene, Entity e);
```

The per-shape rule from the AABB `min`/`max`, `c = 0.5*(min+max)`, `h = 0.5*(max-min)`:
- **Box:** `halfExtents = h`, `offset = c` (Phase 2's behavior, kept).
- **Sphere:** radius = `max(h.x, h.y, h.z)` (the bounding sphere of the box — never smaller than the mesh), stored in `halfExtents.x`; `offset = c`. (Reuse `halfExtents.x` as the radius slot rather than adding a field, per "extend, don't add a component/field".)
- **Capsule:** the long axis is the largest `h` component; radius = the larger of the other two; half-height (the cylinder part, excluding the caps) = `max(0, longAxisH - radius)`. Store radius in `halfExtents.x`, half-height in `halfExtents.y`; `offset = c`. Default the capsule **Y-up** (Jolt's `CapsuleShape` is Y-axis); a non-Y dominant axis is fitted Y-up and noted as a gotcha (per-axis capsule orientation is a later refinement).
- **ConvexHull / Mesh:** `halfExtents`/`offset` are informational only (the shape is the cooked geometry); set `offset = c` so the inspector still shows the mesh centre, set `sourceMesh = MeshComponent.mesh` (the cook source), and leave `halfExtents = h` as a fallback box if cooking fails. **Cooking does not happen here** — fit is cheap/synchronous and Edit-time; the actual hull/triangle cook happens at body creation (§3) so it never blocks the inspector.

`fitColliderToMesh` needs the `AssetServer`+`Renderer` to reach the `GpuMesh` AABB (`loadMeshAsset`, `assets.cppm:4734`). Phase 2 placed the helper where the mesh bounds are reachable; if the `onAdd` lambda cannot see those handles, route the fit through the same `EngineContext` the `add-component` command holds (the command can call `fitColliderToMesh` after `row->addDefault`, since `addDefault`'s `onAdd` ran first to default-construct — but the AABB-aware fit needs the asset handles the command has). Keep it one helper, called from both the registration `onAdd` (best-effort, no-op if assets unreachable) and the command (full fit).

### 2. Add `loadMeshCpuAsset` to `Saffron.Assets` (the cook source)

`GpuMesh` does not keep CPU vertices (`renderer_types.cppm:220-232`), so hull/mesh cooking re-reads the baked `.smesh`. Add a CPU-only sibling of `loadMeshAsset`, modeled on `loadAnimationClipAsset` (`assets.cppm:2899`) for the catalog-lookup-then-decode shape and `loadMeshFromBytes` (`assets.cppm:2684`) for the decode:

```cpp
/// Decode an entity's baked .smesh to a CPU Mesh (positions + indices) for physics cooking.
/// Catalog lookup + bytes read + loadMeshFromBytes; no GPU upload, no cache entry — cooking is a
/// one-shot at Edit->Playing. The negative-cache rule does not apply (this is not the draw path).
auto loadMeshCpuAsset(AssetServer& assets, Uuid id) -> Result<Mesh>;
```

It returns the full `Mesh` (`geometry.cppm:54-59`); the cook in §3 reads `Mesh.vertices[i].position` (`geometry.cppm:38`) for the hull point cloud and `Mesh.indices` (triangle list) for the mesh shape. This lives in `Saffron.Assets` (it owns `.smesh` decode + the catalog), is consumed by `Saffron.Physics` through the seam the Host injects, and is **not** registered into `meshRefByUuid` — a cook is a one-time read, not a per-frame draw resource.

### 3. The shape switch in `populatePhysicsWorld` (the Jolt TU)

Replace Phase 2's single Box branch inside `populatePhysicsWorld` (in the `Saffron.Physics` `.cpp` impl — the only `<Jolt/...>` TU) with a shape switch that builds the right `XShapeSettings` → `.Create()` → `ShapeResult` → `RefConst<Shape>`, then wraps in `RotatedTranslatedShape` for the `offset` (Phase 2's wrap, kept). Cooking re-reads the mesh via the injected `loadMeshCpuAsset` seam.

```cpp
/// Build the Jolt collision shape for a collider. Analytic shapes size from the (auto-fitted)
/// component fields; ConvexHull/Mesh cook from the entity's .smesh vertices via loadMeshCpuAsset.
/// Returns a ref-counted shape, offset-wrapped when collider.offset != 0. MeshShape is rejected on
/// a Dynamic body (Jolt restriction) with a loud failure — the caller falls back / logs and skips.
auto buildColliderShape(const ColliderComponent& c, RigidbodyComponent::Motion motion,
                        const MeshCookSource& cook) -> JPH::ShapeRefC;  // null on failure
```

- **Box:** `BoxShapeSettings(Vec3(halfExtents))` (Phase 2).
- **Sphere:** `SphereShapeSettings(halfExtents.x)` (radius from the fitted slot).
- **Capsule:** `CapsuleShapeSettings(halfHeight = halfExtents.y, radius = halfExtents.x)` (Jolt's capsule is Y-axis, half-height excludes the caps — matches §1's fit).
- **ConvexHull:** `loadMeshCpuAsset(collider.sourceMesh)` → build a `JPH::Array<Vec3>` from `Mesh.vertices[*].position` → `ConvexHullShapeSettings(points)`. Valid on **Dynamic** bodies. Deduplicate/iterate the point cloud in a **stable, index order** (never a hash-set traversal) so the cook is reproducible run-to-run — see the determinism note.
- **Mesh:** `loadMeshCpuAsset(collider.sourceMesh)` → build `JPH::TriangleList` (or `VertexList` + `IndexedTriangleList`) from `Mesh.vertices` + `Mesh.indices` → `MeshShapeSettings`. **Static/Kinematic only.** If `motion == Dynamic`, this is a hard, loud failure: `logError` a clear message (`"Mesh collider on a Dynamic body (entity …): Jolt MeshShape is Static/Kinematic only — use ConvexHull for dynamic, or make the body Static/Kinematic"`), return a null shape, and **skip the body** (do not silently downgrade to a box — make the misconfiguration visible). This is the one shape×motion constraint of the phase; surface it everywhere a collider is built.

Every `ShapeResult` is checked with `result.IsValid()` (Jolt does not use `std::expected`); on a cook failure log and return null (the body is skipped, matching the negative-cache philosophy — a broken collider does not abort the world). `PhysicsMaterial` is wired here too: copy `collider.material.friction` → `BodyCreationSettings::mFriction` and `collider.material.restitution` → `mRestitution` (Phase 2 stubbed these from the defaults; this phase makes them load-bearing — they are what the §5 e2e's sliding/bouncing asserts observe). `motion` comes from the optional `RigidbodyComponent` exactly as Phase 2 (collider-alone ⇒ Static).

The `cook`/`MeshCookSource` is a small Jolt-free callable (`std::function<Result<Mesh>(Uuid)>`) the Host binds to `loadMeshCpuAsset` and threads into `populatePhysicsWorld`, keeping `<Jolt/...>` out of `Saffron.Assets` and `loadMeshCpuAsset` out of the Jolt TU's includes.

### 4. A `fit-collider` re-fit command (keep-scriptable)

Auto-fit-on-add covers the common case; re-fitting after a mesh swap or a manual mistake needs a verb (and the keep-scriptable rule wants the state drivable). Add one command following the control workflow (`engine/source/saffron/control/AGENTS.md`): declare params/result in `control_dto.cppm`, add it + a fixture to `gen.ts`, register in `control_commands_scene.cpp` (beside the component commands), run `bun run tools/gen-control-dto/gen.ts`, commit the five outputs, add an `se` verb.

```cpp
struct FitColliderParams { EntitySelector entity; };  // optional: a shape override to re-fit as
struct FitColliderResult
{
    WireUuid entity;
    std::string shape;        // the shape that was fitted
    glm::vec3 halfExtents;    // the resulting dimensions (radius/half-height packed for sphere/capsule)
    glm::vec3 offset;         // the fitted centre
};
```

The handler resolves the entity (`resolveEntity`), errors if it has no `ColliderComponent`, calls `fitColliderToMesh(...)` with the command's `EngineContext` asset handles (the full-fit path §1 mentions), bumps `sceneVersion`, and returns the fitted dims. This is purely a re-fit — **not** the only way to add a collider (add-component already auto-fits), and **not** a replacement for `set-component-field` (manual edits still go there). It exists so a script/test can re-fit deterministically after changing `shape` or the mesh.

### 5. The shapes e2e (this phase's gate)

New `tests/e2e/physics-shapes.test.ts`, mirroring `rig-query.test.ts`'s structure and Phase 2's falling-box test. Boot an empty project (`SAFFRON_AUTO_EMPTY_PROJECT: "1"`), import a fixture mesh (reuse a simple e2e fixture so the AABB is known), and assemble scenes over the wire. Cover each new shape + the material behavior + auto-fit + the Mesh-on-Dynamic guard:

- **Auto-fit:** `create-entity` with a `MeshComponent`, `add-component Collider`, then read the collider back (via the inspector/`get-component` path or `fit-collider`'s result) and assert `halfExtents`/`offset` match the fixture's known AABB (no manual sizing was needed). Switch `shape` to `Sphere` via `set-component-field`, call `fit-collider`, assert the radius equals the AABB's max half-extent.
- **Sphere & Capsule fall and settle** on a static floor (Phase 2's box-falls assertion shape: Y descends, then stops at ≈ `floorTopY + radius`, no tunnelling).
- **ConvexHull on a Dynamic body** cooks and settles (asserts the hull cook from the fixture `.smesh` succeeded — body count includes it, it comes to rest).
- **Mesh on a Static floor** works (a triangle-mesh floor a box falls onto); **Mesh on a Dynamic body is rejected** — set `shape = Mesh` on a Dynamic-rigidbody entity, `enter-play`, and assert the body is **skipped** (the dynamic-body count excludes it) and a validation/error log carries the loud message (or the body simply never appears — assert the count, and that the run is otherwise clean).
- **PhysicsMaterial behavior:** a high-`restitution` sphere dropped on the floor rebounds (its Y rises after first contact across the step loop) where a `restitution = 0` one does not; a high-`friction` box on a slight slope/with initial lateral velocity stops where a low-friction one keeps sliding. Step via the `Paused`+`step` fixed-`dt` path (Phase 2) so the assertions are frame-deterministic.
- Closing test: `engine.validationErrors()` toEqual `[]` (the Mesh-on-Dynamic *expected* error is logged through a channel the harness does not count as a validation error — assert that separation explicitly, or scope the rejection assertion to body-count rather than the validation log).

### 6. Docs

Add `docs/content/explanations/physics/collision-shapes.md` (the physics hub `_index.md` was created by Phase 1; Phase 2 added the rigidbody/collider row). Cover: the five shapes and when each applies (sphere/capsule analytic; ConvexHull for dynamic, Mesh for static/kinematic — and **why** Mesh can't be dynamic, the Jolt restriction surfaced as a loud failure); **auto-fit-to-AABB on add** as the locked design (adding a collider sizes it to the mesh, editable after, re-fit via `fit-collider` — not a manual-only or button-only flow); the cook source (`.smesh` re-read via `loadMeshCpuAsset` because `GpuMesh` keeps no CPU verts) and its deterministic ordering; `PhysicsMaterial` (friction/restitution) and the visible sliding/bouncing it produces; `RotatedTranslatedShape` for the local `offset`. Add the row to the hub `_index.md`. Use the `What | File | Symbols` pointer table (`fitColliderToMesh`, `loadMeshCpuAsset`, `buildColliderShape`, `fit-collider`), KaTeX for the AABB→radius/half-height fit math, and run the prose through the humanizer pass.

## Validation (done criteria)

- `make engine` green: `Saffron.Physics` compiles the shape switch + the cook seam; `Saffron.Assets` compiles `loadMeshCpuAsset`; no new `<Jolt/...>` include leaks outside the one impl TU.
- `make prepare-for-commit` clean (clang-format + clang-tidy) over the new/changed files.
- `bun run check` clean: the regenerated `se-types.ts` typechecks with the new `fit-collider` command. (The `ColliderComponent` serde is **unchanged** — Phase 2 already serialized the full enum + nested material — so the only `gen.ts`/regeneration churn this phase is the new command's DTO + fixture; the contract test `bun run tools/gen-control-dto/gen.ts` + `git diff --exit-code` stays green.)
- `make e2e`: `tests/e2e/physics-shapes.test.ts` passes — auto-fit sizes each shape from the mesh AABB; sphere/capsule/convex-hull fall and settle; a Mesh floor catches a box; a Mesh collider on a Dynamic body is rejected (body skipped, loud log); friction stops sliding and restitution bounces; the run is validation-clean.
- `docs/`: `docs/content/explanations/physics/collision-shapes.md` added with its hub `_index.md` row.

## Notes / gotchas

- **`GpuMesh` keeps no CPU vertices — cook by re-reading the `.smesh`.** The renderer sweeps the verts once for the AABB and discards them (`renderer_drawlist.cpp:145-150`, `renderer_types.cppm:220-232`). Do not try to "save the verts on `GpuMesh`" to cook from — that bloats every GPU mesh for a one-shot Edit→Playing need. `loadMeshCpuAsset` (modeled on `loadAnimationClipAsset`, `assets.cppm:2899`) is the right cost: a single decode at body creation, no cache, no per-frame cost.
- **MeshShape is Static/Kinematic only — fail loudly, never silently downgrade.** Jolt's `MeshShape` cannot back a Dynamic body. When a user/script puts `shape = Mesh` on a Dynamic rigidbody, **skip the body and `logError`** a message that names the entity and the fix (use ConvexHull for dynamic). Silently substituting a box would hide a real authoring error and violate the no-compat-shim rule — there is one correct behavior, and it is the explicit failure. ConvexHull is the dynamic-capable cooked shape.
- **Auto-fit is the design, not a convenience.** Adding a `ColliderComponent` fits the shape to the mesh AABB through the registered `onAdd` — this is locked. Do not implement it as a separate must-press button or a manual-only flow. The `fit-collider` command exists *in addition* (re-fit on demand, per keep-scriptable), and `set-component-field` exists for manual overrides after the fit — three layers, with auto-fit as the default that just works.
- **Fit and cook in mesh-local space; the body transform carries scale.** The AABB is local (`renderer_thumbnail.cpp:212-213` reads it un-transformed); `transformMatrix` is `T·R·S` (`scene.cppm:328-334`) and the body's world transform already applies the entity scale (Phase 2's write-back). Baking scale into the fitted extents or the cooked points would double-apply it. A re-fit after the entity is scaled must reproduce the same local dims — keep it scale-free.
- **Deterministic cook ordering (honour `CROSS_PLATFORM_DETERMINISTIC`).** Phase 1 built Jolt with `CROSS_PLATFORM_DETERMINISTIC`; that determinism is only real if the *inputs* are stable. Feed the ConvexHull point cloud and the MeshShape triangle list in **mesh index order** (iterate `Mesh.vertices`/`Mesh.indices` linearly), never via an unordered set or a hash-keyed dedupe whose iteration order varies — the cooked shape, and therefore the simulation, must be byte-reproducible run-to-run. If you dedupe hull points, do it order-preserving.
- **Sphere/capsule pack their dims into `halfExtents`, no new fields.** Reusing `halfExtents.x` as radius and `.y` as capsule half-height (rather than adding `radius`/`height` fields) keeps the component single and the serde unchanged — extend the existing field's meaning per-shape rather than growing the struct. Document the packing in the docs page so the inspector's `halfExtents` reads correctly per shape (a later phase can add a shape-aware inspector label; the generic field renderer shows the raw vec3 for now).
- **Capsule axis.** Jolt's `CapsuleShape` is Y-axis; §1 fits Y-up regardless of the mesh's dominant axis. A capsule whose mesh is long on X/Z is fitted Y-up and may need a rotation in `RotatedTranslatedShape` — scope v1 to Y-up and **say so**; per-axis capsule orientation (rotate the offset wrap to align the dominant axis) is a small later refinement, not this phase's job.
- **`<Jolt/...>` stays in the one impl TU.** `buildColliderShape` and the cook seam keep Jolt types out of `Saffron.Assets` (`loadMeshCpuAsset` returns a plain `Mesh`) and out of `host.cppm` (the cook is a `std::function<Result<Mesh>(Uuid)>` the Host binds). The Phase-1/2 module-shape rule is unchanged: only the `Saffron.Physics` `.cpp` includes Jolt.
