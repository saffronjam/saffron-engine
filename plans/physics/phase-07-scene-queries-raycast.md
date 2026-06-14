# Phase 7 — Scene queries: raycast / shapecast + script API

**Status:** COMPLETED

## Goal

Make the live physics world *queryable*. Expose Jolt's `NarrowPhaseQuery` against the running world as
three surfaces, all reading the same C++ entry point:

- a **`raycast`** control command (`origin` + `dir` + `maxDist` → hit entity uuid / point / normal /
  distance), plus a basic **`shapecast`** (sphere sweep) for thicker probes;
- an **`se raycast`** verb that prints the structured hit;
- a **Lua query** (`se.raycast{...}`) gameplay scripts call inside `on_update`.

This is strictly **read-only against the world built in Phases 1–6**: it casts rays/shapes and reports
what they hit; it never spawns, moves, or wakes a body. It is the gameplay counterpart to the editor's
existing mesh-AABB `pick` command (`control_commands_scene.cpp:683`) — but it hits the *physics* shapes
(static colliders, dynamic rigidbodies, the character controller) at their simulated transforms, not the
render AABBs. Until this lands, a script can step the world but cannot ask "what is under the player's
feet?" — which is the prerequisite for the ground-probe locomotion the foot-IK phase deferred to physics.

## What exists to build on

- **The world + its entity↔BodyID maps exist (Phases 1–6).** `Saffron.Physics:Types` exports the opaque
  `PhysicsWorld` (wrapping Jolt `PhysicsSystem` + `BodyInterface` + the `TempAllocator`/`JobSystem` + the
  `entt::entity`↔`JoltBodyID` maps + the fixed-step accumulator). It is owned by `HostState` (a sibling of
  `se::AnimationRuntime animation` / `se::ScriptHost script`, `host.cppm:53-54`), built on the
  `Edit→Playing` edge and torn down on `→Edit` exactly like the script VM
  (`onPlayStateChanged.subscribe`, `host.cppm:694-718`), and stepped inside the `simTick` seam
  (`host.cppm:719`). A query reads this world; it does not create it.
- **`simTick` is the host-installed seam, not a `Saffron.Physics` symbol** (`scene_edit_context.cppm:224`
  — `std::function<void(Scene&, f32)> simTick`). The Host already composes animation→scripts there
  (`tickAnimation` at `host.cppm:887`, then `tickPlay`→`simTick` at `host.cppm:890`); Phase 6 inserts the
  physics step into that same closure. The query runs *between* steps, against the settled world.
- **A control command only sees `EngineContext`** — `window` / `renderer` / `sceneEdit` / `assets`
  (`command.cppm:29-35`), **not** `HostState`. So the world is not directly reachable from a command
  today. This is the one wiring decision (§4): the Host installs a non-owning `PhysicsWorld*` handle on
  `SceneEditContext` (the same place `simTick` lives), nulled on teardown — the command reads it through
  `ctx.sceneEdit`, never owns it. This mirrors how animation commands reach play state through
  `ctx.sceneEdit.animationVersion` rather than touching `AnimationRuntime`.
- **The query-command + structured-result recipe is established.** `pick`
  (`control_commands_scene.cpp:683-717`) is the template: take a small params DTO, run a cast against live
  state, return a `PickResult { bool hit; std::optional<WireUuid> id; std::optional<std::string> name;
  std::optional<PickKind> kind; }` (`control_dto.cppm:1181-1187`). `get-world-transform`
  (`control_commands_scene.cpp:758-774` → `WorldTransformResult`, `control_dto.cppm:1400-1404`) shows the
  vec3-returning result shape the e2e tests already read.
- **The CLI text-formatter recipe.** `printResult` in `tools/se/source/main.cpp:127` dispatches on `cmd`;
  the `pick` branch (`:369-381`) prints a one-line hit/no-hit. A `raycast` verb adds one branch keyed on
  `"raycast"`, falling through to the UTF-8 `dump` default otherwise (`:398`).
- **The Lua binding recipe.** `Saffron.Script` (`script.cppm`) binds the `se` namespace via LuaBridge3
  (`getGlobalNamespace(L).beginNamespace("se")...`, `script_runtime.cpp:385-447`). Free functions are
  bound as captured lambdas with the `ScriptHost&` in the capture (e.g. `get_entity_by_name`,
  `:407-425`); `se::ScriptEntity` is a bound class returned by value (`:387-395`). The host borrows the
  scene for the session (`host.currentScene`, `:449`). The script module imports only `{Core, Scene}`
  today — adding a physics query means the **Host wires the query in** (the only importer of both
  `Saffron.Physics` and `Saffron.Script`), exactly like §4's command seam, rather than `Saffron.Script`
  gaining a `Saffron.Physics` edge (which the DAG forbids — only Host imports the leaf game modules).
- **The e2e harness.** `tests/e2e/foot-ik.test.ts` and `tests/e2e/play.test.ts` boot a headless engine,
  `import-model` a fixture, `play`, `settle(ms)`, and assert over the control plane; `worldY` reads
  `get-world-transform`. A raycast test follows the same shape (drop a body onto a floor, cast down,
  assert the hit is the floor at roughly the contact height).

## Work

### 1. The query result type + the engine entry point (`Saffron.Physics`)

In `Saffron.Physics:Types`, a POD result the command, the CLI, and Lua all read — no Jolt types leak:

```cpp
/// One ray/shape query hit against the live physics world (world space).
struct PhysicsRayHit {
    bool hit = false;
    u64 entity = 0;          // the IdComponent uuid of the body owner (0 when the hit body has no entity)
    glm::vec3 point{0.0f};   // world-space contact point
    glm::vec3 normal{0.0f};  // world-space surface normal at the hit
    f32 distance = 0.0f;     // along the ray from origin, in dir units (== fraction * maxDist)
};
```

The single implementation (the only TU that includes `<Jolt/...>`), exported from `Saffron.Physics`:

```cpp
/// Cast a ray from `origin` along `dir` (need not be normalized) up to `maxDist`.
/// Returns the closest hit, mapping the hit BodyID back to its entity uuid via the
/// world's body<->entity map. `hit == false` when nothing is along the ray.
auto raycastWorld(const PhysicsWorld& world, glm::vec3 origin, glm::vec3 dir, f32 maxDist) -> PhysicsRayHit;

/// Sweep a sphere of `radius` from `origin` along `dir` up to `maxDist` (a thicker
/// probe — e.g. a ground check that tolerates edges). Same hit mapping as raycastWorld.
auto sphereCastWorld(const PhysicsWorld& world, glm::vec3 origin, glm::vec3 dir, f32 radius, f32 maxDist)
    -> PhysicsRayHit;
```

Implementation notes (the Jolt-spec points, all from the research):
- `raycastWorld` builds a `JPH::RRayCast{ origin, dir * maxDist }` and calls
  `world.system->GetNarrowPhaseQuery().CastRay(ray, RayCastResult&)` — the single-closest overload. On a
  hit, `result.mBodyID` → entity via the world's `BodyID→entity` map; the contact point is
  `ray.GetPointOnRay(result.mFraction)`; the **normal** comes from
  `bodyInterface.GetWorldSpaceSurfaceNormal(result.mBodyID, point)` (or the locked-body variant); the
  distance is `result.mFraction * maxDist`. Single precision (`DOUBLE_PRECISION OFF`, Phase 1) means
  `RVec3 == Vec3`, so no narrowing.
- `sphereCastWorld` uses `NarrowPhaseQuery::CastShape` with a `SphereShape(radius)` and a
  `RShapeCast` from the start transform along `dir * maxDist`; take the closest `ShapeCastResult`. Keep
  the surface returned by `mPenetrationAxis`/`mContactPointOn2` for the normal/point.
- A body that is not in the entity map (none should exist, but a defensive zero is cheaper than a crash)
  reports `entity == 0`; the caller treats 0 as "no owning entity," matching `ScriptErrorDto.entity`
  (`control_dto.cppm:435`).
- **Read-only and cross-platform-deterministic-safe:** a query touches no body state, so it does not
  perturb the bit-exact step (Phase 1's `CROSS_PLATFORM_DETERMINISTIC`). It must run *outside* an active
  `PhysicsSystem::Update` (i.e. between steps in `simTick`, or from a command on the main thread between
  frames) — never concurrently with the step's job graph.

### 2. The `raycast` / `shapecast` control commands + DTOs

In `control_dto.cppm`, params (field order = positional CLI order) and a result that reuses the hit shape:

```cpp
struct RaycastParams {
    Vec3 origin;
    Vec3 dir;                       // need not be normalized
    std::optional<f32> maxDist;     // default 1000
};

struct ShapecastParams {
    Vec3 origin;
    Vec3 dir;
    f32 radius;
    std::optional<f32> maxDist;     // default 1000
};

/// A physics query hit, surfaced over the wire (entity 0 when none / no hit).
struct RaycastResult {
    bool hit;
    WireUuid entity;
    Vec3 point;
    Vec3 normal;
    f32 distance;
};
```

Add the `dtoToJson(const RaycastResult&)` + `parseDto(... DtoTag<RaycastParams>)` /
`DtoTag<ShapecastParams>` forward decls alongside the existing animation ones (`control_dto.cppm:1768+`),
and the matching bodies in `emitDtoSerde()` in `gen.ts`.

The commands live in a **new `control_commands_physics.cpp`** (the registry already groups by area —
`registerSceneCommands` / `registerAnimationCommands`; add `registerPhysicsCommands(reg)` and call it from
the same place the others are registered):

```cpp
registerCommand<RaycastParams, RaycastResult>(
    reg, "raycast",
    "raycast {origin:{x,y,z}, dir:{x,y,z}, maxDist=1000} — closest physics hit (entity/point/normal/distance)",
    [](EngineContext& ctx, const RaycastParams& params) -> Result<RaycastResult>
    {
        const PhysicsWorld* world = ctx.sceneEdit.physics;  // host-installed seam (§4); null in Edit
        if (world == nullptr)
        {
            return Err("no physics world — enter play first");
        }
        const PhysicsRayHit h = raycastWorld(*world, toGlm(params.origin), toGlm(params.dir),
                                             params.maxDist.value_or(1000.0f));
        return RaycastResult{ h.hit, WireUuid{ h.entity }, fromGlm(h.point), fromGlm(h.normal), h.distance };
    });
```

`shapecast` is the same with `radius` plumbed through to `sphereCastWorld`. The "no physics world" error
is the deliberate, stable contract: a query only makes sense against a built world, and the world exists
**only while Playing/Paused** (Phase 1's lifecycle), so this command — unlike `pick`, which works against
the editor scene — refuses in Edit. (The e2e asserts that message exactly.)

> Per the keep-scriptable rule, this is the physics-specific command the AGENTS guidance calls for:
> there is real engine state (the live world) worth driving from a shell. `control_commands_physics.cpp`
> joins the file table in `engine/source/saffron/control/AGENTS.md` in this change.

### 3. The `se raycast` CLI verb

One branch in `printResult` (`tools/se/source/main.cpp`, keyed on `"raycast"` / `"shapecast"`), mirroring
the `pick` branch (`:369-381`):

```cpp
if (cmd == "raycast" || cmd == "shapecast")
{
    if (result.value("hit", false))
    {
        const json p = result.value("point", json::object());
        const json n = result.value("normal", json::object());
        std::printf("hit entity=%s  point=(%.3f, %.3f, %.3f)  normal=(%.2f, %.2f, %.2f)  dist=%.3f\n",
                    result.value("entity", std::string{}).c_str(), p.value("x", 0.0), p.value("y", 0.0),
                    p.value("z", 0.0), n.value("x", 0.0), n.value("y", 0.0), n.value("z", 0.0),
                    result.value("distance", 0.0));
    }
    else
    {
        std::printf("no hit\n");
    }
    return;
}
```

Invocation (the `dir` / `origin` are JSON-literal positionals coerced by `buildParams`):
`se raycast --origin '{"x":0,"y":5,"z":0}' --dir '{"x":0,"y":-1,"z":0}' --maxDist 20`.

### 4. Wire the world handle to `SceneEditContext`, and bind the Lua query in the Host

**The world handle on `SceneEditContext`** (the seam a command reads through, §2):

```cpp
// In scene_edit_context.cppm, next to `simTick` — a non-owning view of the play world.
// The Host sets it on Edit->Playing and nulls it on ->Edit; null means "no world" (Edit/Paused-before-build).
struct PhysicsWorld;  // forward decl; SceneEdit does not import Saffron.Physics
PhysicsWorld* physics = nullptr;
```

`SceneEdit` only forward-declares `PhysicsWorld` (a pointer needs no definition) so the DAG edge
`SceneEdit → Physics` is **not** introduced — the Host, which imports both, assigns the pointer. In the
`host.cppm:694` subscription, alongside the script start/stop, set `state->editor->physics =
&state->physicsWorld;` on `Playing` and `state->editor->physics = nullptr;` on `Edit` (and on the
`Detach`/`onExit` path at `host.cppm:981`, beside `simTick = nullptr`).

**The Lua `se.raycast`** is registered by the Host into the script VM, not by `Saffron.Script` (which
must not gain a physics edge). After `startScripts` builds the VM, the Host adds the query function with
the world captured — the same lambda-with-capture pattern as `get_entity_by_name`
(`script_runtime.cpp:407`):

```cpp
// Host-side, after startScripts succeeds (host.cppm script-lifecycle block). The VM lives exactly
// as long as the world does (both keyed on Playing), so the captured pointer is lifetime-safe.
luabridge::getGlobalNamespace(state->script.vm.state)
    .beginNamespace("se")
    .addFunction("raycast",
                 [state](float ox, float oy, float oz, float dx, float dy, float dz, float maxDist) -> luabridge::LuaRef
                 {
                     const PhysicsRayHit h = raycastWorld(state->physicsWorld, { ox, oy, oz }, { dx, dy, dz }, maxDist);
                     // return a table { hit, entity (a se.Entity or nil), point={x,y,z}, normal={x,y,z}, distance }
                     return makeRayHitTable(state->script.vm.state, *state, h);
                 })
    .endNamespace();
```

`makeRayHitTable` resolves `h.entity` (uuid) back to a `ScriptEntity` via the scene the host already
borrows (`host.currentScene`), so gameplay code can `local hit = se.raycast(px,py,pz, 0,-1,0, 2); if
hit.hit and hit.entity then ... end`. A `Saffron.Script`-only test cannot exercise this (it needs the
world), so its coverage is the e2e in §5, not the `runScriptSelfTest` spike.

> Why the Host registers it and not the script module: the DAG block in `AGENTS.md` lists
> `Script → {Core, Scene}` and "only Host may import" the leaf game modules. `Saffron.Physics` is such a
> leaf. Binding the query from the Host keeps `Saffron.Script` free of any physics edge, identical to how
> the Host owns and composes both runtimes today.

### 5. e2e coverage — `tests/e2e/physics-query.test.ts`

Mirror `foot-ik.test.ts` / `play.test.ts`. Boot headless, `import-model` a box fixture above a floor (the
Phase-2 falling-box / Phase-3 floor fixtures), `play`, `settle` until the box rests, then:

- **Down-ray finds the floor:** `raycast` from above the floor straight down → `hit == true`, `entity`
  is the floor entity, `point.y ≈ floor top`, `normal.y ≈ 1`, `distance` ≈ the drop.
- **Miss reports no hit:** a ray pointing into empty space → `hit == false`, `entity == "0"`.
- **`shapecast` tolerates an edge a thin ray misses:** a sphere sweep grazing a box corner hits where the
  zero-radius ray returns no hit.
- **Refuses in Edit:** before `play` (no world), `raycast` rejects with `/no physics world/` — the stable
  contract from §2 (`expect(...).rejects.toThrow(/no physics world/)`, as `rig-query.test.ts:87` does for
  `/no rig/`).
- **A Lua script can query:** a `src/` script that casts down in `on_update` and, on a hit, writes a flag
  the test reads back through an inspectable component (the `script.test.ts` pattern) — proving the
  Host-bound `se.raycast` reaches the live world.
- The standard `expect(engine.validationErrors()).toEqual([])` close.

Add the command's `gen.ts` fixture (an `origin`/`dir` payload) so the manifest contract test covers it.

### 6. docs

Add `docs/content/explanations/physics/scene-queries.md` — the concept (a query reads the live world's
narrow phase; entity-uuid mapping; ray vs sphere sweep; read-only so it does not perturb the deterministic
step), the slim `What | File | Symbols` table (`raycastWorld`/`sphereCastWorld`, the `raycast` command, the
`se.raycast` Lua binding), and the gotcha that queries only work in Play (the world's lifecycle). Add the
row to the physics hub `docs/content/explanations/physics/_index.md`.

## Validation (done criteria)

- `make engine` green (`Saffron.Physics` gains `raycastWorld`/`sphereCastWorld`; the new
  `control_commands_physics.cpp` compiles; the `SceneEditContext::physics` forward-decl pointer adds no
  new DAG edge).
- `make prepare-for-commit` clean (clang-format + clang-tidy) over the new/changed files.
- `bun run check` passes — the regenerated `@saffron/protocol` carries `RaycastParams`/`ShapecastParams`/
  `RaycastResult`; the five generated outputs are regenerated by `bun run tools/gen-control-dto/gen.ts`
  and committed (control serde, scene-component serde, `se-types.ts`, OpenRPC, manifest).
- `make e2e` passes `tests/e2e/physics-query.test.ts` (down-ray hits the floor, miss reports none,
  sphere-cast grazes an edge, Edit refuses, the Lua script queries) with a validation-clean log.
- `docs/`: the scene-queries page + the physics hub row land in the same change.

## Notes / gotchas

- **Query vs `pick` are distinct and both stay.** `pick` (`control_commands_scene.cpp:683`) tests render
  AABBs/billboards for *editor selection* in Edit; `raycast` tests *physics shapes at simulated
  transforms* for gameplay in Play. They serve different surfaces — this is not a duplicate path to retire.
- **The world handle is non-owning.** `SceneEditContext::physics` is a borrowed `PhysicsWorld*`, set and
  nulled by the Host on the play-state edges, exactly like `simTick`. A command must treat null as "no
  world" and return the stable error — never deref. The world dies with the play duplicate on `stop`
  (Phase 1), so the pointer is nulled in the same `→Edit` handler that tears the world down; there is no
  separate reset.
- **Run the query off the step.** Jolt's `NarrowPhaseQuery` is safe to call when no `PhysicsSystem::Update`
  is in flight. Commands run on the main thread between frames (`command.cppm` dispatch), and the Lua
  binding runs inside `on_update` (the `simTick` seam, *after* the step completes), so both are clear of
  the step's job graph. Do not call a query from a `ContactListener` callback (that fires *during* the
  step).
- **Single-closest only in v1.** `CastRay`/`CastShape` return the nearest hit; an all-hits collector
  (`CastRayCollector`), layer-mask filtering of the query (an `ObjectLayerFilter` argument), and
  `CollidePoint`/overlap queries are **deferred** — they extend this same entry point and result type
  later, not a new path.
- **`dir` is not normalized for the caller.** `raycastWorld` scales `dir` by `maxDist` internally, so a
  caller can pass an un-normalized direction (e.g. a velocity vector) and read `distance` back in those
  units; document this on the param so the CLI/Lua usage is unambiguous.
