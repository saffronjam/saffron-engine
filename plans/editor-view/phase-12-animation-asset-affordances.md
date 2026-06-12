# Phase 12 — animation-asset affordances

**Status:** NOT STARTED

## Goal

Make rig/animation assets feel first-class everywhere they appear: real icons instead of the `File`
fallback, a duration badge on clip tiles, sensible double-click routing, and clean empty/error
states — the polish that turns the feature from "reachable" into "discoverable". (UE5's asset
browser color-codes and tooltips animation assets; we do the lightweight equivalent.)

## What exists to build on

- Both icon fallthroughs are explicit: the tile `TypeIcon` (`AssetTile.tsx:92-101` — mesh→Box,
  texture→ImageIcon, else File) and the tab `tabIcon` (`WindowTitlebar.tsx:425-444` — same shape);
  `"animation"` exists in the type union (`se-types.ts:978-984`) but no surface branches on it.
- Clip duration is already on the catalog row (`assets.cppm:298-303`) and the wire via `list-clips`
  (`AnimationClipDto`), and on `get-rig` clip entries (phase 2) — the tile needs it on
  `AssetEntryDto` or fetched lazily; the DTO addition is one optional field + regen (the phase-2
  recipe; precedent says keep the DTO lean, but duration is display data the grid genuinely needs —
  add it as optional, omitted for non-animation rows).
- Thumbnails: `Animation` assets have none (`control_commands_asset.cpp:401-403`); the tile shows
  the icon. A posed-rig thumbnail (render the linked rig mid-clip) is possible after phase 4 but is
  one-shot PNG work with real cost — explicitly deferred; the icon + duration badge is v1.
- Routing today (phase 7): animation double-click → rig editor; mesh double-click → image viewer,
  context-menu "Open in Rig editor".
- Error states: `get-rig`'s stable no-sidecar error keyed to the migration affordance (phases 3/7).

## Work

### 1. Icons + badges

- `TypeIcon` + `tabIcon`: `animation` → a film/clapper lucide icon (match the Topbar timeline
  trigger's `Clapperboard` for vocabulary consistency); rigged mesh assets keep `Box` (no async
  rig probing in the icon path).
- Clip tiles: a bottom-right duration badge (`Badge` ui primitive, `formatTime` from the timeline)
  when duration is present.

### 2. Routing completeness

- Mesh assets that **have** a rig: make double-click open the rig editor instead of the image
  viewer. "Has a rig" must be synchronously known to the click handler — expose the **persisted**
  `rigged` catalog key (phase 2 stores it on import/migrate, so it survives project reload — unlike
  a stamp-on-mutate-only flag) on `AssetEntryDto` as an optional field, read straight from the
  grid's `AssetEntry`. Unrigged meshes keep the image viewer. Context menus offer both
  ("View image" / "Open in Rig editor") for rigged meshes.
- A clip whose rig is unresolvable (old project, never migrated — `get-rig { asset: clipId }`
  errors): double-click opens the rig editor's error state with the migrate affordance — not a dead
  toast. (Phase 7 already routes clip-open through `get-rig`; this phase just ensures the error
  surface is the workspace, not a swallowed rejection.)

### 3. State polish

- Workspace empty/error states reviewed as a set: no-sidecar (migrate action), asset deleted while
  tab open (the `setAssetList` re-title path leaves a stale tab — close it like the delete flow
  does, `closeViewTab(\`asset:${asset.id}\`)` in `confirmDeleteAssets`, `AssetsPanel.tsx:614`),
  engine restart mid-preview (the watchdog flow lands back on the scene tab; re-entering re-spawns).
- Tooltips only where meaning is non-obvious (`editor/AGENTS.md`): the duration badge and type
  icons need none; the migrate action gets one explaining what migration does.

## Validation (done criteria)

- `bun run check` + `bun run lint` clean; `make engine` + contract test clean (the DTO fields).
- Manual (`make run`): the Assets grid shows clip icons + durations; double-click matrix — rigged
  mesh → rig editor, unrigged mesh → image viewer, clip → rig editor with that clip active,
  unlinked clip → error state with migrate.
- `make e2e`: `list-assets` rows carry `rigged`/`duration` where expected.
- `docs/`: the rig-editor page's "opening" section updated with the final routing.

## Notes / gotchas

- `rigged` is **persisted** (phase 2's catalog key), so `list-assets` reads it straight off the
  in-memory catalog — no per-list sidecar stat (the command runs on every reconcile burst), and it
  survives project reload (`catalogFromJson` rebuilds the catalog from JSON only; a stamp-on-mutate
  flag would be lost on open). This phase only adds the wire DTO field + the routing that reads it.
- Surfacing `rigged`/`duration` on `AssetEntryDto` means the contract schemas regenerate — remember
  `additionalProperties: false` makes a forgotten regen a hard contract failure, which is the point.
- Posed-rig thumbnails for clips stay deferred; if/when added they reuse `enter-rig-preview` +
  the capture path offline, not a new render pipeline.
