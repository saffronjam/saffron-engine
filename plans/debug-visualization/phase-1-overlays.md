# Phase 1 — Debug overlays + command scaffolding

**Status:** COMPLETED

Additive geometry in the existing `editor-overlay` pass — no render-graph change. Build the **bounds-only
vertical slice first** (state → command → builder → panel → e2e), then fan out to `sceneAabb` /
`lightVolumes`.

## Engine state — `scene_edit_context.cppm`
`struct DebugOverlayOptions { bool bounds=false; bool sceneAabb=false; bool lightVolumes=false; };`
(after `SkeletonOverlayOptions`), member `DebugOverlayOptions debugOverlays;` next to `skeletonOverlay`.

## DTOs + codegen
- `control_dto.cppm`: `DebugOverlaysParams { optional<bool> bounds, sceneAabb, lightVolumes; }`,
  `DebugOverlaysResult { bool bounds, sceneAabb, lightVolumes; }`, plus their `dtoToJson`/`parseDto`
  forward decls.
- `gen.ts`: `commands[]` += `get-debug-overlays` (`EmptyParams`→`DebugOverlaysResult`) and
  `set-debug-overlays` (`DebugOverlaysParams`→`DebugOverlaysResult`); `commandFixtures` +=
  `["get-debug-overlays","empty"]`, `["set-debug-overlays","debug-overlays-bounds"]` with body
  `{bounds:true}` defined.

## Command handlers — `control_commands_scene.cpp`
`debugOverlaysState(const DebugOverlayOptions&) -> DebugOverlaysResult` (mirror `skeletonOverlayState`);
`get-debug-overlays` returns it; `set-debug-overlays` per-field `if (params.X) opts.X = *params.X;` then
echoes.

## Overlay builder — `host.cppm`
- **Signature change:** thread `Scene& scene, AssetServer& assets` into `submitSceneEditOverlay` + its sole
  call site (~955, where `live` + `state->assets` are in scope). No overload.
- `buildDebugOverlays(SceneEditContext&, Scene&, AssetServer&, const CameraView&, u32 w, u32 h, std::vector<OverlayVertex>& depthTested)`
  after `buildSceneEditCameraFrustums`, hooked inside the `if (editChrome)` guard. All geometry into
  `depthTested`. `viewProjection = cameraProjection(cam,aspect)*cam.view`.
  - `addWorldAabb`: 8 corners → 12 edges via `addClippedOverlayLine` (reuse the frustum `Edges` table).
  - **bounds:** static `MeshComponent` → `loadMeshAsset` → `worldMatrix` → AABB(`boundsMin/Max`);
    skinned `SkinnedMeshComponent` → joint-union box (`jointMatrices` union, as renderScene
    `assets.cppm:5703-5720`), distinct tint.
  - **sceneAabb:** recompute the scene union locally; one box. One-line comment that this intentionally
    mirrors renderScene's discarded AABB.
  - **lightVolumes:** point → 3 world-space great-circle rings (radius=`range`); spot → cone from
    translation along `worldRotation*forward`, apex + base ring from `outerAngle`; directional → skip.

## Editor
- `client.ts`: `getDebugOverlays()`, `setDebugOverlays(opts)`.
- `store.ts`: `debugOverlays` slice (init null, identity-stable setter, no `pushEdit`) + a panel-open-gated
  reconcile branch; fetch on mount / `phase→ready`.
- `RenderPanel.tsx`: a "Debug" separator + switches reusing `ToggleRow`, reading the `debugOverlays` slice,
  optimistic → fire → echo → rollback+notifyError. Bounds tooltip explains pick/joint-union semantics.

## Tests + docs
- `tests/e2e/debug-overlays.test.ts` (clone `skeleton-overlay.test.ts`): defaults off, round-trip, partial
  update, bounds pixel-diff, zero validation errors.
- New `docs/content/explanations/ui-and-editor/debug-visualization.md` + hub `_index.md` row + the
  `tooling-and-control/_index.md` command row.

## Gate
`make engine` + `make prepare-for-commit`; `bun run e2e` (debug-overlays); `tools/check-control-schema`
green; commit the 5 generated outputs.
