# Phase 1 — Engine: play-state machine + scene duplication

**Status:** NOT STARTED

**Depends on:** phase 0.

The whole play-mode core lands in `Saffron.SceneEdit`, with no host or control-plane changes
yet: the `PlayState` machine on `SceneEditContext`, the play-scene duplicate built through the
existing JSON serde, the `activeScene()` chokepoint, the tick gate, and a self-test proving the
discard guarantee in-process.

## State (`scene_edit_context.cppm`)

Next to `GizmoOp` (`:40`), a backend-neutral enum plus name mapping (mirroring
`gizmoOpName`/`gizmoOpFromName` so the control plane and CLI reuse it):

```cpp
enum class PlayState { Edit, Playing, Paused };
auto playStateName(PlayState s) -> const char*;          // "edit"|"playing"|"paused"
auto playStateFromName(const std::string& name) -> PlayState;
```

On `SceneEditContext` (`:102-121`), beside the existing version stamps:

```cpp
PlayState playState = PlayState::Edit;
std::optional<Scene> playScene;          // the throwaway duplicate; nullopt in Edit
u64 playVersion = 0;                     // bumped on every transition (reconcile-poll stamp)
i32 stepFrames = 0;                      // pending single-step ticks (Hazel gate)
bool hadPrimaryCamera = false;           // captured at enterPlay; drives the editor warning
SubscriberList<PlayState> onPlayStateChanged;  // the physics/scripting lifecycle seam
```

`playState` is session policy: it lives on the context, never on `Scene`, and never reaches
`sceneToJson`. The context is already heap-owned (`newSceneEditContext`, `:139`), so the extra
registry inside `playScene` adds nothing new to teardown placement.

The chokepoint, in the same header:

```cpp
inline auto activeScene(SceneEditContext& ctx) -> Scene&
{
    return ctx.playState == PlayState::Edit ? ctx.scene : *ctx.playScene;
}
```

Every later phase routes through this; nothing else may branch on `playState` to pick a scene.

One more helper this phase must add, because no standalone uuid→entity lookup exists today —
the only such loop is inlined in `resolveEntity` (`control_server.cpp:103-114`):

```cpp
auto findEntityByUuid(Scene& scene, u64 uuid) -> Entity;  // null handle when absent
```

Exported from `Saffron.Scene` (a free function over the world, beside `forEach`). The
selection re-resolution below uses it against both scenes, and phase 3 rewires
`resolveEntity`'s inline loop onto it.

## Transitions (new `sceneedit/scene_edit_play.cpp`)

Free functions exported from `Saffron.SceneEdit`, shaped like `setSelection`
(`scene_edit_context.cpp`). Every transition bumps `playVersion` and fires
`onPlayStateChanged.publish(next)` (the `onSelectionChanged.publish` idiom,
`scene_edit_context.cpp:22`).

```cpp
auto enterPlay(SceneEditContext&) -> Result<void>;   // Edit -> Playing; error if not Edit
void pausePlay(SceneEditContext&);                    // Playing -> Paused
void resumePlay(SceneEditContext&);                   // Paused -> Playing
void stepPlay(SceneEditContext&, i32 frames);         // Paused only: stepFrames += frames
auto stopPlay(SceneEditContext&) -> Result<void>;     // Playing|Paused -> Edit; no-op in Edit
void tickPlay(SceneEditContext&, f32 dt);             // the host onUpdate driver
```

`enterPlay`:

1. `nlohmann::json snap = sceneToJson(ctx.registry, ctx.scene)` (`scene.cppm:900`).
2. Build the duplicate: fresh `Scene`; `environment` is a value copy; `catalog` shares the
   borrowed pointer (`scene.cppm:308` — the host re-sets it per frame anyway);
   `sceneFromJson(ctx.registry, play, snap)` (`scene.cppm:917` — clears the registry, rebuilds
   entities with their durable uuids, `relinkHierarchy` restores the parent caches; the
   `ComponentRegistry` is scene-agnostic, so the edit-context registry serves both). The
   `snap` doc is dropped immediately; nothing holds it for stop.
3. `ctx.hadPrimaryCamera = primaryCamera(play).valid` (`scene.cppm:685`).
4. Re-resolve the selection into the duplicate by uuid (`findEntityByUuid`): the old `entt`
   handle indexes the edit registry and must not leak into play (handles can coincide across
   registries and silently alias the wrong entity). Bump `selectionVersion`.
5. `ctx.playScene = std::move(play); ctx.playState = Playing;` stamp + signal. `Scene` is
   move-only (`entt::registry` deletes copy), which is exactly what the `std::optional`
   assignment needs. Deliberately no `sceneVersion` bump here: the duplicate is
   uuid-identical to the authored scene at this instant, so the editor's entity list is
   already correct; the `selectionVersion` bump alone refreshes the inspector.

`stopPlay`:

1. Re-resolve the selection back into `ctx.scene` by uuid via `findEntityByUuid` (clear it if
   the selected entity was runtime-spawned and has no authored twin).
2. `ctx.playScene.reset(); ctx.playState = Edit;` — the discard *is* the restore; the edit
   scene was never writable through `activeScene` while playing.
3. Bump `selectionVersion`, `playVersion`, **and `sceneVersion`** — the sceneVersion bump is
   what makes the editor's heavy reconcile re-fetch the authored entity list after the play
   scene disappears (`store.ts:529`).

`tickPlay` — the runtime tick seam, gated Hazel-style:

```cpp
inline constexpr f32 kFixedStep = 1.0f / 60.0f;  // deterministic `step` tick
inline constexpr f32 kMaxDelta  = 1.0f / 3.0f;   // reserved anti-tunneling clamp (Unity parity)

void tickPlay(SceneEditContext& ctx, f32 dt)
{
    if (ctx.playState == PlayState::Edit) return;
    const bool run = ctx.playState == PlayState::Playing || ctx.stepFrames > 0;
    if (!run) return;
    if (ctx.stepFrames > 0) { ctx.stepFrames -= 1; dt = kFixedStep; }
    dt = std::min(dt, kMaxDelta);
    // The seam: physics step, script update, animation advance hang here, against *ctx.playScene.
    // v1 has none of them; updateWorldTransforms already runs inside renderScene (assets.cppm:833).
    static_cast<void>(dt);
}
```

Invariants enforced by the transition functions (and asserted by phase 4 over the wire):
`enterPlay` from `Playing`/`Paused` is an error; `pausePlay` from anything but `Playing` is an
error; `stepPlay` from anything but `Paused` is an error; `stopPlay` from `Edit` is an
idempotent no-op returning success.

## Self-test

`runPlayModeSelfTest()` exported from `Saffron.SceneEdit`, called from the host's
`SAFFRON_SELFTEST` block beside `runSceneSerializationSelfTest` (`host.cppm:464-468`),
using the same `expect` style as the scene self-tests (`runSceneSerializationSelfTest`,
`scene.cppm:1008`):

- enterPlay duplicates: same entity count, uuids match pairwise, edit and play handles for one
  entity differ-or-alias-safely (assert lookups go by uuid).
- Mutate a transform and create an entity in `playScene`; `stopPlay`; assert the edit scene's
  transform is bit-identical and the runtime entity does not exist.
- The state-machine error cases above; `stepFrames` accounting (pause, step 2, two ticks run,
  third does not).
- Selection: select an entity, play → selection resolves to the play twin (uuid equal), stop →
  back to the authored entity; select a runtime-spawned entity, stop → selection cleared.

## Touched

| What | File | Symbols |
|---|---|---|
| State + accessor | `engine/source/saffron/sceneedit/scene_edit_context.cppm` | `PlayState`, `SceneEditContext`, `activeScene` |
| Uuid lookup | `engine/source/saffron/scene/scene.cppm` | `findEntityByUuid` |
| Transitions + tick + self-test | `engine/source/saffron/sceneedit/scene_edit_play.cpp` (new) | `enterPlay`…`tickPlay`, `runPlayModeSelfTest` |
| Self-test wiring | `engine/source/saffron/host/host.cppm` | the `SAFFRON_SELFTEST` block |
| Build | `engine/CMakeLists.txt` (sceneedit `.cpp` block at `:40-43`) | add the new TU |

## Verify

- `toolbox run -c saffron-build bash -lc 'cmake --build build/debug -j1'` clean.
- Headless selftest run (`SAFFRON_SELFTEST=1 SAFFRON_EXIT_AFTER_FRAMES=3`) passes with a
  validation-clean log.
