# Phase 1 — `Saffron.Physics` module + Jolt vendoring + world lifecycle

**Status:** COMPLETED

## Goal

Stand up a new **`Saffron.Physics`** module and the bare lifecycle scaffolding around it: vendor **Jolt**
via FetchContent (cross-platform deterministic, samples off), define a `:Types` partition (POD enums +
layer ids + an opaque `PhysicsWorld` handle), and own a `PhysicsWorld` on `HostState` that is **created
on the Edit→Playing edge and destroyed on →Edit** via `onPlayStateChanged` — exactly mirroring the
script-VM lifecycle. The world is empty: no components, no bodies, no stepping yet. What this proves is
that the module builds in isolation, that Jolt's global init (`RegisterDefaultAllocator` / `Factory` /
`RegisterTypes`) and matching teardown are correct, and that entering/stopping play allocates and frees a
Jolt world **validation-clean** (no Jolt asserts, no leaks). Bodies, components, and the fixed-step tick
land in Phase 2+; do not bleed them in here.

## What exists to build on

- **Engine is one flat static lib.** Module interface units live in the `CXX_MODULES` file set
  (`engine/CMakeLists.txt:4-28`), impl `.cpp` units are `PRIVATE` sources (`:32-53`); `cxx_std_26` +
  `CXX_MODULE_STD ON` are set on the target (`:55-56`). `Saffron.Animation` is the most recent module to
  follow this — its interface is `:15` (`source/saffron/animation/animation.cppm`) and its impl is `:41`
  (`source/saffron/animation/animation.cpp`). Copy that pair shape.
- **The DAG** (`AGENTS.md:158-173`): `Animation→ {Core, Geometry, Scene}` (`:165`), and
  `Host → {Core, App, Window, Rendering, SceneEdit, Control, Scene, Animation, Script, Assets}` (`:172`).
  Modules wrapping heavy **C++** headers (`scene`, `geometry`, `animation`, …) use classic `#include` in
  the global module fragment and **do NOT `import std`** (`AGENTS.md:176-179`). `Saffron.Physics` wraps
  the Jolt C++ headers, so it follows that rule.
- **Third-party deps are vendored via FetchContent** in `cmake/Dependencies.cmake`. The pattern is
  `FetchContent_Declare(... GIT_TAG <tag> GIT_SHALLOW ON)` then `FetchContent_MakeAvailable(...)`
  (`:13-33`), and everything the engine links is aggregated under the `saffron_third_party` INTERFACE
  target (`:99-114`). The engine links it `PUBLIC` (`engine/CMakeLists.txt:58`). Jolt joins that target.
- **`HostState`** (`host/host.cppm:48-60`) is the subsystem bag: it already holds
  `se::AnimationRuntime animation;` (`:53`) and `se::ScriptHost script;` (`:54`) as siblings, plus
  `se::SubscriptionId scriptSubscription;` (`:55`) for the lifecycle hook. The new `PhysicsWorld` is a
  sibling here.
- **The script-VM lifecycle is the exact template.** In `config.onCreate`, the Host subscribes to
  `onPlayStateChanged` (`host/host.cppm:694-718`): on `PlayState::Playing && !scriptVmActive` it starts
  the VM and flips `scriptVmActive = true`; on `PlayState::Edit && scriptVmActive` it tears the VM down.
  Teardown in `config.onExit` (`:979-982`) calls `stopScripts`, then `onPlayStateChanged.unsubscribe(
  state->scriptSubscription)`. The physics world gets a parallel `physicsSubscription` + an
  `unsubscribe` in `onExit`.
- **`onPlayStateChanged` is the documented physics/scripting lifecycle seam** (`scene_edit_context.cppm:220`
  — `SubscriberList<PlayState> onPlayStateChanged; // the physics/scripting lifecycle seam`). It is fired
  by `publishTransition` (`scene_edit_play.cpp:23-28`), which every transition routes through:
  `enterPlay` → `publishTransition(ctx, PlayState::Playing)` (`:98`); `stopPlay` →
  `publishTransition(ctx, PlayState::Edit)` (`:159`).
- **Play mode has no undo — the discard is the restore.** `enterPlay` duplicates the scene via
  `sceneToJson`/`sceneFromJson` into `ctx.playScene` (`scene_edit_play.cpp:83-97`); `stopPlay` does
  `ctx.playScene.reset()` (`:152`). So a physics world built against the play duplicate on Edit→Playing
  and dropped on →Edit lines up perfectly with the scene it simulates — there is nothing authored to
  reset.
- **The fixed-step constants the later tick will use already exist:** `inline constexpr f32 PlayFixedStep
  = 1.0f / 60.0f;` (`scene_edit_context.cppm:172`) and `inline constexpr f32 PlayMaxDelta = 1.0f / 3.0f;`
  (`:173`). The `simTick` seam (`scene_edit_context.cppm:224`; invoked in `tickPlay` at
  `scene_edit_play.cpp:200-203`) is where Phase 2 will step physics. **Not this phase** — listed only so
  the world's `dt` accumulator field is shaped right.

## Work

### 1. Vendor Jolt in `cmake/Dependencies.cmake`

Add a `FetchContent_Declare` next to the existing source libs (`Dependencies.cmake:13-33`), pinning a
release tag and shallow-cloning. Jolt's CMake lives under its `Build/` subdir, so point
`SOURCE_SUBDIR` at it, and set the cache vars that strip samples/tests and the determinism + flag knobs
**before** `FetchContent_MakeAvailable`:

```cmake
# --- Jolt Physics (jrouwe/JoltPhysics, MIT; built from source, static) --------
# Cross-platform determinism ON from the start (the engine wants bit-exact sims
# across machines for future lockstep networking/replay; modest perf cost, still
# single precision). OVERRIDE_CXX_FLAGS OFF so Jolt honors our libc++/gnu++26
# preset instead of forcing its own. Samples/tests/tools excluded.
set(CROSS_PLATFORM_DETERMINISTIC ON  CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION             OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS           OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES               OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS            OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD           OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER                OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.3.0 GIT_SHALLOW ON
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(JoltPhysics)
```

Then add `Jolt` to the `saffron_third_party` INTERFACE link list (`Dependencies.cmake:100-114`,
alongside `glm::glm` / `EnTT::EnTT`). The target name Jolt's CMake exports is `Jolt`; confirm against the
pinned tag's `Build/CMakeLists.txt` when wiring (Jolt has historically exported `Jolt`, not a namespaced
alias). If Jolt's headers trip `-Wdeprecated`/nullability under our warning set the way
LuaBridge3/VMA/cgltf do, mark its include dirs SYSTEM (the `SYSTEM` keyword on the `Declare`, like
LuaBridge3 at `Dependencies.cmake:62-65`) rather than weakening engine warnings.

> Determinism is **locked** (design decision 4): set `CROSS_PLATFORM_DETERMINISTIC ON` here, in phase 1,
> not later. It changes the math Jolt compiles in, so flipping it post-hoc would invalidate any recorded
> sim. Single precision (`DOUBLE_PRECISION OFF`) stays — cross-platform determinism is float-deterministic.

### 2. Create the `Saffron.Physics` module skeleton

New files `engine/source/saffron/physics/physics.cppm` (interface) and `physics.cpp` (impl), wired into
`engine/CMakeLists.txt`: the `.cppm` into the `CXX_MODULES` set (after `scene.cppm`/`animation.cppm`,
`:13-16`) and the `.cpp` into `PRIVATE` (`:32-53`). The module declaration mirrors `scene.cppm` /
`host.cppm:1-40`: classic `#include` in the global module fragment, **no `import std`**:

```cpp
module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

export module Saffron.Physics;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Scene;
import Saffron.Animation;
```

> The DAG edge declared is `Physics → {Core, Geometry, Scene, Animation}`. Core for `Result`/`f32`/`u32`;
> Geometry for the `.smesh` vertex data convex-hull/mesh cooking will read (Phase 3); Scene for the
> `Entity` and the (Phase-2) `RigidbodyComponent`/`ColliderComponent` it consumes; Animation for the
> ragdoll handoff (`PoseBuffer`, Phase 8-9). Declare the full edge now to avoid CMake/DAG churn later,
> even though this phase only touches Core. **Jolt headers appear in `physics.cpp` only** (see §4) — the
> interface unit is Jolt-free so consumers never transitively pull `<Jolt/...>`.

### 3. The `:Types` partition — POD enums, layer ids, and the opaque world handle

A partition `Saffron.Physics:Types` holds the Jolt-free vocabulary every consumer (Phase 2 components map
onto these enums; the control commands echo them). These are deliberately a *parallel* vocabulary, not
re-exports of Jolt enums, so the interface stays Jolt-free:

```cpp
export module Saffron.Physics:Types;

import Saffron.Core;

namespace se
{
    /// How a body participates in the simulation. Mirrors Jolt EMotionType 1:1.
    /// A ColliderComponent without a RigidbodyComponent is an implicit Static body
    /// (locked design decision 1); a present RigidbodyComponent's motion type wins.
    enum class MotionType : u8 { Static, Kinematic, Dynamic };

    /// Broad-phase / object collision layers. v1 ships exactly two, matching the
    /// canonical Jolt example layout (moving vs non-moving); Phase 4 generalizes
    /// to an authored layer table + the three Jolt filter interfaces.
    enum class PhysicsLayer : u16 { NonMoving = 0, Moving = 1 };
    inline constexpr u16 PhysicsLayerCount = 2;
}
```

The `PhysicsWorld` itself is an **opaque, move-only handle** declared in the main interface unit
(`physics.cppm`, not the partition — it is defined out-of-line in `physics.cpp` where Jolt is visible).
The pointer-to-incomplete-impl (pimpl) keeps `<Jolt/...>` out of the BMI:

```cpp
namespace se
{
    struct PhysicsWorldImpl;  // defined in physics.cpp; owns the Jolt objects

    /// The per-play-session physics world: wraps Jolt's PhysicsSystem + BodyInterface +
    /// TempAllocator + JobSystem and (Phase 2+) the entity<->BodyID maps and a fixed-step
    /// accumulator. Owned by HostState, built on Edit->Playing, dropped on ->Edit.
    /// Move-only RAII: the destructor removes all bodies and frees the Jolt objects.
    class PhysicsWorld
    {
    public:
        PhysicsWorld(PhysicsWorld&&) noexcept;
        PhysicsWorld& operator=(PhysicsWorld&&) noexcept;
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;
        ~PhysicsWorld();

    private:
        PhysicsWorld() noexcept;                 // only createPhysicsWorld constructs one
        std::unique_ptr<PhysicsWorldImpl> impl_;
        friend auto createPhysicsWorld() -> Result<PhysicsWorld>;
    };

    /// One-time Jolt global init (idempotent): RegisterDefaultAllocator, install the
    /// Factory, RegisterTypes. Called once before the first world is built.
    auto initPhysics() -> Result<void>;

    /// Tear down the Jolt globals: UnregisterTypes + delete the Factory. Pairs with initPhysics.
    void shutdownPhysics();

    /// Allocate an empty Jolt world (PhysicsSystem::Init with v1 limits, the two-layer
    /// filters, single-precision, cross-platform deterministic). No bodies yet.
    auto createPhysicsWorld() -> Result<PhysicsWorld>;
}
```

`PhysicsWorldImpl` (in `physics.cpp`) holds the Jolt members the later phases need, even though this
phase only constructs them empty: `JPH::PhysicsSystem system;`, a `JPH::TempAllocatorImpl tempAllocator;`,
a `JPH::JobSystemThreadPool jobSystem;`, the three filter-interface implementors
(`BroadPhaseLayerInterface` / `ObjectVsBroadPhaseLayerFilter` / `ObjectLayerPairFilter`), and the
soon-to-be-used `std::unordered_map<u32 /*entity*/, JPH::BodyID>` + the `f32 accumulator` (left at 0 in
v1). Keep the maps/accumulator present-but-unused so Phase 2 adds bodies without touching the type.

### 4. Jolt global init + the single Jolt TU (`physics.cpp`)

`physics.cpp` is the **only** translation unit that includes `<Jolt/...>`. The Jolt boot sequence is fixed
(`RegisterDefaultAllocator()`, `JPH::Factory::sInstance = new JPH::Factory()`, `JPH::RegisterTypes()`),
matched by `JPH::UnregisterTypes()` + `delete JPH::Factory::sInstance`. Guard it so `initPhysics` is
idempotent and `shutdownPhysics` only fires after the last world is gone:

```cpp
module;

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>

#include <memory>

module Saffron.Physics;

import Saffron.Core;

namespace se
{
    auto initPhysics() -> Result<void>
    {
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr)
        {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
        return {};
    }

    void shutdownPhysics()
    {
        if (JPH::Factory::sInstance != nullptr)
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }
}
```

`createPhysicsWorld` builds `PhysicsWorldImpl`, calls `system.Init(maxBodies, numBodyMutexes,
maxBodyPairs, maxContactConstraints, broadPhaseLayerInterface, objectVsBroadPhaseFilter,
objectLayerPairFilter)` with v1 limits (e.g. `maxBodies = 1024`, the canonical mutex/pair/constraint
counts), and returns the move-only handle. The destructor must run before the Jolt globals shut down
(the world holds bodies registered in the system) — so the Host drops the world *before* it calls
`shutdownPhysics` (see §5). Route any `system.Init` / shape-create failure through `Result<T>`, checked on
the spot (`AGENTS.md` Errors rule); **no exceptions** — Jolt itself uses asserts, not exceptions, so the
fallible surface is the `std::expected` boundary we add.

### 5. Own the world on `HostState`; build/destroy on the play edge

Add a sibling to `animation`/`script` on `HostState` (`host/host.cppm:48-60`):

```cpp
struct HostState
{
    // ... editor, control, assets, animation, script ...
    se::SubscriptionId scriptSubscription;
    se::SubscriptionId physicsSubscription;     // the onPlayStateChanged physics lifecycle hook
    std::optional<se::PhysicsWorld> physics;    // a Jolt world exists exactly while play is active
    bool physicsInit = false;                   // initPhysics() has run (Jolt globals installed)
    // ... flags ...
};
```

In `config.onCreate`, call `se::initPhysics()` once (next to the other one-time setup), then subscribe to
`onPlayStateChanged` exactly like the script hook (`host/host.cppm:694-718`):

```cpp
state->physicsSubscription = state->editor->onPlayStateChanged.subscribe(
    [state](se::PlayState next)
    {
        if (next == se::PlayState::Playing && !state->physics.has_value())
        {
            auto world = se::createPhysicsWorld();
            if (!world)
            {
                se::logError(std::format("physics world create failed: {}", world.error()));
                return false;
            }
            state->physics.emplace(std::move(*world));
        }
        else if (next == se::PlayState::Edit && state->physics.has_value())
        {
            state->physics.reset();  // RAII: removes bodies + frees the Jolt world
        }
        return false;
    });
```

In `config.onExit` (`host/host.cppm:979-985`), tear down in order: drop the world if it survived a
mid-play quit (`state->physics.reset();`), unsubscribe (`state->editor->onPlayStateChanged.unsubscribe(
state->physicsSubscription);`), then `se::shutdownPhysics();` (after the last world is gone, so the
`Factory`/types outlive every body). This mirrors the script teardown right above it and must precede
`destroySceneEditContext`.

> The world is **not** stepped this phase — `simTick` (`scene_edit_play.cpp:200`) still runs only the
> script seam. The Phase-2 milestone is the first frame physics actually advances. Here, success is
> "enter play → a Jolt world is alive; stop play → it is freed; no Jolt assert, no leak."

### 6. Update the module DAG + module list in `AGENTS.md`

Add `Physics → {Core, Geometry, Scene, Animation}` to the DAG block (`AGENTS.md:158-173`), placed after
`Animation→` (`:165`) since it depends on it, and add `Physics` to the `Host → {…}` edge (`:172`):
`Host → {Core, App, Window, Rendering, SceneEdit, Control, Scene, Animation, Physics, Script, Assets}`.
Add `physics` to the classic-`#include`/no-`import std` module list (`AGENTS.md:176-178`). Add a one-line
`Saffron.Physics (Jolt)` row to the Stack table / Status (it moves out of the "Not yet" list
(`AGENTS.md:272`) only when the rigidbody slice — Phases 1-7 — lands; in this phase it is `IN PROGRESS`).

## Validation (done criteria)

- `make engine` green: Jolt builds from source (samples/tests excluded), `Saffron.Physics` compiles, and
  the BMI is Jolt-free (a consumer importing `Saffron.Physics` does not pull `<Jolt/...>` — verified by
  the fact that only `physics.cpp` includes it and the engine still links).
- `make prepare-for-commit` clean (clang-format + clang-tidy) for the new files; Jolt's headers are SYSTEM
  if they would otherwise raise warnings (the LuaBridge3/VMA precedent), and **no engine warning is
  weakened** to accommodate them.
- A headless `SAFFRON_EXIT_AFTER_FRAMES=N` run that enters and stops play (driven over the control plane
  or a self-test) allocates and frees a Jolt world with **no Jolt assert and no validation/leak noise** in
  the log. Add a `runPhysicsSelfTest()` to the `SAFFRON_SELFTEST` block (`host/host.cppm:736-761`,
  alongside `runAnimationSelfTest`) that calls `initPhysics` → `createPhysicsWorld` → drop →
  `shutdownPhysics` and asserts each step's `Result` is ok — the unit gate the headless build runs.
- `make e2e`: a minimal `tests/e2e/physics-world.test.ts` (mirroring `tests/e2e/rig-preview.test.ts`)
  that boots a headless engine, sends `enter-play` then `stop-play` over the control plane, and asserts a
  validation-clean log across the transition. Add a tiny `get-physics-stats` control command (one
  `registerCommand` in `Saffron.Control`, per the keep-scriptable rule) returning `{ worldAlive: bool }`
  so the test has engine state to observe; wire the matching `se` verb.
- `docs/`: add `docs/content/explanations/physics/_index.md` hub + a "physics world lifecycle" page
  (concept: the per-session Jolt world built on the play edge and discarded with the play duplicate; the
  cross-platform-deterministic + single-precision choice and why; the Jolt-free module boundary). Add the
  hub row to the explanations index.

## Notes / gotchas

- **The world's lifetime must nest inside the Jolt globals.** `~PhysicsWorld` removes its bodies from the
  `PhysicsSystem` and frees Jolt objects that reference the registered types/`Factory`; `shutdownPhysics`
  (`UnregisterTypes` + `delete Factory`) must run **after** the last world is destroyed. The `onExit`
  order in §5 enforces this; do not flip it.
- **Determinism is compiled in.** `CROSS_PLATFORM_DETERMINISTIC ON` is set in §1 and is **not**
  renegotiable later (design decision 4) — it changes Jolt's math at compile time, so a recorded sim from
  a non-deterministic build would never replay. Note the modest perf cost in the docs page but do not
  treat it as a tunable.
- **No bodies, no step, no components in this phase.** `RigidbodyComponent`/`ColliderComponent` (which
  live in `Saffron.Scene` and follow the component recipe — declare in `scene.cppm`, edit `emitSceneSerde`
  in `tools/gen-control-dto/gen.ts`, register in `scene_edit_components.cpp`) are Phase 2. Auto-fit,
  layers/filters beyond the two canonical ones, triggers, kinematic bone-following, character controller,
  raycasts, and the ragdoll (Phases 8-9) are all later. Resist adding any of it here — the deliverable is
  a clean, validation-clean empty-world lifecycle.
- **Keep the interface Jolt-free.** Every Jolt type stays behind the pimpl (`PhysicsWorldImpl` in
  `physics.cpp`). If a Jolt header ever leaks into `physics.cppm`, the BMI carries it and every importer
  (ultimately `Saffron.Host`) gets the heavy include — exactly the mistake the `:Types`/opaque-handle
  split exists to prevent. This is the same discipline `renderer_types.cppm` uses for `vk::`.
- **`OVERRIDE_CXX_FLAGS OFF` is load-bearing for the build.** Jolt's default CMake forces its own flags;
  with it off, Jolt honors the `debug`/`release` preset's `-stdlib=libc++` + `gnu++26`, so its objects
  link against the same libc++ as the rest of the engine. If Jolt still injects an incompatible flag at
  the pinned tag, prefer a per-target flag scrub over weakening the preset.
