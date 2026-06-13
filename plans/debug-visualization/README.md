# Debug Visualization

**Status:** IN PROGRESS

Viewport debug visualization for the editor, drivable over the control plane + `se` CLI and surfaced in
the Render panel. Two axes of control, all **transient** (never persisted to the project, never undoable):

- **Debug overlays** (additive) — `set-debug-overlays {bounds?, sceneAabb?, lightVolumes?, grid?}`,
  state in `SceneEditContext.debugOverlays`.
- **View mode** (mutually exclusive render output) — `set-view-mode {lit|wireframe|…}`, state on
  `Renderer`, echoed back through `render-stats.viewMode` (no `get-view-mode`).

## Why

`pickEntity` (`assets.cppm`) is AABB-only and skips skinned meshes, so the picker over-selects rotated
meshes and never hits rigs; the pick / cull / shadow-fit volumes are computed and discarded, never shown.
This feature makes those volumes visible (`bounds` doubles as the picking debugger) and adds wireframe +
buffer-channel render outputs for general debuggability. Fixing picking itself is a separate follow-up
this only makes visible.

## Command surface

| Command | Shape precedent | State home | Persisted | Undoable |
|---|---|---|---|---|
| `set-view-mode` / read via `render-stats` | `set-aa` (enum verb) | `Renderer` | no | no |
| `get-debug-overlays` / `set-debug-overlays` | `set-skeleton-overlay` (grouped optional) | `SceneEditContext` | no | no |
| `RenderStatsDto.viewMode` (append LAST) | — | — | — | — |

Enums/flags **only ever list implemented values** — each grows in the phase that wires it, so no command
accepts a flag it ignores.

## Phases

1. [phase-1-overlays.md](phase-1-overlays.md) — debug overlays (bounds, sceneAabb, lightVolumes) + command scaffolding. **No PSO/shader/render-graph work.**
2. [phase-2-wireframe.md](phase-2-wireframe.md) — wireframe view mode (`fillModeNonSolid` + `polygonMode=eLine` PSO variant).
3. [phase-3-grid.md](phase-3-grid.md) — infinite analytic grid (depth-tested fullscreen render-graph pass).
4. [phase-4-channels.md](phase-4-channels.md) — buffer-channel view modes (albedo/normal/roughness/metallic/emissive/depth/AO/motion/overdraw/light-complexity).

Each phase is its own commit behind the milestone gate (`make engine` + `make prepare-for-commit`, e2e,
`tools/check-control-schema`).

## Codegen checklist (every command/enum-touching phase)

Edit `control_dto.cppm` (DTO structs **and** the hand-maintained `dtoToJson`/`parseDto` forward
declarations) → `tools/gen-control-dto/gen.ts` (`commands[]`, `enumWireNames` for new enums with
kebab-case wire names, `commandFixtures` map entry **and** a defined fixture body) → `bun run gen:protocol`
→ commit the 5 generated outputs → `client.ts` wrappers → build + lint → `tools/check-control-schema`
green vs a live engine. A missing `enumWireNames` entry serializes an enum as an int and fails the
contract test; a `commandFixtures` entry without a defined body aborts `gen:protocol`.
