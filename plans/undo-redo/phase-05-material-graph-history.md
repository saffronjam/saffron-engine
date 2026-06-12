# Phase 5 — Per-tab material-graph history

**Status:** NOT STARTED

**Depends on:** phase 01 (the pure history primitive + the per-tab store slice:
`historyByTab` keyed by `ViewTab.id`, `pushEdit`/`undo`/`redo`/`clearTabHistory`, the
`historyReplaying` flag); phase 03 (the global keyboard handler + Edit menu that resolve
`activeViewTabId` and dispatch into that tab's history); phase 04 (mouse Back/Forward driving the
same dispatch). Independent of phase 02 (scene-edit routing) — this tab's edits never touch the
scene history.

## Goal

Give the material-graph tab its own undo/redo history, keyed by its `ViewTab.id`
(`materialGraph:<materialId>`). This is the second concrete per-main-tab history and the one place
where the recorded thing is a local JS snapshot rather than an inverse control command — between the
editor's debounced saves the authoritative graph *is* the React Flow `nodes`/`edges` state, so there
is no engine prior-value to read back.

Each debounced apply records a `{before, after}` pair of `MaterialGraph` values; undo loads `before`
into the canvas and pushes it to the engine (`materialSetGraph` + a preview re-render), redo does the
same with `after`. After this phase, `Ctrl+Z` / mouse Back inside a graph tab steps the graph back one
debounced settle at a time, independently of the scene tab — switching back to the scene tab finds its
own stack intact.

The engine stays unaware: undo/redo here replay the same `materialSetGraph` / `previewRender` calls a
normal edit makes. Nothing new is added to the control surface.

## What exists to build on

- `materials/graph.ts`: `MaterialGraph { nodes: GraphNode[]; edges: GraphEdge[] }` is the wire shape.
  `graphToFlow(graph) -> { nodes: FlowNode[]; edges: Edge[] }` builds the canvas state (canvas position
  comes out of `props.editorPos`, then stripped from `data.props`). `flowToGraph(nodes, edges) -> MaterialGraph`
  folds the canvas back to the wire shape, **rounding** each node's position into `props.editorPos`
  (`[Math.round(n.position.x), Math.round(n.position.y)]`) and dropping edges with no
  `sourceHandle`/`targetHandle`. The round-trip is lossless for graph content; the only editor-only data
  is the rounded `editorPos`.
- `panels/MaterialGraphEditor.tsx`: `GraphCanvas({ materialId })` holds the canvas via
  `useNodesState`/`useEdgesState` (`nodes`/`setNodes`, `edges`/`setEdges`). A load `useEffect` keyed on
  `[materialId, setNodes, setEdges]` resets `loadedRef.current = false`, fetches
  `client.materialGet(materialId)`, runs `graphToFlow` into the canvas, and renders a preview (cached in
  the module-level `previewCache`). The **debounced apply** `useEffect` keyed on `[nodes, edges, materialId]`
  is the transaction boundary: it skips the first settle after load
  (`if (!loadedRef.current) { loadedRef.current = true; return; }`), then after 500 ms computes
  `flowToGraph(nodes, edges)`, calls `client.materialSetGraph(materialId, graph)`, sets a status string,
  and re-renders the preview.
- The per-tab history slice from phase 01: `historyByTab: Record<string, TabHistory>` in `store.ts`, the
  `pushEdit(tabId, edit)` / `undo(tabId?)` / `redo(tabId?)` / `clearTabHistory(id)` actions, and the
  `historyReplaying` flag that edit sites read to suppress re-recording during a replay. `undo`/`redo`
  resolve `tabId ?? activeViewTabId` and no-op on read-only tabs.
- `openMaterialGraphTab(materialId)` (`store.ts`) builds the tab with id `` `materialGraph:${materialId}` ``
  and reuses an existing tab for that material — so there is exactly one history per logical graph context,
  and `closeViewTab(id)` is the single place it is dropped (phase 01 wires `clearTabHistory` there).
- `MaterialGraphEditor` is mounted in the `materialGraph` workspace body in `App.tsx`
  (`activeKind === "materialGraph"` → `MaterialGraphWorkspace` → `MaterialGraphEditor`), so the canvas
  component is live whenever the tab is active — which is precisely when its history can be triggered.
- `client.materialGet(material)`, `client.materialSetGraph(material, graph)`, `client.previewRender(material, size?)`
  (`control/client.ts`) — the exact three calls the debounced apply already uses; undo/redo reuse them.

## Work

### 1. Seed a `lastAppliedRef` from the loaded graph

In `GraphCanvas`, add `lastAppliedRef = useRef<MaterialGraph | null>(null)`. In the load `useEffect`,
after `graphToFlow` populates the canvas, seed it with the **normalized** graph — round-trip the loaded
material through the canvas space so the stored value compares in the same space the apply effect later
produces: `lastAppliedRef.current = flowToGraph(n, e)` (where `n`/`e` are the `graphToFlow` outputs).
This matters because `flowToGraph` rounds `editorPos`; seeding from the raw `material.graph` would make
the first real edit's `before` carry unrounded positions and falsely diff against `after`. The first
settle after load is already skipped via `loadedRef`, so this seed is only the baseline for the *second*
settle onward.

### 2. Push a `{before, after}` entry at each debounced apply

In the debounced apply effect, after the `loadedRef` skip and before `client.materialSetGraph`:

1. Compute `after = flowToGraph(nodes, edges)` (the effect already needs this value — reuse it).
2. Read `before = lastAppliedRef.current`.
3. If `before` is non-null and `before` differs from `after` (stable normalized deep compare, see the
   gotchas), build an `UndoableEdit` and `pushEdit("materialGraph:" + materialId, edit)`:
   - `label: "Edit material graph"` (a generic label; node-op granularity is later polish).
   - `undo`: set the canvas to `before` via `graphToFlow(before)` → `setNodes`/`setEdges`, then push it
     to the engine (`await client.materialSetGraph(materialId, before)`) and re-render
     (`client.previewRender(materialId, 256)`, updating `previewCache` + the `preview` state and the
     status string the same way the apply effect does).
   - `redo`: the same with `after`.
4. Set `lastAppliedRef.current = after` regardless of whether an entry was pushed — a no-op settle still
   advances the baseline to the value the engine now holds.

The engine call's success/failure is surfaced through the existing `notifyError`/`setStatus` path and
does not gate recording: the canvas already holds `after`, so the history must too.

### 3. Guard the replay loop

`undo`/`redo` here call `setNodes`/`setEdges`, which re-triggers the debounced apply effect ~500 ms
later. Without a guard that re-application would diff `before` against the now-restored `lastAppliedRef`
and push a spurious entry — a runaway stack on every undo.

The store's `historyReplaying` flag (set by phase 01 around `await edit.undo()` / `edit.redo()`) is
cleared the moment the async thunk resolves, while the debounced effect fires ~500 ms *later* — so the
editor needs its own short-lived guard that outlives the debounce:

- Add `replayingGraphRef = useRef(false)` in `GraphCanvas`.
- In both `undo` and `redo` thunks, set `replayingGraphRef.current = true` *before* `setNodes`/`setEdges`,
  and after the engine push completes set `lastAppliedRef.current` to the replayed graph (`before` for
  undo, `after` for redo) so the next user edit diffs against what the engine now holds.
- In the debounced apply effect, when `replayingGraphRef.current` is true: skip the engine push and the
  recording for that settle (the thunk already pushed the authoritative graph to the engine), clear
  `replayingGraphRef.current = false`, and return. The replay's own `materialSetGraph` is authoritative,
  so the debounce-triggered re-send is redundant.

The two flags have distinct jobs: `historyReplaying` (store) stops *other* edit sites and the global
handler from re-entering during the await; `replayingGraphRef` (local) bridges the gap until the
debounce settles, since the canvas mutation outlives the await.

### 4. Drive undo/redo into the canvas via captured closures

The `undo`/`redo` thunks close over `setNodes`/`setEdges` and the captured `before`/`after`
`MaterialGraph` values, captured at entry-creation time. This is safe because:

- Per-tab keying guarantees the editor is mounted whenever its history is dispatched:
  `undo("materialGraph:<id>")` only runs when that tab is active (`activeViewTabId` resolves to it), and
  the tab being active means `MaterialGraphEditor` is mounted (`App.tsx` `activeKind === "materialGraph"`),
  so `setNodes`/`setEdges` reference a live component.
- `closeViewTab` drops the history (`clearTabHistory`, phase 01), so no entry outlives its component's
  unmount — there is no stale-closure window where a thunk could call a `setNodes` whose component is gone.

The captured values are plain `MaterialGraph` objects (snapshots taken with `flowToGraph` at record time),
never a live `nodes` array reference, so an entry stays correct after many intervening edits.

Fallback (only if the closure proves fragile under test — e.g. if the `useNodesState`/`useEdgesState`
setters turn out not to be stable across a tab remount): register a per-material canvas-control object
(`{ setGraph(graph: MaterialGraph): void }`) into a module-level `Map<materialId, …>` on mount and clear
it on unmount, and have the thunks look it up by `materialId` at replay time instead of closing over the
setters. Prefer the closure; reach for the registry only if needed.

### 5. Confirm independence from the scene history

Graph entries live under `historyByTab["materialGraph:<id>"]` and never under `historyByTab["scene"]`.
The global key handler (phase 03) and the mouse handler (phase 04) both dispatch `undo`/`redo` with no
explicit `tabId`, so the store resolves `activeViewTabId` — a `Ctrl+Z` while the graph tab is active
steps the graph, a `Ctrl+Z` while the scene tab is active steps the scene. The two stacks are fully
independent and each survives as long as its tab is open.

## Validation (done criteria)

- `cd editor && bun run check` + `bun run lint` clean (run inside the `saffron-build` toolbox);
  `make prepare-for-commit` clean.
- Manual via `make run`: open a material graph tab, add a node / connect an edge / change a constant,
  wait for the 500 ms debounced apply (status flips to "applied …" and the preview re-renders). Then
  `Ctrl+Z` (or mouse Back) reverts the **canvas and the preview** to the prior graph; `Ctrl+Shift+Z`
  re-applies it. Multiple edits step back one settle at a time.
- Independence: edit the graph, switch to the Scene tab, `Ctrl+Z` — it steps the scene history, not the
  graph. Switch back to the graph tab and its undo stack is intact (it was never cleared by scene undos).
- No runaway growth: a single undo restores the prior graph and pushes it to the engine once; the
  debounce-triggered re-application that follows records **no** new entry (the `replayingGraphRef` guard
  holds across the 500 ms window).
- A no-op settle (e.g. a node nudge that rounds back to the same `editorPos`, or a selection-only change)
  pushes **no** entry — the normalized deep-compare rejects it.
- `tests/e2e` reach: limited. The e2e suite drives the **engine** over the wire and has no React Flow
  canvas, so it cannot exercise the editor's snapshot history directly. The reachable engine-side
  assertion is only that `material-set-graph` round-trips a graph and a later `material-get` returns it —
  which already holds without this phase. Do **not** add an e2e test that pretends to cover the editor's
  undo; note the gap honestly in the phase-06 docs/e2e summary.

## Notes / gotchas

- **The 500 ms debounce is the entry boundary, not each node drag.** Every edit folding into one
  debounce window becomes one entry, mirroring the single `material-set-graph` the engine sees per
  settle. This is the graph tab's analogue of the coalescer's "one in-flight stream = one undo entry"
  rule, and it costs no extra engine calls — the entry rides the apply the editor already performs.
- **The deep-compare must be stable.** Compare the *normalized* wire shapes: both `before` and `after`
  come through `flowToGraph` (which already rounds `editorPos`), so positions match when content is
  unchanged; the comparison must be order-insensitive where order is not semantic (compare node/edge
  collections by id, or canonicalize before comparing) so a settle that merely reorders the `nodes` array
  does not look like a change. The first post-load settle never reaches the compare — it is skipped via
  `loadedRef` — and `lastAppliedRef` is seeded from the normalized load (§1), so the baseline is in the
  same space.
- **Closures capture values, not live arrays.** The thunks close over `setNodes`/`setEdges` plus the
  captured `before`/`after` `MaterialGraph` snapshots — never the mutable `nodes` array. An entry created
  ten edits ago still restores its exact graph.
- **A local snapshot is unavoidable and correct here.** There is no engine prior-value between debounced
  saves — the editor's React Flow state *is* the authority for the unsaved graph. This is the one place in
  the whole feature where the recorded thing is a `{before, after}` snapshot rather than an inverse
  control command, and that is by design (see the README's load-bearing decision on the material graph).
  It does not mean the engine grows any undo awareness: undo still replays an ordinary `materialSetGraph`.
- **Closing the tab loses the graph history** (`clearTabHistory` in `closeViewTab`, phase 01). This is
  consistent with the existing behavior that a graph tab does not persist unsaved local state across a
  close — the engine already holds the last debounced graph, and reopening the tab loads it fresh.
- **Keep the preview cache and status in sync on replay.** The undo/redo thunks must update `previewCache`
  and the `preview` state the same way the apply effect does, so a stepped-back graph shows its own
  rendered sphere, not a stale one. Set the status string too (reuse the "applied …" path) so the user
  sees the replay landed.
