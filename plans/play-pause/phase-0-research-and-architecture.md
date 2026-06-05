# Phase 0 — Research and architecture decision

**Status:** COMPLETED

How UE5, Unity, Frostbite, Godot, and Hazel implement play-in-editor; which model fits this
codebase; and the decisions every later phase builds on. Research was done against primary
sources (engine docs, EA/DICE talks, Hazel source) — links inline.

## How the major engines do it

### Unity — in-place serialize/restore

Entering play mode performs a [scene reload](https://docs.unity3d.com/6000.3/Documentation/Manual/scene-reloading.html)
(destroy all scene objects, deserialize a fresh copy — the on-disk scene *is* the snapshot)
plus a [domain reload](https://docs.unity3d.com/6000.3/Documentation/Manual/domain-reloading.html)
(reset all script statics). Play then mutates the *same* scene objects in place; stop discards
the in-memory copy and re-deserializes. Consequences Unity owns:

- Everything edited during play is silently discarded on stop; the
  [play-mode tint](https://whitepotstudios.com/blog/unity-tip-play-mode-tint/) exists solely to
  stop users losing work to this.
- Editing during play is *allowed* (Inspector, hierarchy, gizmos all live) — the discard is
  the safety mechanism, not a lock.
- Pause halts the player loop before the next frame; [`EditorApplication.Step()`](https://docs.unity3d.com/ScriptReference/EditorApplication.Step.html)
  runs exactly one full frame then re-pauses. `Time.timeScale = 0` is a separate, orthogonal
  mechanism (Update still runs, FixedUpdate does not).
- The lifecycle is a four-edge event enum ([`PlayModeStateChange`](https://docs.unity3d.com/ScriptReference/PlayModeStateChange.html)):
  `ExitingEditMode → EnteredPlayMode → ExitingPlayMode → EnteredEditMode`; pause is a separate
  axis. Editor subsystems subscribe rather than poll.
- Disabling the reloads for iteration speed ([Enter Play Mode Options](https://docs.unity3d.com/6000.2/Documentation/Manual/configurable-enter-play-mode.html))
  is the canonical source of state-leak bugs: statics keep last-run values, event handlers
  double-subscribe. The lesson: restore correctness must not depend on every system resetting
  itself.
- Game view renders from the scene's `Camera` component; no camera → a "No cameras rendering"
  warning, not a silent black screen.

### Unreal Engine 5 — world duplication (PIE)

Starting PIE calls [`UWorld::DuplicateWorldForPIE`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/UWorld/DuplicateWorldForPIE):
the editor world is cloned into a separate PIE world (per-instance name prefix `UEDPIE_N_`,
tracked by `FWorldContext`), gameplay runs entirely in the clone, and the editor world stays
["safely immutable"](https://dev.epicgames.com/documentation/unreal-engine/ineditor-testing-play-and-simulate-in-unreal-engine).
Stop = garbage-collect the duplicate; there is no restore step at all.

- Runtime changes are discarded by default; [Keep Simulation Changes](https://docs.unrealengine.com/4.26/en-US/BuildingWorlds/LevelEditor/InEditorTesting/KeepSimulationChanges)
  is the explicit opt-in to copy selected actor state back (only works for actors that exist
  in both worlds — the duplicate model is what makes the feature possible).
- Two play sub-modes: PIE (possess a pawn, game camera) vs Simulate-In-Editor (free editor
  camera, world ticks); F8 eject/possess toggles live. Pause + Frame Skip (1 frame per press)
  in the toolbar.
- A rich delegate sequence (`PreBeginPIE`/`BeginPIE`/`PostPIEStarted`/`EndPIE`/`PausePIE`/
  `SingleStepPIE`, [FEditorDelegates](https://notes.hzfishy.fr/Unreal-Engine/Editor-Only/Types/FEditorDelegates))
  is how editor subsystems flip into play mode — same role as Unity's state events.

### Frostbite (FrostEd) — editor remote-controls the runtime

Public material is sparse but the architecture is verifiable: the editor is a separate C#
application over the C++ runtime ([DICE](https://x.com/frostbiteengine/status/428815128780632064)),
and the data layer is split into tool / storage / runtime schemas bridged by pipeline code
([GDC: A Tale of Three Data Schemas](https://tools.engineer/tools-tutorial-day-a-tale-of-three-data-schemas)).
Live editing works by embedding pipeline code in dev runtimes so authored data hot-transforms
into the running game — the editor never simulates anything itself; instant no-export iteration
is the stated design goal ([EA on PGA Tour terrain](https://www.ea.com/frostbite/news/procedural-terrain-in-ea-sports-pga-tour)).
Exact play/stop semantics are not publicly documented.

### Godot — out-of-process play (the control-plane analogue)

The editor launches the game as a **separate process** and inspects it over a debugger
protocol: the scene dock grows [Remote vs Local tabs](https://docs.godotengine.org/en/stable/tutorials/scripting/debug/overview_of_debugging_tools.html)
— Local is the authored scene file, Remote is the live runtime tree, and edits to Remote
affect only the running instance, never the `.tscn`. Crash isolation for free; the authored
scene is non-destructively safe *by construction*.

### Hazel — the custom-engine reference

[Hazel](https://github.com/TheCherno/Hazel) (TheCherno's C++/entt engine — the closest public
codebase to this one) lands on the same shape at ECS granularity:
[`Scene::Copy`](https://github.com/TheCherno/Hazel/blob/master/Hazel/src/Hazel/Scene/Scene.cpp)
duplicates the editor scene into a runtime scene (matching UUIDs, every component copied),
`OnRuntimeStart/Stop` bracket the session, the editor calls `OnUpdateRuntime(dt)` (renders via
`GetPrimaryCameraEntity()`) vs `OnUpdateEditor(dt, EditorCamera&)`, and the pause/step gate is
one line: `if (!m_IsPaused || m_StepFrames-- > 0) tick();`.

## The decision: duplicate, don't restore

Three candidate architectures were designed independently against this codebase and judged
(in-place snapshot/restore à la Unity; world duplication à la UE5/Hazel; process separation à
la Godot/Frostbite). **World duplication wins for v1**, with the process model's
"editor remote-controls the running scene" framing kept as the wire-contract discipline.

Why duplication beats in-place restore *here*:

- **Restore correctness is structural, not behavioral.** In-place restore is correct only if
  serialization captures everything any runtime system will ever mutate — a standing invariant
  physics/scripting can silently break (Unity's domain reload exists because of exactly this
  class of bug). With a duplicate, the edit scene is never mutably aliased during play; stop is
  `playScene.reset()` — there is no restore step to get wrong.
- **It is nearly free in this codebase.** The expensive part of world duplication elsewhere is
  GPU/asset state; here the GPU caches are uuid-keyed on `AssetServer` (`assets.cppm:40-41`)
  and shared by construction, and `Scene` is a move-only value type around an `entt::registry`
  (`scene.cppm:304-309`). The duplicate costs one registry of component structs plus a
  transient JSON doc.
- **It is the substrate later systems want.** Jolt bodies and script VMs get built against a
  world that is *defined* to be thrown away — no teardown discipline, no leak-across-sessions
  class of bug.
- **It keeps "keep simulation changes" and out-of-process play reachable** — both need the
  authored/runtime split to exist.

Why not out-of-process in v1: a second host means a second swapchain, a second X11 reparent on
already-delicate attach timing, double asset load, and llvmpipe rendering two scenes — all for
isolation the duplicate already provides in-process. The control commands are designed so a
separate play process could serve them later without changing the editor.

## Decisions locked with the project owner

1. **Editing during play: live-tune-and-discard** (Unity/UE model). Panels stay interactive
   and writes route to the play scene; the tint + discard semantics are the guardrail. The
   gizmo stays hidden during play in v1; save/load are locked.
2. **Hotkeys: the Ctrl+P family** (Unity parity). Ctrl+P play/stop toggle, Ctrl+Shift+P
   pause/resume, Ctrl+Alt+P step. (Esc is taken: it closes the host, `host.cppm:592-601`.)
3. **Duplication via the JSON serde first** (`sceneToJson` → fresh `Scene` → `sceneFromJson`).
   Zero new machinery, already self-tested, uuid identity survives by construction. A direct
   registry copy (Hazel `Scene::Copy`) is a swap-in behind the same `enterPlay` API if
   profiling ever shows a play-button hitch on large scenes.

## Design rules every phase follows

- The canonical state machine: `Edit → Playing ↔ Paused → (stop) → Edit`; `step` only from
  `Paused`; entering play while playing is an error; `stop` in `Edit` is idempotent.
- One chokepoint, `activeScene(ctx)`, decides which scene every consumer sees. No consumer
  branches on `playState` to pick a scene itself.
- Pause freezes only the runtime tick. Rendering, present, the control plane, and the editor
  fly-camera always run (UE/Unity both do this; it is what keeps a paused frame inspectable).
- `step` advances a *fixed* tick (deterministic), not a wall-clock frame; a max-dt clamp is
  reserved now so physics never sees a spiked delta later.
- Play state is editor-session policy on `SceneEditContext` — never serialized into the
  project, never a component.
- Transitions fire `onPlayStateChanged` (the `onSelectionChanged` pattern,
  `scene_edit_context.cppm:107`); future physics/scripting subscribe to lifecycle edges
  instead of being called by the host loop.
- The scene with no primary camera falls back to the fly-cam and surfaces
  `hasPrimaryCamera=false` — warn, never render black (Unity's "No cameras rendering").
