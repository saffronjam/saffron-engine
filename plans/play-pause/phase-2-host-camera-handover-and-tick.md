# Phase 2 — Host: camera handover + runtime tick driver

**Status:** NOT STARTED

**Depends on:** phase 1.

The host stops hardcoding "the edit scene through the fly-cam" and becomes play-state aware:
render the active scene through the right camera, drive `tickPlay` every frame, and keep the
edit-only overlay (gizmo + billboards) out of play mode. This phase makes play mode *visible*;
it is still only reachable programmatically until phase 3 adds the commands.

## Render path (`host.cppm:574-589`, `layer.onUi`)

Today: catalog set on `state->editor->scene`, camera always `sceneEditCameraView`, render
always `state->editor->scene`, overlay always submitted. Becomes:

```cpp
layer.onUi = [state, &app]()
{
    se::SceneEditContext& ed = *state->editor;
    se::Scene& live = se::activeScene(ed);
    live.catalog = &state->assets.catalog;        // on the ACTIVE scene (was ed.scene)
    se::setViewportDesiredSize(app.renderer, app.window.width, app.window.height);
    se::updateSceneEditCamera(ed.camera, false, 0.0f);
    se::syncNativeGizmo(ed);
    se::CameraView cam = se::sceneEditCameraView(ed.camera);
    if (ed.playState != se::PlayState::Edit)
    {
        if (se::CameraView game = se::primaryCamera(live); game.valid)
        {
            cam = game;                            // the play camera cut
        }                                          // else: fly-cam fallback, never black
    }
    if (app.window.width > 0 && app.window.height > 0)
    {
        se::renderScene(app.renderer, live, state->assets, cam);
        if (ed.playState == se::PlayState::Edit)
        {
            se::submitNativeGizmo(ed, app.renderer, cam, app.window.width, app.window.height);
        }
    }
};
```

Notes:

- `primaryCamera` is evaluated per frame against the play scene, so a camera animated or
  re-flagged during play (live tuning) is honored — same as Unity's Game view.
- The fly-cam fallback covers `hasPrimaryCamera == false`; the *warning* is editor UI
  (phase 5) fed by the flag captured at `enterPlay` (phase 1) — the host never blocks play.
- Gizmo and billboards (`submitNativeGizmo`, `host.cppm:362-369`) are edit-only: UE/Unity hide
  editor chrome inside the game view, and the gizmo writes transforms the duplicate would
  swallow confusingly mid-simulation. `updateSceneEditCamera` keeps running so the fly-cam is
  warm for the instant the user stops (and for the fallback).

## Tick driver (`host.cppm:562-568`, `layer.onUpdate`)

```cpp
layer.onUpdate = [state, &app](se::TimeSpan dt)
{
    if (state->control != nullptr)
    {
        se::pollControl(*state->control, app.window, app.renderer, *state->editor, state->assets);
    }
    se::tickPlay(*state->editor, dt.seconds);
};
```

Control first, tick second: a `play`/`pause`/`step` command that arrives this frame takes
effect this frame (a `step` issued while paused runs its tick in the same frame, so the
single-step feels immediate).

## Pointer input during play (`host.cppm:551-558` + `handleNativeGizmoPointer`, `:373-426`)

The SDL event sink currently computes the fly-cam view and runs gizmo hover/drag + ray-pick.
During play: skip the gizmo paths entirely (hover, drag-start, drag), but keep click-select —
billboard pick + `pickEntity` run against `activeScene(ed)` with the *rendered* camera so
clicking what you see selects the runtime entity (Unity allows selection during play; the
control-plane `pick` command gets the same routing in phase 3). Two traps the sweep must not
miss:

- The **sink closure itself** hardcodes `cam = sceneEditCameraView(state->editor->camera)`
  (`host.cppm:555`). During play the rendered eye is `primaryCamera(activeScene(ed))`, so the
  sink must select `cam` exactly the way `onUi` does, or every click ray-casts from the wrong
  camera. Factor the camera choice into one small helper both lambdas call.
- `pickSceneEditBillboard` and `buildSceneEditBillboards` read `editor.scene` *inside their
  bodies* (`host.cppm:282-290/:334-342`) — changing call sites does nothing. Re-route their
  bodies through `activeScene(editor)` (the build path is edit-only after the gating above, but
  the pick path runs during play). The same body-not-call-site rule applies to the phase-3
  control sweep.

The gizmo early-out belongs at the top of `handleNativeGizmoPointer` so the sink stays one
code path.

## Touched

| What | File | Symbols |
|---|---|---|
| Render + camera + overlay gating | `engine/source/saffron/host/host.cppm` | `layer.onUi` |
| Tick driver | `engine/source/saffron/host/host.cppm` | `layer.onUpdate` |
| Pointer gating + pick camera | `engine/source/saffron/host/host.cppm` | the SDL sink closure, `handleNativeGizmoPointer` |
| Helper-body routing | `engine/source/saffron/host/host.cppm` | `pickSceneEditBillboard`, `buildSceneEditBillboards` |

## Verify

- Build clean; `SAFFRON_EXIT_AFTER_FRAMES=60` headless smoke validation-clean.
- Temporary manual check until phase 3 lands (then delete it): trigger `enterPlay` after
  project load under a flag or debugger and confirm the viewport cuts to the scene camera
  (add one via the camera preset first), gizmo/billboards vanish, `stopPlay` cuts back; with
  no `CameraComponent`, play keeps the fly-cam view.
- Play→stop cycles leak nothing: the `playScene` registry drops on reset; GPU refs were never
  duplicated (`AssetServer` owns them, `assets.cppm:40-41`).
