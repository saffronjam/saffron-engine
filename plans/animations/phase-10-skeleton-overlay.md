# Phase 10 — skeleton line overlay + `show-bones`

**Status:** NOT STARTED

## Goal

Draw a line skeleton over the selected rig in the viewport — bone segments + joint dots + optional RGB
axis lines — reusing the native overlay path with **zero new rendering infrastructure**. Critically, it
must render in **both Edit and Play** (you need to watch bones move while a clip plays), which the current
`PlayState::Edit`-only gizmo guard does not allow. Add a `show-bones` toggle over the control plane so the
editor and `se` can switch it. Engine-side and independent of the runtime — it only needs bones, so it can
land any time after Phase 3.

## What exists to build on

- The overlay producers + submit: `buildNativeGizmo` (`host.cppm:247`) and `buildSceneEditBillboards`
  append `OverlayVertex`es into one `std::vector`; `submitNativeGizmo` (`host.cppm:386`) builds that
  vector then calls `submitOverlay(renderer, std::move(vertices))` (`renderer.cppm:2874`). `OverlayState`
  (`renderer_types.cppm:894`) holds the per-frame buffer ring; the `editor-overlay` graph pass draws it.
- Primitive builders: `addLine(vertices, aPx, bPx, thickness, color, w, h)` (`host.cppm:77`),
  `addCircleFill(vertices, centerPx, radius, color, w, h)` (`host.cppm:170`), `addBox` (`:144`).
  `OverlayVertex` (`renderer_types.cppm:885`) = NDC `position`, `color`, `edge` (feather coords — must be
  packed correctly or it renders garbage).
- Screen-constant sizing: `buildNativeGizmo` uses `distance = length(cameraPosition(cam) - position)` and
  `axisLen = max(0.75f, distance * 0.22f)` (`host.cppm:262`). `viewportProject(cam, w, h, world)`
  (`scene_edit_context.cppm:305`) projects world→pixels; `cameraPosition(cam)` (`:309`).
- Bone source: selected entity's `SkinnedMeshComponent` (`scene.cppm:78`) → `bones[]` resolved via
  `boneHandles[]`; parent links via each bone's `RelationshipComponent.parentHandle` (`scene.cppm:51`);
  `worldTranslation`/`worldRotation` (`scene.cppm:591-606`). Selection lives on `SceneEditContext`
  (`editor.selected.handle`); selected-highlight color used by the gizmo is `{1.0, 0.78, 0.18, 1.0}`.
- The overlay PSO is `depthTestEnable = VK_FALSE` (`renderer_pipelines.cpp:230`) → draws on top
  unconditionally (UE5 `SDPG_Foreground` / Blender "In Front"), which is the v1 default.
- **The Edit-only guard:** `submitNativeGizmo` is called only when `editor.playState == PlayState::Edit`
  (`host.cppm:683`). The skeleton must escape this.

## Work

### 1. `buildSkeletonOverlay`

Add `void buildSkeletonOverlay(SceneEditContext& editor, const CameraView& cam, u32 width, u32 height,
std::vector<OverlayVertex>& vertices)` in `host.cppm`, modeled on `buildNativeGizmo`:
- Guard: the selected entity must have a `SkinnedMeshComponent` (else return). Also honor an enable flag
  (see §3).
- For each bone `i`, resolve `boneHandles[i]` (skip `entt::null`); get `worldTranslation` and
  `viewportProject` it to pixels. For each bone with a parent that is also a joint
  (`RelationshipComponent.parentHandle` carrying `BoneComponent`), `addLine(parentPx, childPx, ~2.0f px,
  boneColor, w, h)`.
- `addCircleFill` a joint dot at each bone pixel, radius scaled screen-constant
  (`max(rMin, distance * k)`), so dots don't vanish when zoomed out. (Line **thickness** stays a pixel
  value — do **not** scale it by distance.)
- Optional RGB axis lines per joint (Blender "Show Axes" / UE "Axis Length"): three short `addLine`s using
  the bone's `worldRotation` basis — nearly free, high value for animators.
- Color: neutral default; the selected entity's skeleton uses a brighter tint; reserve a distinct
  "active/hovered bone" color for the deferred picking work.

### 2. Render in Edit **and** Play

Lift the skeleton submit out of the `PlayState::Edit` guard (`host.cppm:683`). Concretely: keep the
manipulation gizmo (`buildNativeGizmo`) Edit-only, but build + submit the skeleton overlay whenever
`show-bones` is on, in any play state — so a played clip shows its bones moving. Since `submitOverlay`
takes the whole vertex vector, either (a) call `buildSkeletonOverlay` into the same vector inside a submit
that runs in all states, or (b) factor an always-run overlay submit that the Edit-only gizmo appends to.
Keep it one `submitOverlay` per frame.

### 3. `show-bones` control toggle

- Add a `SkeletonOverlayOptions { bool show; bool axes; f32 jointSize; }` to `SceneEditContext`.
- Register a `set-skeleton-overlay` command in `control_commands_animation.cpp` (Phase 5) — DTO +
  `gen.ts` regen — and a `get` for the editor to read current state. `se set-skeleton-overlay --show
  --axes` flips it. (The Phase-12 timeline / a viewport toolbar button can bind it later.)

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean.
- Manual: select a rigged entity → a line skeleton draws on top of the mesh; hit Play → the skeleton
  **moves with the animation**; toggle `se set-skeleton-overlay --show false` → it disappears.
- `make e2e`: a smoke test that `set-skeleton-overlay` round-trips and the log stays validation-clean.
- `docs/`: add `docs/content/explanations/animation/skeleton-overlay.md` (what it draws, on-top default,
  the Edit+Play behavior) + hub row.

## Notes / gotchas

- **`edge` packing is mandatory** — `addLine` packs half-thickness, shapes pack half-extents; copy the
  exact pattern from the existing builders or the overlay renders garbage.
- **Mix pixel vs NDC carefully** — project to pixels, pass pixels to `add*` (they convert to NDC
  internally). Don't pre-convert.
- **Don't touch `nativeGizmo` hover/active** in the skeleton builder — this is read-only visualization;
  bone **picking** (CPU ray-vs-segment reusing `pointSegmentDistance`, reporting over the control plane)
  is a deferred follow-up.
- Multiple visible skeletons would z-fight (no depth test); v1 scopes to the **selected** entity, which
  bounds vertex count and avoids the issue. An occluded/dim-when-behind mode (a second depth-tested PSO +
  flipping the scene depth store-op) is deferred — the README notes it as out of scope.
- Bone **names/labels** need a text system the engine doesn't have; defer to screen-space React labels in
  the editor if ever wanted.
