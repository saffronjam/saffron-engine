# Phase 3 — Infinite analytic grid

**Status:** COMPLETED

Adds the `grid` flag to `set-debug-overlays` and renders it as a depth-tested fullscreen render-graph pass.

## Flag plumbing
- `DebugOverlayOptions.grid`; `grid?` in `DebugOverlaysParams`, `grid` in `DebugOverlaysResult`;
  regenerate. Host reads `editor.debugOverlays.grid` → `RenderSceneOptions.showGrid` → a per-frame renderer
  flag the graph build checks.

## Shader — `engine/assets/shaders/grid.slang` (new)
Fullscreen-triangle vertex; fragment reconstructs the world ray from `invViewProj`, intersects `Y=0`,
analytic anti-aliased lines via `fwidth`, distance fade, writes color + `gl_FragDepth` (clip-space plane
depth) so scene geometry occludes it.

## Pass — `renderer.cppm`
`RgPass{ kind=Graphics }` added in the graph build (conditional, like SSAO/contact), after the opaque scene
pass and **before tonemap**. `colors = { RgAttachment{ sceneColorAttachment, eLoad, eStore } }` (blend),
`depth = RgAttachment{ sceneDepth, eLoad, eStore }`; `execute` binds the grid pipeline + pushes
`invViewProj` + `cmd.draw(3,1,0,0)`. Declare all access via attachments/`RgAccess`; never hand-write a
barrier. New cached pipeline (mirror tonemap/copy_color/fxaa). Verify before-vs-after-tonemap placement.

## Editor / tests / docs
Grid toggle in the Debug section; e2e grid round-trip + pixel-diff; document the grid + placement.

## Gate
`make engine` + `make prepare-for-commit`; e2e; contract green.
