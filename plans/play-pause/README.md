# Play / Pause / Stop Plan

This plan adds an editor play mode to SaffronEngine: a Play button that switches the viewport
from the editor fly-camera to the scene's primary `CameraComponent` and starts a runtime tick,
Pause/Step for frame-accurate inspection, and Stop that restores the authored scene exactly.
The architecture follows the Unreal PIE model adapted to this codebase: `play` duplicates the
edit scene into a throwaway play scene (via the existing JSON serde), everything simulates and
renders against the duplicate, and `stop` discards it — the edit scene is never mutably aliased
during play, so restore is structurally guaranteed rather than re-derived. The state machine
lives engine-side in `Saffron.SceneEdit`, is driven over the control plane
(`play`/`pause`/`stop`/`step`/`get-play-state`), and surfaces in the editor as a Topbar
playback group with Ctrl+P-family hotkeys and a play-mode tint.

Phase 0 records the research across UE5 / Unity / Frostbite / Godot / Hazel and the decision
rationale; it is the why behind every later phase.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file only
*after* it is `COMPLETED` and merged.

## Target shape

End state: Topbar shows Play; pressing it (or Ctrl+P) snapshots the scene into a play
duplicate, the viewport cuts to the scene's primary camera (fly-cam fallback + warning when
none), and a tint marks the editor as live. Pause freezes the runtime tick (rendering and the
control plane keep running); Step advances exactly one fixed tick while paused. All panels keep
working against the *running* scene (Godot remote-tree style — inspect and live-tune, changes
discarded on stop). Stop drops the duplicate, the viewport returns to the fly-cam, and the
authored scene is back untouched. Physics (Jolt), scripting, and animation later plug into the
already-wired tick seam and play-state signal without touching the machinery again.

IN SCOPE:

- `PlayState { Edit, Playing, Paused }` + transitions + `playVersion` stamp on
  `SceneEditContext`, with an `onPlayStateChanged` signal (the future physics/scripting seam).
- Scene duplication on `play` through `sceneToJson`/`sceneFromJson`; pure discard on `stop`.
- The `activeScene()` chokepoint: render, picking, and every scene-touching control command
  (including the asset ones) route to the play scene while playing, the edit scene otherwise.
- Camera handover to `primaryCamera()` with fly-cam fallback and a `hasPrimaryCamera` warning.
- The runtime tick seam (`tickPlay`) with the pause/step gate, fixed step size for `step`, and
  a reserved max-dt clamp.
- Control commands `play`/`pause`/`stop`/`step`/`get-play-state`, `SelectionResult` extended
  with `playState`/`playVersion`, generated DTO/schema/protocol, `se` CLI, contract test.
- Editor: Topbar playback group, store slice, Ctrl+P / Ctrl+Shift+P / Ctrl+Alt+P, play tint,
  save/load locked during play; selection survives play/stop by uuid re-resolution.
- An e2e test proving the discard guarantee over the wire.

OUT OF SCOPE / deferred:

- Eject/possess (UE5 F8) and a separate Game-vs-Scene dual view — one viewport, one camera at
  a time.
- "Keep simulation changes" (copy runtime state back to the edit scene) — the duplicate model
  makes it possible later; not in v1.
- Play-from-here, time scale UI, slow motion.
- An out-of-process play instance (Godot/Frostbite model). v1 plays in the existing host
  process; the wire contract is designed so a second process could serve the same commands
  later.
- A direct entt registry copy for duplication — deliberate decision to ship the JSON
  round-trip first and swap the internals behind `enterPlay` only if profiling shows a hitch.
- Physics, scripting, animation (they consume the seam; nothing here implements them).
- Undo/redo.

## Phase map

| # | Phase | File | Depends on |
|---|-------|------|------------|
| 0 | Research and architecture decision | `phase-0-research-and-architecture.md` | - |
| 1 | Engine: play-state machine + scene duplication | `phase-1-engine-play-state-and-scene-duplication.md` | 0 |
| 2 | Host: camera handover + runtime tick driver | `phase-2-host-camera-handover-and-tick.md` | 1 |
| 3 | Control: play commands, active-scene routing, schema + se CLI | `phase-3-control-play-commands-and-schema.md` | 1, 2 |
| 4 | e2e: the discard guarantee over the wire | `phase-4-e2e-discard-guarantee.md` | 3 |
| 5 | Editor: playback toolbar, store, hotkeys, tint + docs page | `phase-5-editor-toolbar-store-hotkeys.md` | 3 |

Phases 4 and 5 are independent of each other.

## Current anchors

All verified against the tree at plan time:

- The host renders **only** through the editor fly-camera: `layer.onUi` computes
  `cam = sceneEditCameraView(state->editor->camera)` and calls
  `renderScene(app.renderer, state->editor->scene, state->assets, cam)`
  (`host.cppm:574-589`). Nothing renders from a `CameraComponent` today.
- `primaryCamera(Scene&) -> CameraView` already exists (`scene.cppm:685`): first
  `CameraComponent` with `primary` wins, view = inverse world matrix (parent chain composed),
  `valid=false` when the scene has none. `CameraView` (`scene.cppm:676`) is the type both
  `renderScene` and the gizmo already consume — camera handover is choosing which `CameraView`
  to pass.
- Scene round-trip serde exists and is self-tested: `sceneToJson` (`scene.cppm:900`),
  `sceneFromJson` (`scene.cppm:917`, "Replaces the scene's entities") which does
  `registry.clear()` (`scene.cppm:936`) then `relinkHierarchy` (`scene.cppm:969`).
  `SceneVersion = 3` (`scene.cppm:770`). Entity identity is a durable uuid (`IdComponent`),
  so identity survives the duplicate; `entt` handles do not. (Line numbers drift — this tree
  is concurrently edited; the symbols are the anchor.)
- Duplication is GPU-free: the mesh/texture caches are uuid-keyed on `AssetServer`
  (`meshRefByUuid`/`textureRefByUuid`, `assets.cppm:40-41`), not per-`Scene`; `Scene.catalog`
  is a borrowed pointer set per frame (`scene.cppm:304-309`, `host.cppm:578`). Two scenes
  resolve the same uuid to the same `Ref<GpuMesh>`.
- The tick seam exists but is unused for simulation: `Layer.onUpdate(TimeSpan)`
  (`app.cppm:18`, `TimeSpan{f32 seconds}` `core.cppm:62`); the host's `onUpdate` only pumps
  `pollControl` (`host.cppm:562-568`). `updateWorldTransforms` already runs inside
  `renderScene` (`assets.cppm:833`), so the rendered scene's world transforms stay fresh
  without the tick doing anything yet.
- `SceneEditContext` (`scene_edit_context.cppm:102-121`) is the editor-session state: `scene`,
  `registry`, `selected`, `onSelectionChanged` (fired via `.publish`,
  `scene_edit_context.cpp:22`), `camera`, and the `sceneVersion`/`selectionVersion` stamps the
  editor's reconcile poll diffs against. Play state slots in beside them; `playState` is
  session policy and must never serialize into the project.
- Control commands register via `registerCommand<Params, Result>` in
  `control_commands_scene.cpp`; handlers receive `EngineContext` (`command.cppm:29`) and read
  `ctx.sceneEdit.scene` literally throughout — including local `Scene&` aliases, second-stage
  `entityRefDto(ctx.sceneEdit.scene, …)` reads, and helpers (`pickBillboard`) that read
  `editor.scene` inside their bodies. The phase-3 sweep must reach all of them, plus the
  scene-touching asset commands (`assign-asset`/`asset-usages`/`delete-asset` in
  `control_commands_asset.cpp`). `resolveEntity` lives at `control_server.cpp:72`
  (decl `command.cppm:80`) and inlines the only uuid→entity loop in the codebase — phase 1
  extracts it as a reusable helper. `SelectionResult{selectionVersion, sceneVersion, entity?}`
  is at `control_dto.cppm:619-624`.
- DTO serde, the TS protocol, and the generated schemas all come from the catalog in
  `tools/gen-control-dto/gen.ts` (commands array + `commandFixtures`/`commandSkips`);
  `tools/check-control-schema/check.ts` is the live-vs-schema contract test wired into
  `tools/ci/check.sh`. `save-scene`/`load-scene` are `control_commands_asset.cpp:661/:678`.
- The editor learns engine state by a focus-gated ~6 Hz poll, not push: the fast lane is
  `Promise.all([getSelection, renderStats, getGizmo])` (`store.ts:643`), and a `sceneVersion`
  change triggers `refreshHeavyState` (`store.ts:529`, invoked at `:591`). Folding
  `playState`/`playVersion` into `SelectionResult` rides this for free.
- `Topbar.tsx` is the placement and the pattern: grouped icon buttons
  (`rounded-md border border-border p-0.5`, lucide icons), optimistic store write + fire the
  command + reconcile poll repairs (`Topbar.tsx:24-31`). Hotkeys follow
  `useGizmoShortcuts.ts`.
- The e2e harness (`tests/e2e/harness.ts`, used by `hierarchy.test.ts`) boots a headless
  engine with `SAFFRON_AUTO_EMPTY_PROJECT=1`, drives typed `engine.call(...)`, and asserts a
  validation-clean log — exactly the shape `play.test.ts` needs.
- The engine has no scripting, no physics, no animation: in v1 the play scene holds still
  until those systems arrive. The user-visible payoff of play mode today is the camera cut,
  the runtime-vs-authored split, and the seams.
