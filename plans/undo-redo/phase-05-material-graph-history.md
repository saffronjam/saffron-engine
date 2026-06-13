# Phase 5 — Per-tab snapshot history (the reusable editable-tab seam)

**Status:** NOT STARTED

**Depends on:** phase 01 (the pure history primitive + the per-tab store slice: `historyByTab` keyed by
`ViewTab.id`, `pushEdit`/`undo`/`redo`/`clearTabHistory`, the `historyReplaying` flag); phase 03 (the
global keyboard handler + Edit menu that resolve `activeViewTabId` and dispatch into that tab's
history); phase 04 (mouse Back/Forward driving the same dispatch). Independent of phase 02 (scene-edit
routing) — a snapshot tab's edits never touch the scene history.

## Goal

Two things ship together, and the order matters. First a **reusable hook, `useTabSnapshotHistory`**, for
any main tab whose canonical state is a local JS model flushed to the engine through a single apply
command. Then the **material-graph tab wired as its first consumer**, which is what proves the hook.

The hook is the seam every future *editable asset tab* plugs into — the rig editor when it gains
authoring, or any new asset editor — so giving a new editor undo/redo becomes "provide `read` / `write`
/ `equals`", never a fresh design and never an engine change. The material graph is the working
template; a second editor reuses the machinery rather than copying it.

This is the one place in the whole feature where the recorded thing is a local `{ before, after }`
snapshot rather than an inverse control command: between the editor's debounced saves the authoritative
model *is* the editor's local state (the React Flow `nodes`/`edges` for the material graph), so there is
no engine prior-value to read back. The hook captures that pattern once.

The engine stays unaware: undo/redo here replay the same apply command (`materialSetGraph` for the
graph) a normal edit makes. Nothing new is added to the control surface.

## Why a reusable hook, not material-graph-specific code

Sweeping the plans (`plans/editor-view`, `plans/material-uplift`) shows every editor that *persists*
edits shares one shape — a local model edited in a canvas/panel, flushed to the engine by one debounced
or explicit apply command:

- the **material graph** (`material-uplift` phase 20, COMPLETED): a local React Flow model → debounced
  `material-set-graph`.
- the **rig editor** (`plans/editor-view`) is a *viewer* in v1 (its `README.md`: "v1 is a
  viewer/inspector, not an authoring tool"), so it has nothing to undo yet — but its authoring
  follow-up (bone / rest-pose / keyframe edits, the plan's deferred list) will be the same shape: a
  local rig/clip model flushed by an apply command.

Building the snapshot machinery once and letting each editor supply the three model-specific functions
is the difference between "the asset editor copies 120 lines of undo wiring" and "the asset editor
passes `read`/`write`/`equals`". The material graph proves it; the asset editor reuses it. The extension
recipe is written up in phase 06.

## What exists to build on

- **`lib/` hooks precedent.** `lib/useScrubValue.ts` and `lib/useOutsideCommit.ts` are existing
  editor-local React hooks under `lib/` — the home `useTabSnapshotHistory` follows. (The pure,
  DOM-free state machine still lives in `lib/undo.ts` from phase 01; this hook is the React layer that
  feeds it `{ before, after }` snapshots.)
- **`materials/graph.ts`** — `MaterialGraph { nodes: GraphNode[]; edges: GraphEdge[] }` is the wire
  shape. `graphToFlow(graph) -> { nodes: FlowNode[]; edges: Edge[] }` builds the canvas state (canvas
  position comes out of `props.editorPos`, then stripped from `data.props`). `flowToGraph(nodes, edges)
  -> MaterialGraph` folds the canvas back to the wire shape, **rounding** each node's position into
  `props.editorPos` (`[Math.round(n.position.x), Math.round(n.position.y)]`) and dropping edges with no
  `sourceHandle`/`targetHandle`. The round-trip is lossless for graph content; the only editor-only data
  is the rounded `editorPos`.
- **`panels/MaterialGraphEditor.tsx`** — `GraphCanvas({ materialId })` holds the canvas via
  `useNodesState`/`useEdgesState` (`nodes`/`setNodes`, `edges`/`setEdges`). A load `useEffect` keyed on
  `[materialId, setNodes, setEdges]` resets `loadedRef.current = false`, fetches
  `client.materialGet(materialId)`, runs `graphToFlow` into the canvas, and renders a preview (cached in
  the module-level `previewCache`). The **debounced apply** `useEffect` keyed on
  `[nodes, edges, materialId]` is the transaction boundary: it skips the first settle after load
  (`if (!loadedRef.current) { loadedRef.current = true; return; }`), then after 500 ms computes
  `flowToGraph(nodes, edges)`, calls `client.materialSetGraph(materialId, graph)`, sets a status string,
  and re-renders the preview.
- **The per-tab history slice from phase 01** — `historyByTab: Record<string, TabHistory>` in
  `store.ts`, the `pushEdit(edit, tabId?)` / `undo(tabId?)` / `redo(tabId?)` / `clearTabHistory(id)`
  actions, and the `historyReplaying` flag that edit sites read to suppress re-recording during a
  replay. `undo`/`redo` resolve `tabId ?? activeViewTabId` and no-op on read-only tabs.
- **`openMaterialGraphTab(materialId)`** (`store.ts`) builds the tab with id
  `` `materialGraph:${materialId}` `` and reuses an existing tab for that material — so there is exactly
  one history per logical graph context, and `closeViewTab(id)` is the single place it is dropped
  (phase 01 wires `clearTabHistory` there).
- **The `rigEditor` ViewTab kind.** Phase 01 lists the kinds `scene | flamegraph | materialGraph |
  rigEditor | asset`; `plans/editor-view` opens its tab as `` `rigEditor:${meshId}` ``. Its history is
  *dormant* until that editor gains authoring — the store already no-ops `pushEdit`/`undo`/`redo` on a
  tab with no recorded edits — and this hook is exactly what it adopts when it does. No work on the rig
  editor happens in this phase; it is named only to fix the seam.
- **`client.materialGet(material)`, `client.materialSetGraph(material, graph)`,
  `client.previewRender(material, size?)`** (`control/client.ts`) — the exact three calls the debounced
  apply already uses; undo/redo reuse them through the consumer's `write`.

## Work

### 1. Build the reusable hook `lib/useTabSnapshotHistory.ts`

A generic React hook over a model type `M`. It owns the snapshot baseline and the replay guard, builds
the `{ before, after }` `UndoableEdit`, and pushes it into the active tab's history via the phase-01
store action. It knows nothing about React Flow, previews, or `client.ts` — the consumer supplies those
through `write`.

```ts
interface TabSnapshotOptions<M> {
  /// Produce the current canonical snapshot of the editor's local model.
  read(): M;
  /// Make the editor show `model` AND persist it to the engine (and any side effects, e.g. a
  /// preview re-render). This is the single function a replay calls; it is the same path a normal
  /// apply takes, so the engine sees an ordinary command.
  write(model: M): Promise<void>;
  /// Stable, normalized equality so a no-op settle records nothing (see gotchas).
  equals(a: M, b: M): boolean;
  /// Entry label for the Edit menu / affordance ("Edit material graph").
  label: string;
  /// Optional id to restore as selection context on replay (a material id, an entity id).
  selectionId?: string;
}

interface TabSnapshotHistory<M> {
  /// Seed the baseline after a load so the FIRST real edit diffs against the loaded model.
  seed(model: M): void;
  /// Call at each apply/settle boundary. Diffs `current ?? read()` against the baseline; on a real
  /// change, pushes one `{ before, after }` UndoableEdit to this tab's history and advances the
  /// baseline. No-op when equal.
  record(current?: M): void;
  /// True-and-clear: if an undo/redo replay is settling, returns true once and resets. The consumer
  /// calls this at the top of its apply effect to SKIP its own persist + record for the replay
  /// settle (the replay already persisted the authoritative model).
  consumeReplay(): boolean;
}

function useTabSnapshotHistory<M>(tabId: string, opts: TabSnapshotOptions<M>): TabSnapshotHistory<M>;
```

Internals:

- `lastAppliedRef = useRef<M | null>(null)` — the baseline (what the engine currently holds). `seed`
  sets it; `record` advances it.
- `replayingRef = useRef(false)` — bridges the ~500 ms debounce gap that outlives the store's
  `historyReplaying` (see gotchas). `consumeReplay()` reads-and-clears it.
- `record(current?)`: compute `after = current ?? opts.read()`, `before = lastAppliedRef.current`; if
  `before !== null && !opts.equals(before, after)`, build the edit and
  `useEditorStore.getState().pushEdit(edit, tabId)`. Set `lastAppliedRef.current = after` regardless
  (a no-op settle still advances the baseline to what the engine now holds).
- the built `UndoableEdit`:
  - `label: opts.label`, `selectionId: opts.selectionId`, `redoable: true`.
  - `undo: async () => { replayingRef.current = true; await opts.write(before); lastAppliedRef.current
    = before; }`.
  - `redo: async () => { replayingRef.current = true; await opts.write(after); lastAppliedRef.current
    = after; }`.

Keep the hook free of any material-graph type — `M` is generic and `opts` carries every model-specific
operation. Add a small bun test for the parts that are testable without React (factor the
`{ before, after }`-edit builder + the diff/advance logic into a pure helper the hook calls, and test
that helper: a real diff pushes once, an `equals` settle pushes nothing, the baseline advances either
way).

### 2. Wire the material graph as the first consumer

In `GraphCanvas` (`panels/MaterialGraphEditor.tsx`):

```ts
const history = useTabSnapshotHistory<MaterialGraph>(`materialGraph:${materialId}`, {
  read: () => flowToGraph(nodes, edges),
  write: async (g) => {
    const { nodes: n, edges: e } = graphToFlow(g);
    setNodes(n);
    setEdges(e);
    await client.materialSetGraph(materialId, g);
    // re-render + cache the preview exactly as the apply effect does (previewCache + preview state +
    // the "applied …" status string), so a stepped-back graph shows its own sphere, not a stale one.
  },
  equals: graphEquals, // stable normalized compare (see the "equals must be stable" gotcha)
  label: "Edit material graph",
  selectionId: materialId,
});
```

1. **Seed on load.** In the load `useEffect`, after `graphToFlow` populates the canvas, seed the
   baseline with the **normalized** graph — round-trip the loaded material through the canvas space so
   it compares in the same space the apply effect later produces: `history.seed(flowToGraph(n, e))`
   (where `n`/`e` are the `graphToFlow` outputs). Seeding from the raw `material.graph` would make the
   first real edit's `before` carry unrounded positions and falsely diff; the first post-load settle is
   already skipped via `loadedRef`, so this seed is the baseline for the *second* settle onward.
2. **Record at the debounced apply.** In the debounced apply effect, after the `loadedRef` skip and at
   the very top, short-circuit a replay settle: `if (history.consumeReplay()) return;` — this skips both
   the engine push and the recording, because the replay's `write` already pushed the authoritative
   graph. Otherwise proceed exactly as today (compute `flowToGraph`, `client.materialSetGraph`, status,
   preview), then call `history.record(after)` with the value it already computed. One entry per 500 ms
   settle.
3. The engine call's success/failure is surfaced through the existing `notifyError`/`setStatus` path
   and does not gate recording: the canvas already holds `after`, so the history must too.

### 3. The replay/debounce guard, encapsulated

`undo`/`redo` call `write`, which calls `setNodes`/`setEdges` and re-triggers the debounced apply effect
~500 ms later. Without a guard that re-application would diff against the now-restored baseline and push
a spurious entry — a runaway stack on every undo. The store's `historyReplaying` flag is cleared the
moment the async thunk resolves, while the debounced effect fires ~500 ms *later*, so a store flag alone
cannot cover the gap. The hook's `replayingRef` does, and `consumeReplay()` (step 2) is the single call
that tests-and-clears it at the apply boundary. The two flags keep distinct jobs: `historyReplaying`
(store) stops *other* edit sites and the global handler from re-entering during the await;
`replayingRef` (hook-local) bridges until the debounce settles. The consumer never touches `replayingRef`
directly — it only calls `consumeReplay()`.

### 4. Independence from the scene history

Graph entries live under `historyByTab["materialGraph:<id>"]` and never under `historyByTab["scene"]`.
The global key handler (phase 03) and the mouse handler (phase 04) both dispatch `undo`/`redo` with no
explicit `tabId`, so the store resolves `activeViewTabId` — `Ctrl+Z` while the graph tab is active steps
the graph, `Ctrl+Z` while the scene tab is active steps the scene. The two stacks are fully independent
and each survives as long as its tab is open. `closeViewTab` drops the graph history (`clearTabHistory`,
phase 01), so no entry outlives its component's unmount and the `write` closures never reference a dead
component.

### 5. Confirm the seam generalizes (no second consumer built here)

Write nothing for the rig/asset editor in this phase — it is a viewer today. Instead, verify by
inspection that a second consumer needs only the three model functions: a future asset editor with a
local model `A` and an apply command `client.saveX(A)` would call
`useTabSnapshotHistory<A>(\`rigEditor:${id}\`, { read, write, equals, label, selectionId })`, seed on
load, and `record()` at its own apply boundary — with no change to the hook, the store, the keybindings
(phase 03), or the mouse handler (phase 04), all of which already resolve `activeViewTabId` and dispatch
generically. The concrete checklist for that wiring is phase 06's extension recipe; this step only
confirms the hook exposes exactly the surface that recipe needs.

## Validation (done criteria)

- `cd editor && bun run check` + `bun run lint` clean (run inside the `saffron-build` toolbox);
  `make prepare-for-commit` clean. The `lib/useTabSnapshotHistory` pure-helper bun test passes alongside
  the phase-01 `lib/undo` tests.
- Manual via `make run`: open a material graph tab, add a node / connect an edge / change a constant,
  wait for the 500 ms debounced apply (status flips to "applied …" and the preview re-renders). Then
  `Ctrl+Z` (or mouse Back) reverts the **canvas and the preview** to the prior graph; `Ctrl+Shift+Z`
  re-applies it. Multiple edits step back one settle at a time.
- Independence: edit the graph, switch to the Scene tab, `Ctrl+Z` — it steps the scene history, not the
  graph. Switch back to the graph tab and its undo stack is intact (it was never cleared by scene undos).
- No runaway growth: a single undo restores the prior graph and pushes it to the engine once; the
  debounce-triggered re-application that follows records **no** new entry (`consumeReplay()` holds across
  the 500 ms window).
- A no-op settle (a node nudge that rounds back to the same `editorPos`, or a selection-only change)
  pushes **no** entry — the normalized `equals` rejects it.
- The hook carries no `MaterialGraph` type — confirm `M` is generic and every graph-specific operation
  lives in the `opts` the consumer passes, so a second editor is a drop-in. (The proof that it *is*
  reusable is the recipe in phase 06; here, the type signature carries the guarantee.)
- `tests/e2e` reach: limited. The suite drives the **engine** over the wire and has no React Flow
  canvas, so it cannot exercise the editor's snapshot history directly. The reachable engine-side
  assertion is only that `material-set-graph` round-trips a graph and a later `material-get` returns it —
  which already holds without this phase. Do **not** add an e2e test that pretends to cover the editor's
  undo; note the gap honestly in the phase-06 docs/e2e summary.

## Notes / gotchas

- **The 500 ms debounce is the entry boundary, not each node drag.** Every edit folding into one
  debounce window becomes one entry, mirroring the single `material-set-graph` the engine sees per
  settle. This is the snapshot tab's analogue of the coalescer's "one in-flight stream = one undo entry"
  rule, and it costs no extra engine calls — the entry rides the apply the editor already performs.
- **The `equals` must be stable.** Compare the *normalized* wire shapes: both `before` and `after` come
  through `flowToGraph` (which rounds `editorPos`), so positions match when content is unchanged; the
  comparison must be order-insensitive where order is not semantic (compare node/edge collections by id,
  or canonicalize before comparing) so a settle that merely reorders the `nodes` array does not look
  like a change. Provide it as the consumer's `graphEquals`; the hook stays type-agnostic and calls
  `opts.equals`.
- **`write` is the whole "show it and save it" path.** For the material graph it is
  `graphToFlow` → `setNodes`/`setEdges` → `materialSetGraph` → preview re-render. For a future editor it
  is whatever makes that editor display a model and persist it. The hook never assumes a canvas or a
  preview — that is what keeps it reusable.
- **Closures capture values, not live arrays.** The built edit closes over the captured `before`/`after`
  `M` snapshots (plain values taken at record time) plus `opts.write` — never a mutable `nodes` array.
  An entry created ten edits ago still restores its exact model. Per-tab keying guarantees the editor is
  mounted whenever its history is dispatched (`undo("materialGraph:<id>")` only runs when that tab is
  active, and active means `MaterialGraphEditor` is mounted via `App.tsx`'s `activeKind ===
  "materialGraph"`), so `write`'s `setNodes`/`setEdges` reference a live component.
- **A local snapshot is unavoidable and correct here.** There is no engine prior-value between debounced
  saves — the editor's local state *is* the authority for the unsaved model. This is the one place the
  recorded thing is a `{ before, after }` snapshot rather than an inverse command, and that is by design
  (the README's load-bearing decision on editable tabs). It does not grow any engine undo awareness:
  undo still replays an ordinary apply command.
- **Closing the tab loses the snapshot history** (`clearTabHistory` in `closeViewTab`, phase 01),
  consistent with a graph tab not persisting unsaved local state across a close — the engine already
  holds the last debounced model, and reopening loads it fresh.
- **No new engine command.** The hard constraint holds: a replay is the same apply command a normal edit
  uses, fed a value the editor held. If a consumer ever seems to need engine help to undo, it is
  mis-shaped — its model is not local, or its apply is not a single replayable command; rescope it or
  defer it (the README's "Explicitly OUT" section), never add engine support.
