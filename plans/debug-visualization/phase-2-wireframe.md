# Phase 2 — Wireframe view mode

**Status:** COMPLETED

Ships `set-view-mode` with `ViewModeDto { Lit, Wireframe }` only.

## DTOs + codegen
- `control_dto.cppm`: `enum class ViewModeDto { Lit, Wireframe };`,
  `SetViewModeParams { optional<ViewModeDto> mode; }`, `SetViewModeResult { ViewModeDto viewMode; }`;
  append `ViewModeDto viewMode;` as the **LAST** member of `RenderStatsDto`; add the forward decls.
- `gen.ts`: `enumWireNames["ViewModeDto"] = { Lit:"lit", Wireframe:"wireframe" }`; `commands[]` +=
  `set-view-mode` (no `get-view-mode`); fixture `["set-view-mode","view-mode-wireframe"]` body
  `{mode:"wireframe"}` defined.

## Renderer state
Engine enum `ViewMode { Lit, Wireframe }` on `Renderer` (DTO mapped at the control boundary);
`setViewMode`/`viewMode` modeled on `setAa`/`aaMode`; **no PSO rebuild on switch**. Not persisted (no
`renderSettingsToJson` entry); resets to `Lit` at construction.

## Device feature — `renderer.cppm`
Query `VkPhysicalDeviceFeatures{ .fillModeNonSolid=VK_TRUE }` via `enable_features_if_present` beside
`pipelineStatsFeat`; store `renderer.context.fillModeNonSolid`. Do **not** request `wideLines`.

## PSO + draw — `renderer_pipelines.cpp`, `renderer_drawlist.cpp`
- `newMeshPipeline(... bool skinned, bool wireframe)`; `raster.polygonMode = wireframe ? eLine : eFill` at
  the **mesh PSO site only** (`:104`). Scope out the other six `eFill` sites.
- Cache key += `"|wireframe"`; `requestMeshPipeline(..., bool skinned, bool wireframe)`, replace call
  sites (2-arg overload forwards `false`).
- `renderer_drawlist.cpp:508`: `bool wf = viewMode(renderer)==Wireframe && context.fillModeNonSolid` (covers
  static + skinned, both route through the non-skinned mesh PSO).

## Command + editor
- `control_commands_render.cpp`: `set-view-mode` beside `set-aa`; append `toDto(viewMode(renderer))` to
  `renderStatsDto()` LAST.
- `client.ts`: `setViewMode(mode)`; `viewMode` arrives via the existing `render-stats` poll.
- `RenderPanel.tsx`: a "View Mode" `Select` beside AA; handler = `onAa` minus `recordRender`/`pushEdit`;
  `VIEW_MODES` lists only implemented modes.

## Tests + docs
e2e `view-mode` test (echo + render-stats + wireframe pixel-diff); extend `debug-visualization.md` with the
View Mode section + the software-GPU `fillModeNonSolid` caveat.

## Gate
`make engine` + `make prepare-for-commit`; e2e; contract green; commit generated outputs.
