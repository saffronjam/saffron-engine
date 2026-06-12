# Phase 09 — polish + docs

**Status:** NOT STARTED

## Goal

Close the UX checklist items deferred from earlier phases, delete what died along the way,
and write the docs page the keep-current rule owes for a new editor concept.

## What exists to build on

- The UX research checklist (README): keyboard/menu fallback for every drag (VS Code
  "Move View", JetBrains "Move to"), tab overflow (JetBrains one-row + chevron), tab
  context menus (Unity/Unreal).
- The phase-04 overflow guard (shrink + truncate) — legible but unbounded below usable
  widths with many tabs.
- `docs/` conventions: one concept per page, front-matter title = H1, slim
  `What | File | Symbols` table, mermaid for diagrams; hub `_index.md` row in the same
  change (root `AGENTS.md`).

## Work

1. **Tab context menu** (the existing shadcn `ContextMenu` primitive,
   `components/ui/context-menu.tsx` — `ContextMenuTrigger asChild` around the tab; dock
   tabs only): "Move to…" with a destination picker listing every non-locked leaf (plus
   "new split left/right/top/bottom of …") driven by `movePanel` — the
   no-drag/keyboard-accessible fallback for every drag operation — and "Close" for
   closable panels.
2. **Tab overflow upgrade:** when shrink hits the minimum legible width, the strip becomes
   horizontally scrollable (wheel) with a chevron menu listing off-screen tabs (JetBrains
   pattern). The drag machine's centers snapshot must account for scroll offset — snapshot
   in viewport coordinates and re-snapshot on scroll during reorder.
3. **Optional group-level FLIP** on structural drops (record every leaf rect pre-mutation,
   invert + play at group granularity — same technique as the tab settle). Take it only if
   it reads well; the snap-with-overlay-preview is already acceptable.
4. **Per-neighbor-width reorder refinement** (the inherited mixed-width inaccuracy): shift
   each displaced neighbor by the *dragged* tab's width but compute insertion against each
   neighbor's own center — revisit `insertionIndexForPointer` with per-neighbor widths if
   compact strips feel off. Measure first; skip if imperceptible.
5. **Dead code sweep:** anything the migration stranded — unused shadcn `Tabs` imports in
   `Layout`, leftover label maps, `BottomTool`/`RightTool` type remnants, stale comments
   describing the old regions (e.g. the `Layout.tsx` header block and `App.tsx:4-8`).
6. **Docs page** under `docs/content/` (explanation section): the dock system — the tree
   model, drag domains, the portal host, the viewport-locked leaf, persistence; hub row in
   the matching `_index.md`. Run the humanizer pass per the docs conventions.

## Verify

- `bun test` + `bun run check`; `make prepare-for-commit` clean; full `make check` once at
  plan completion.
- Manual via `make run`:
  - Move every panel everywhere using only the context menu (no drag) — including to a new
    split; close + reopen via the Panels menu.
  - Overflow: 5+ tabs in the right strip — scroll, chevron menu, reorder across the scroll
    boundary all behave.
  - The docs site builds (`cd docs && hugo`) and the new page renders with a working hub
    link.
- Mark the plan `COMPLETED` in `README.md` when this phase lands.
