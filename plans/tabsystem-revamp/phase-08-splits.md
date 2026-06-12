# Phase 08 — split drops

**Status:** NOT STARTED

## Goal

Enable the `{ kind: "split" }` half of `DropTarget`: dropping a torn tab on a leaf's edge
band splits that leaf and docks the panel beside it. **Requirement 2's strong reading —
the Material panel literally side-by-side with the Timeline — lands here.**

## What exists to build on

- `splitLeaf` already exists and is unit-tested in the pure model (phase 03); `normalize`
  already merges same-orientation nesting and collapses single-child branches.
- The drag layer (phase 06) already computes zones per hovered leaf and renders the single
  transitioned overlay; `acceptsSplits` is already in the registry snapshot.
- `DockRoot` (phase 07) renders arbitrary trees, so a freshly split tree renders without
  new code; the settle subscriber covers the mutation.
- Zone-math conventions (README research, VS Code `editorDropTarget.ts`): edge bands at
  1/3 of the leaf extent pick the split direction; the overlay previews the *resulting*
  region by filling 50% of the leaf on that side; center remains merge (100%).

## Work

1. **Zone math:** extend the per-leaf hit-testing — outer-third edge bands on leaves with
   `acceptsSplits` produce `{ kind: "split", leafId, edge }`; the overlay fills the
   corresponding 50%. Merge (center) and strip-insert behavior unchanged.
2. **Commit:** the drop calls `movePanel(id, { kind: "split", … })` → `splitLeaf`. The new
   sibling starts at 50% of the split leaf (VS Code behavior); cross-leaf structural drops
   **snap** — the transitioned overlay already previewed the destination; group-level FLIP
   stays a phase-09 polish option.
3. **Viewport rules:** the locked viewport leaf accepts no merges (already) but its
   *siblings* may split; splitting against the viewport leaf's own edges inserts a sibling
   beside it in the parent branch — never over it. Respect px minimums: a split that would
   violate the viewport's 520 px min (or a sidebar min) is an invalid target — no zone
   renders (Unreal convention: illegal zones never appear).
4. **Empty-band splits:** the phase-06 window-edge reveal bands keep meaning "dock into
   that region as a tab" — recreating the well-known leaf if it was collapsed (the
   phase-07 recreate-on-drop semantic); no split semantics there.

## Verify

- `bun test` (split + normalize round-trips: split, move the tab back out, tree collapses
  to the original shape) + `bun run check`; `make prepare-for-commit` clean.
- Manual via `make run`:
  - **Material dropped on the Timeline leaf's right edge: a horizontal split inside the
    bottom region, both visible side-by-side; state intact; ratios drag and persist;
    survives reload.**
  - Split each edge of a normal leaf; verify the 50% overlay preview matches the result.
  - Splits adjacent to the viewport: viewport re-glues, min widths hold, no occlusion.
  - Tear the split-off panel back out: the split collapses (`normalize`), neighbor
    reclaims space, viewport re-glues.
  - No zone renders where a split would violate minimums (shrink the window to force it).
