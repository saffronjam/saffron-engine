# Phase 9 — clip list + details panel

**Status:** NOT STARTED

## Goal

Fill the workspace's right panel: the rig's **linked clip list** (the skeleton-filtered asset
browser every engine docks beside the preview) and a **details** section for whatever is focused —
the clip (duration, track count, wrap) or the rig (joint count, mesh link). Clicking a clip switches
what the preview plays. This is the panel that makes multi-clip imports usable: today only the
first clip is reachable (the import binds it to the player; the rest are orphans in the Assets grid).

## What exists to build on

- The data: `get-rig` (phase 2) returns `clips: AnimationClipDto[]` (id, name, duration) — the
  linked set, not the whole catalog; `get-animation-state` reports the active clip + wrap + time
  (`control_commands_animation.cpp:154-169`).
- Switching: `play-animation { entity, clip, ... }` swaps the player's clip (with optional blend,
  `:186-218`); the preview rig is the selected entity, so the existing client wrappers
  (`client.playAnimation`, `client.ts:241-247`) work unchanged.
- List UI precedents: the dock's clip `Select` in `TimelinePanel.tsx:402-418` (a dropdown — the
  rig editor upgrades this to a list panel); `AssetTile`'s row styling; `ScrollArea`.
- Details rendering: the Inspector's field kit (`NumberDrag`, `fieldRenderer.tsx`,
  `humanizeFieldName` for field labels) — read-only values in v1, but rendered with the same
  components so a later editable details panel is a prop flip, not a rewrite.
- UE5 reference behaviors worth keeping: rows show name + duration; the active clip is visually
  marked; double-click focuses playback at t=0. Color-coding by asset type and hover-preview
  tooltips are deferred.

## Work

### 1. The clip list

`RigClipList { clips, activeClipId, onPick }`: a `ScrollArea` of thin rows — name, right-aligned
`duration` (formatted like the timeline footer), active row highlighted with the semantic accent
(`bg-accent`). `onPick` → `play-animation` on the preview rig **paused at t=0** (pick ≠ play:
match UE5 — selecting a clip loads it at frame 0; the transport plays it). Reflect the engine's
truth from the poll (`animationState.clip`), optimistic row highlight in between.

### 2. The details section

Below the list (`Tabs variant="line"` if it needs to split later; a plain stacked section for v1):
- Clip focus: name, duration, wrap (from `get-animation-state`), track count (extend
  `AnimationClipDto` or `get-rig`'s clip entries with `tracks` — one i32, trivially available at
  bake time and worth persisting on the catalog row like `duration` was).
- Rig focus (no clip selected): mesh name, joint count, bone count, linked-clip count.
Labels through `humanizeFieldName`; values read-only.

### 3. Empty/edge states

No linked clips → "No clips imported with this rig." with a muted hint that `migrate-rigs`/re-import
links them (phase 3); clip rows whose asset was deleted since linking render struck-through and
unpickable (the catalog link is stale — `get-rig` filters or flags them).

## Validation (done criteria)

- `bun run check` + `bun run lint` clean; `make engine` clean if `tracks` rides the catalog/DTO
  (codegen + fixtures updated).
- Manual (`make run`): a two-clip rig (extend the leg fixture or author a second clip) lists both;
  picking the second loads it paused at 0; the timeline (phase 11) and footer reflect it; the
  active marker follows engine truth after a CLI-side `se play-animation` switch.
- `make e2e`: `get-rig` clip entries carry `tracks`; pick-then-state round-trip (`play-animation`
  paused semantics — assert `playing == false`, `time == 0`, `clip` switched).
- `docs/`: the rig-editor page gains the clip-list/details section.

## Notes / gotchas

- **Pick must not blast the wire**: a click is one `play-animation` call — no coalescer needed for
  discrete picks, but debounce double-fire from row double-click vs single-click handlers.
- The "paused at t=0" pick semantics need a small engine nuance: `play-animation` sets
  `playing = true` (`:214`) — add an optional `paused` param (codegen touch) or follow with
  `pause-animation` + `seek-animation 0` (two extra serialized calls; acceptable, zero engine
  change). Prefer the param: one call, explicit semantics, and the CLI gains it too.
- Deleting a clip asset while it is the preview's active clip: the player's clip resolves to
  nothing — the evaluator already handles a missing clip (rest pose); the list re-renders from the
  next poll. No special casing beyond the struck-through row.
