# Phase 6 — the skeleton tree panel

**Status:** NOT STARTED
**Depends on:** 1 (`get-rig`), 3 (the highlight channel), 5 (the workspace shell)

## Goal

Fill the workspace's left panel: the rig's **bone hierarchy** as a tree — the view that answers the
original complaint that bones clutter the scene outliner with no home of their own. v1 is read-only
navigation: expand/collapse, select a bone, and the selection reflects into the live preview (the
skeleton overlay's selected highlight + the Inspector-style details slot in phase 7). UE5's Skeleton
Tree is the reference; per-bone retarget/socket columns are not copied.

## What exists to build on

- `get-rig`, sourced from the `.smodel` model container, returns the flat parent-indexed bone list
  (`RigBoneDto { index, name, parent, joint }`) — exactly a tree's adjacency form, including
  non-joint intermediate nodes (render joints emphasized, intermediates muted).
- There is no tree primitive in `components/ui/` — the hand-built precedents are
  `panels/HierarchyTree.tsx` (scene outliner: rows, indent, expand state, selection styling,
  keyboard) and `panels/AssetFolderTree.tsx`. Follow `HierarchyTree`'s row/indent/selection idiom
  so the two trees read as siblings.
- Bone → preview entity table: `enter-rig-preview`'s result (phase 2) maps each `get-rig` bone
  index to its spawned preview-scene entity uuid (`RigBoneEntityDto`) — the table this panel
  highlights through. (The engine-side table lives in phase 2 with the command + its fixture; this
  panel just consumes it.)
- **The highlight channel (phase 3), not scene selection.** `buildSkeletonOverlay` (rewritten in
  phase 3) keys off the *previewed rig*, not `editor.selected`, and carries a highlighted-joint
  index set by `set-skeleton-highlight { joint }`. The tree drives that, **not** the `select`
  command — selecting a bone entity would null the selection-keyed `animationState` slice (a bone
  has no `AnimationPlayerComponent`) and break the rig timeline (phase 9), and the selection-keyed
  overlay draws only when the selected entity carries `SkinnedMeshComponent` (a bone does not). Highlighting via the dedicated channel
  keeps the engine selection on the rig (timeline alive, overlay drawn) and tints the chosen joint.
- UI conventions: semantic tokens, `ScrollArea` for the list, `humanizeFieldName` does **not**
  apply to bone names (they are data, not field keys — render verbatim).

## Work

### 1. The tree component

`RigSkeletonTree { bones, selectedIndex, onSelect }` in `editor/src/panels/` (or `components/`):
build children-lists from the parent indices once (memo), render expand/collapse rows with the
HierarchyTree idiom — thin rows, indent per depth, joint rows with a bone glyph, intermediate
nodes muted (`text-muted-foreground`). Default expanded (rigs are small; persist collapse state
locally in the component). Filter out the phase-3 furnishing by construction (the tree renders
`get-rig` data, not scene entities — the floor never appears).

### 2. Highlight wiring

Bone row click → `set-skeleton-highlight { joint: boneIndex }` (phase 3) through a coalescer (rapid
arrow-key navigation must not flood the wire); the previewed rig's overlay tints that joint in the
live frame. The tree's own row highlight is **local component state** (the selected row is a view
concern, not engine selection) — no `get-selection` round-trip, no scene-selection mutation. This
is the deliberate split that keeps the rig timeline (phase 9) fed: the engine selection stays on
the rig the whole time.

### 3. Toolbar hooks

The workspace toolbar (phase 5) gains the bone-related toggles next to where they conceptually
live: show-bones / show-axes / joint-size feeding `set-skeleton-overlay` (already wired engine-side).

## Validation (done criteria)

- `bun run check` + `bun run lint` clean.
- Manual (`make run`): open the SimpleSkin rig — the tree shows the node hierarchy with the two
  joints emphasized; clicking a joint **tints it in the preview while the overlay stays drawn and
  the timeline stays live** (the highlight-channel proof); collapse/expand works; the floor entity
  does not appear.
- `make e2e`: `set-skeleton-highlight` on a joint index → the highlighted-joint screenshot differs
  while `get-animation-state` on the rig still resolves (selection unchanged) — the engine pieces
  this panel rides land in phases 2 (bone table) and 3 (highlight channel), so this phase adds no
  new commands and its gates are `bun run check`/`lint` + the manual matrix.
- `docs/`: the rig-editor page gains the skeleton-tree section.

## Notes / gotchas

- **Render bone names verbatim** — they are the durable clip-binding keys; cosmetic renaming in the
  view would lie about the data.
- Rigs can nest deeper than they are wide — indent × depth must not force horizontal scroll for
  typical humanoid depth (~6–8); clamp indent growth like HierarchyTree does.
- **The tree never touches scene selection** — that is the whole point of the highlight channel.
  The engine selection stays on the rig mesh entity for the tab's lifetime, so the selection-keyed
  `animationState` slice the timeline reads never goes null (the bug phase 9 would otherwise hit).
- Multi-select, search/filter, and drag-reparent are explicitly out — this tree is a viewer.
