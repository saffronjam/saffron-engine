# Phase 5 — control plane + `se` CLI + e2e + `animationVersion`

**Status:** NOT STARTED

## Goal

Make animation **scriptable and testable** over the JSON control plane, and add the e2e gate (plus the
rigged fixture it needs). Add typed DTOs + commands — `list-clips`, `play-animation`, `pause-animation`,
`seek-animation`, `set-animation-loop`, `get-animation-state` — register `AssetTypeDto::Animation`, add an
**`animationVersion`** stamp so the editor reconcile poll (Phase 12) can cheaply detect changes, and make
a paused `seek` refresh the pose immediately via the Phase-3 one-shot eval seam. This is the natural cut
line before any UI: the whole runtime is drivable from a shell and covered by a headless test.

## What exists to build on

- `registerCommand<Params, Result>(reg, name, help, handler)` (`command.cppm:52-73`); handlers receive
  `(EngineContext&, const Params&) -> Result<ResultDto>`. Registered in `registerBuiltinCommands`
  (`command.cppm:87-92`) via `registerRenderCommands`/`registerSceneCommands`/`registerAssetCommands`.
- DTO structs are plain-field structs in `control_dto.cppm` (e.g. `PlayStateResult` `:954`, `{ state,
  playVersion, sceneVersion, hasPrimaryCamera }`); `gen.ts` regex-parses this file. `EntitySelector`/
  `AssetSelector` (`:26-34`) accept id-or-name.
- `AssetTypeDto` enum (`control_dto.cppm:125`) = `{ Mesh, Texture, Other }`; `enumWireNames` in
  `gen.ts:88` maps each to a wire string — **missing an entry throws at gen time**.
- Version-stamp pattern: scene mutators bump `ctx.sceneEdit.sceneVersion += 1` before returning
  (`control_commands_scene.cpp:206`); `PlayStateResult.playVersion` + `SelectionResult` carry stamps the
  editor polls. Play commands `play`/`pause`/`step`/`stop`/`get-play-state` already exist
  (`control_commands_scene.cpp:906-968`).
- `gen.ts` (`tools/gen-control-dto/gen.ts`) regenerates **four** files: `control_dto_serde.generated.cpp`,
  `editor/src/protocol/se-types.ts`, `schemas/control/openrpc.generated.json`,
  `command-manifest.generated.json`. Run `bun run tools/gen-control-dto/gen.ts` (or `bun run gen:protocol`
  in `editor/`). **Never hand-edit the generated files.**
- `se` CLI (`tools/se/source/main.cpp`) auto-integrates registered commands (JSON-over-socket + arg
  coercion). e2e harness: `Engine.boot({env})` → `engine.call<T>(cmd, params)` → `engine.settle(ms)` →
  `engine.validationErrors()` asserted empty at shutdown (`tests/e2e/control.test.ts:10-110`).
- Entity ids wire as **decimal JSON strings**, never numbers (precision > 2^53).

## Work

### 1. DTOs (`control_dto.cppm`)

```cpp
struct AnimationClipDto { WireUuid id; std::string name; f32 duration; };
struct ListClipsParams { EntitySelector entity; };          // clips available to this rig (its catalog refs)
struct ListClipsResult { std::vector<AnimationClipDto> clips; };
struct PlayAnimationParams { EntitySelector entity; AssetSelector clip; f32 speed = 1.0f; bool loop = true; f32 blend = 0.0f; };
struct SeekAnimationParams { EntitySelector entity; f32 time; };
struct SetAnimationLoopParams { EntitySelector entity; std::string wrap; };  // "once"|"loop"|"pingpong"
struct AnimationStateParams { EntitySelector entity; };
struct AnimationStateResult {
    WireUuid clip; std::string clipName; f32 duration; f32 time;
    bool playing; std::string wrap; f32 speed; i32 animationVersion;
};
```

Add `Animation` to `AssetTypeDto` (`:125`) and `{ Animation: "animation" }` to `enumWireNames`
(`gen.ts:88`). Add `i32 animationVersion` to `PlayStateResult` (`:954`) and `SelectionResult` (`:?`) so
the reconcile poll can gate on it like `playVersion`.

### 2. Commands (`control_commands_animation.cpp`, new TU)

Add `void registerAnimationCommands(CommandRegistry&)` declared in `command.cppm` and called from
`registerBuiltinCommands`. Register:
- `get-animation-state` → reads the entity's `AnimationPlayerComponent` + the catalog entry's name/duration.
- `list-clips` → the `AssetType::Animation` catalog entries (optionally filtered to those compatible with
  the entity's skeleton; v1 may return all animation assets).
- `play-animation` → resolves `clip` (AssetSelector), sets `playing=true`, `speed`, `wrap=loop?Loop:Once`,
  and **`previewInEdit=true`** (so it previews in Edit, not only Play); starts a transition if `blend>0`
  (Phase 4). Bumps `animationVersion`.
- `pause-animation` → `playing=false` (keeps `previewInEdit` so the paused pose stays shown). Bumps
  `animationVersion`.
- `seek-animation` → sets `time` (and `previewInEdit=true` if not already). No special eval call needed —
  the Phase-3 evaluator runs every frame in both Edit and Play and re-samples at the new `time`, so the
  pose updates next frame whether playing, paused, or Edit-previewed. Bumps `animationVersion`.
- `set-animation-loop` → sets `wrap`. Bumps `animationVersion`.
- (Optional) `stop-preview` → clears `previewInEdit` + `playing` so the rig reverts to rest pose in Edit.

Add an `animationVersion` counter to `SceneEditContext` (next to `playVersion`, `scene_edit_context.cppm:204`).
Each animation mutator increments it; include it in `PlayStateResult`/`SelectionResult`. Commands that
write the **edit** scene's component (when not playing) should also bump `sceneVersion` so the inspector
refreshes.

### 3. Regenerate + editor client + e2e

- Run `gen.ts`. Confirm `se-types.ts`, OpenRPC, manifest, and serde regenerated.
- Add typed wrappers in `editor/src/control/client.ts` (`client.getAnimationState`, `listClips`,
  `playAnimation`, `seekAnimation`, etc.) following the `call<C>(cmd, params)` overload pattern (`:106-117`).
- `tests/e2e/animation.test.ts`: `Engine.boot({SAFFRON_AUTO_EMPTY_PROJECT or a project with the rigged
  fixture})`; import/add the rigged entity; `list-clips`; `play-animation`; `settle(200)`;
  `get-animation-state` and assert `time` advanced + `playing`; `seek-animation` to a fixed time and assert
  the reported `time`; assert `validationErrors()` empty at shutdown.

## Validation (done criteria)

- `make engine` + `make schema` (control-schema contract test) + `make e2e` green.
- `bun run check` (editor) regenerates protocol clean and typechecks.
- `se play-animation <entity> <clip> --loop` then `se get-animation-state <entity>` shows advancing time
  in a running `make run-engine`.
- `docs/`: add a control-plane reference row + a short "driving animation from `se`" how-to.

## Notes / gotchas

- **Bump the right counter.** Play/pause/seek/loop are not structural scene edits, so they bump
  `animationVersion` (and `playVersion` is untouched). Editing the component in Edit mode (assigning a
  clip in the inspector) bumps `sceneVersion`. Phase 12's poll keys on `animationVersion` separately.
- `AssetTypeDto` is a **wire-contract change** — the manifest contract test validates byte-exact
  round-trips, so regenerate and re-run `make schema` or the test will flag the stale enum.
- `seek-animation` works the same in Play, Paused, and Edit-preview: it only sets `time` (+ marks
  `previewInEdit`); the Phase-3 evaluator runs unconditionally each frame and re-samples, so the pose
  updates next frame in every state. There is **no** paused-only one-shot eval path. Test that a seek in
  Edit (with `previewInEdit`) updates the pose without entering Play.
- The reconcile poll only runs when `document.hasFocus() && phase==='ready'` — e2e must `settle()` to
  reach ready before calling commands.
