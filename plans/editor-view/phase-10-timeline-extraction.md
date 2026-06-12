# Phase 10 — timeline extraction

**Status:** NOT STARTED

## Goal

A pure editor refactor with **no behavior change**: split the Phase-12 (animations plan)
`TimelinePanel` into target-agnostic pieces the rig editor can also mount. The dock Timeline is
coupled to the global selection through exactly four store reads; everything below them — the
canvas, the scrub pipeline, the transport — is already target-agnostic. After this phase the dock
panel renders the shared pieces against the selection exactly as today, and phase 11 mounts the
same pieces against the preview rig.

**Do not mount the dock's panel instance in the rig editor** — the tabsystem-revamp will portal
dock panels into per-panel hosts, and a second mount of the same panel id would fight its
render-once registry (README coordination rules). Shared components, two mounts.

## What exists to build on

- The four couplings (`TimelinePanel.tsx:78-84`): `selectedId` (command target),
  `animationState` (`store.ts:151-154`, poll-filled mirror), `animationClips` (`store.ts:155-157`),
  `componentsBySelected` (the rig gate `isRiggedEntity`, `TimelinePanel.tsx:71-75`).
- Already target-agnostic: `TimelineCanvas` (`timelineCanvas.ts` — imperative
  `setSize`/`setModel`/`setPlayhead`, rAF-coalesced, `bars|diamonds` modes); the scrub pipeline
  (`useScrubValue` `useScrubValue.ts:21-71` → `makeCoalescer` 50 ms → `seekAnimation`,
  `TimelinePanel.tsx:100-119`); the pointer-capture scrub surface (`:281-325`); the transport
  button group (`:333-438`); the footer.
- The driving effect (`:140-247`): one store subscription diffing
  `animationClips`/`componentsBySelected`/`animationState` references → `applyModel` vs
  `applyPlayhead` — the piece that becomes source-injected.
- The state source today: `refreshAnimation(selectedId)` (`store.ts:1216-1240`) filled by the
  poll's `animationVersion` gate (`store.ts:1410-1416`); the preview rig rides the **same** path
  because entering the preview selects it (phase 4) — the source interface must still be explicit
  so the two mounts don't share UI state (active scrub, loop optimism).

## Work

### 1. Define the seams

A props interface, not a context (two mounts, no tree sharing):

```ts
interface TimelineTarget {
  entityId: string | null;            // command target (scene selection / preview rig)
  state: AnimationStateResult | null; // playhead/clip/wrap mirror
  clips: AnimationClipDto[];          // for the clip Select (dock) — rig editor hides it (phase 9 owns picking)
  enabled: boolean;                   // replaces the componentsBySelected rig gate
}
```

Command sinks stay `client.*` (both targets are scene entities in the active scene — the commands
are identical); only the id is injected via the existing `selectedRef` pattern
(`TimelinePanel.tsx:102-111`).

### 2. Split the components

- `TimelineTransport` (buttons + loop + optional clip Select), `TimelineSurface` (track headers +
  canvas + scrub surface + footer; owns the `TimelineCanvas` instance and the driving effect, fed
  by `TimelineTarget`), both under `editor/src/components/timeline/`.
- `TimelinePanel` (the dock panel) becomes a thin composition: builds the target from the four
  store reads (verbatim semantics, including the `isRiggedEntity` gate) and renders
  Transport + Surface. Identical DOM/classes — this is a move.

### 3. Guard the singletons

Each `TimelineSurface` mount owns its own `TimelineCanvas`, scrub state, and coalescer (already
per-mount via `useMemo([])`) — verify nothing module-level leaks between mounts (the
`previewCache`-style module caches in other panels are the anti-pattern to avoid here).

## Validation (done criteria)

- `bun run check` + `bun run lint` clean; no new warnings.
- **No behavior change** in the dock: select a rig → tracks/clip/duration appear; play/scrub/loop
  round-trip; deselect → empty state; the render-frequency property holds (playhead advances with
  zero React re-renders — re-verify with the dev render logger, the original Phase-12 gotcha).
- `make e2e` unaffected.

## Notes / gotchas

- The clip `Select` stays a Transport option (the dock keeps it; the rig editor disables it in
  favor of the clip list panel) — one component, presence-flagged, rather than two transports.
- Keep the CSS-var row-height sync (`--ruler-h`/`--row-h`, `TimelinePanel.tsx:183-188`) inside
  `TimelineSurface` — both mounts need it and it is easy to drop in the split.
- `useScrubValue.end()` flush-before-release ordering (`useScrubValue.ts:66-69`) is load-bearing;
  the split must not reorder the pointer-up handler.
- Resist adding rig-editor features during the split — zoom/pan, per-bone lanes, key diamonds all
  come later through the prepared seams (`LaneMode`, `secToX`).
