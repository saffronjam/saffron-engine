# Phase 1 — History core + store slice

**Status:** NOT STARTED

**Depends on:** nothing — this is the foundation phase. Later phases (route scene edits,
keybindings/menu, mouse back/forward, material-graph history, edge cases) all consume the primitive
and store actions built here.

## Goal

Build the engine of the feature in isolation and prove it, before a single edit site is touched. Two
pieces ship together:

1. A pure `lib/undo.ts` module — the `UndoableEdit` record and a `TabHistory` `{ past, future }` value
   with DOM-free, async-free functions that push, undo, redo, and read the stacks — covered by
   `lib/undo.test.ts` on `bun test`.
2. The editor-store slice that holds one `TabHistory` per `ViewTab.id` (`historyByTab`), a
   `historyReplaying` flag, and the `pushEdit` / `undo` / `redo` / `clearTabHistory` /
   `clearSceneHistory` actions plus the `beginEdit` gesture-transaction helper. The store owns the
   `await edit.undo()`; the pure module never touches a promise.

After this phase there is no keybinding, no menu, and no edit site recording history. You prove the
machinery with the unit tests and by driving the store actions directly (a throwaway dev call). The
design intent it locks in: the engine stays completely unaware of undo — an undo is the editor
replaying the same `client.ts` control command a normal edit uses, fed a value the editor already held.

## What exists to build on

- **`state/store.ts` — the `ViewTab` union and the tab slice.** `ViewTab` is a discriminated union on
  `kind` (`scene` | `flamegraph` | `materialGraph` | `rigEditor` | `asset`); the store holds
  `viewTabs: ViewTab[]` and `activeViewTabId: string`. The scene tab
  `{ id: "scene", kind: "scene", title: "Scene", closable: false }` is seeded in the initial state and
  is always present. Material-graph tabs are keyed `id: "materialGraph:<materialId>"` by
  `openMaterialGraphTab(materialId)`. The id is the stable key the history dictionary keys on — not
  `kind`, not a panel/component lifetime.
- **`state/store.ts` — the two lifecycle points a history must follow.** `closeViewTab(id)` drops a tab
  (it early-returns on `"scene"`, which is non-closable) — that is where a tab's history is freed.
  `resetSceneState()` is the hard reset on a project/scene load: it clears `entities`, selection,
  `componentsBySelected`, assets, environment, and rebuilds `viewTabs` to exactly the scene tab — that
  is where the scene history clears.
- **`control/coalesce.ts` — `makeCoalescer`.** The single-in-flight write model the gesture-transaction
  API mirrors: a scrub/drag streams many `push(value)` calls, the coalescer keeps at most one send in
  flight and collapses the burst into one logical write. Undo's gesture grouping uses the same
  boundary — capture the prior value at gesture begin, push exactly one `UndoableEdit` at gesture end —
  so "one in-flight coalesced stream" and "one undo entry" coincide by construction. This phase ships
  only the `beginEdit`/`commit` helper that yields that single push; the edit sites wire it in phase 02.
- **`lib/keybindings.ts` — the pure-`lib/` module pattern.** A self-contained module of plain types and
  pure functions (`normalizePressEvent`, `bindingFor`, `matchesBinding`, `findConflict`) with no React
  and no store import, exercised by logic alone. `lib/undo.ts` follows the same shape: types plus pure
  functions, the store layered on top. (No `*.test.ts` exists in `editor/src` yet, and `package.json`
  has no `test` script — this phase adds both.)
- **`state/store.ts` — store-action conventions to match.** Actions return a fresh slice object only on
  a real change and `return {}` to no-op (e.g. `setExpanded`, `setGizmo`, `setActiveViewTab`,
  `removeFromAssetSelection`), so subscribers re-render only on a genuine delta. `setDragActive` is the
  poll gate raised around an optimistic gesture; `historyReplaying` is a *separate* flag (see gotchas).
- **`control/client.ts` — the inverse-command surface (named here so the record shape is right).** The
  mutations a recorded edit's `undo`/`redo` thunks replay are already typed: `setTransform`,
  `setMaterial`, `assignAsset`, `setComponent`, `setComponentField`, `addComponent`, `removeComponent`,
  `setScriptOverride`, `renameEntity`, `setParent`, `setEnvironment`, `setExposure`, and
  `materialSetGraph`. This phase does not call any of them; it only fixes the record shape so a thunk
  closing over one of these is the natural fit (phases 02 and 05 wire them).
- **`lib/flash.ts` — `notifyError` / `errorText`.** The toast path a rejected replay surfaces through
  (the AGENTS toast rule), used by the `undo`/`redo` store actions' catch.

## Work

### 1. Write `lib/undo.ts` — the pure history primitive

DOM-free and async-free. Export:

- `interface UndoableEdit` with:
  - `label: string` — a short human phrase for the Edit menu and the undo/redo affordance ("Move
    entity", "Set albedo").
  - `undo: () => Promise<void>` — replays the inverse (or, for the material graph in phase 05, loads
    the prior snapshot). The store owns the `await`.
  - `redo: () => Promise<void>` — re-applies the edit. An undo-only edit (entity creation, deferred per
    the scope) still defines this but flags the record not redoable.
  - `selectionId?: string` — the entity (or material) the edit acted on, so undo/redo can restore the
    selection context as UX. An opaque string id (see gotchas); never numeric.
  - `redoable?: boolean` — defaults to redoable; `false` means the entry can be undone but its forward
    redo is dropped (the entity-create case, phase 06). `takeRedo` must respect it.
- `interface TabHistory { past: UndoableEdit[]; future: UndoableEdit[] }` — `past` newest-last (its
  tail is the next undo), `future` the redo branch (its head is the next redo).
- `const HISTORY_CAP = 200` (approximate; see gotchas) and pure functions:
  - `emptyHistory(): TabHistory` — `{ past: [], future: [] }`.
  - `canUndo(h)` / `canRedo(h)` — non-empty `past` / non-empty `future` (and, for `canRedo`, the next
    future entry not flagged non-redoable).
  - `undoLabel(h)` / `redoLabel(h)` — the label of the entry the next undo/redo would act on, or
    `null` when none.
  - `pushEdit(h, edit): TabHistory` — append `edit` to `past`, **clear `future`** (a fresh edit
    abandons the redo branch — the standard linear model), and drop the oldest `past` entry when over
    `HISTORY_CAP`.
  - `takeUndo(h): { edit: UndoableEdit; next: TabHistory } | null` — pop the tail of `past`, return it
    plus the history with that entry moved to the front of `future`; `null` when `past` is empty.
  - `takeRedo(h): { edit: UndoableEdit; next: TabHistory } | null` — pop the front of `future`, return
    it plus the history with that entry pushed back onto `past`; `null` when `future` is empty or the
    next entry is `redoable: false`.

Keep every function a pure value-to-value transform: no `await`, no `console`, no `window`, no store
import. The async replay (awaiting `edit.undo()`) and the per-tab dictionary live in the store. This is
what keeps the unit tests synchronous and the store the single engine-touching place.

### 2. Write `lib/undo.test.ts` — bun unit tests + wire up `bun test`

Cover the state-machine invariants with trivial in-memory edits (an `UndoableEdit` whose `undo`/`redo`
are no-op async thunks — the pure functions never call them):

- `pushEdit` appends to `past` and **clears `future`** (push-after-undo abandons the redo branch).
- `takeUndo` / `takeRedo` move the right edit between stacks and return that exact edit; `null` on an
  empty stack.
- `canUndo` / `canRedo` / `undoLabel` / `redoLabel` track the stacks.
- The cap drops the oldest `past` entry once `HISTORY_CAP` is exceeded, keeping the newest.
- `takeRedo` returns `null` (and `canRedo` is `false`) for a `redoable: false` next entry.

Then make the suite runnable and gated:

- Add `"test": "bun test"` to `editor/package.json` scripts.
- The reproducible gate's frontend step is `( cd "$REPO/editor" && bun run build )` (`tools/ci/check.sh`)
  and the editor workflow check is `bun run check`; neither runs `bun test` today. Add the unit run to
  the frontend lane so this primitive is exercised in CI — extend the editor step in `tools/ci/check.sh`
  to run `bun test` alongside `bun run build`. Keep it editor-scoped (the engine build/smoke lanes are
  unaffected).

### 3. Add the store slice

In `EditorState` (`state/store.ts`):

- State: `historyByTab: Record<string, TabHistory>` (default `{}`; histories created lazily on first
  push) and `historyReplaying: boolean` (default `false`).
- Actions:
  - `pushEdit(edit: UndoableEdit, tabId?: string): void` — resolve `tabId ?? activeViewTabId`, no-op on
    a read-only tab (a `flamegraph` or `asset` tab never gets a history), else create-or-update that
    tab's `TabHistory` via the pure `pushEdit`. Return a **fresh `historyByTab`** only on a real change.
  - `undo(tabId?: string): Promise<void>` / `redo(tabId?: string): Promise<void>` — see step 4.
  - `clearTabHistory(tabId: string): void` — drop that key from `historyByTab` (identity-stable when
    absent).
  - `clearSceneHistory(): void` — drop `historyByTab["scene"]` *and* any orphaned non-scene histories
    (see step 5).

A "read-only tab" is one whose `kind` is `flamegraph` or `asset` — those mirror engine/inspect state
and host no authored edits, so `pushEdit`/`undo`/`redo` no-op there. `scene`, `materialGraph`, and
`rigEditor` may host history (only `scene` and `materialGraph` are wired by later phases; `rigEditor`
stays a no-op until it has an in-scope edit).

### 4. Implement `undo` / `redo`

Both resolve `tabId ?? activeViewTabId`, read that tab's `TabHistory`, and run `takeUndo` / `takeRedo`:

1. If the take returns `null` (empty stack, or a non-redoable next entry for redo), no-op.
2. Set `historyReplaying = true`.
3. `await edit.undo()` (or `edit.redo()`); on rejection, surface it with `notifyError(errorText(err))`
   and `console.error` — but **still move the entry** so the stacks stay consistent with what the user
   sees (a half-applied replay is repaired by the next reconcile poll anyway). Do the move regardless of
   success or failure.
4. Restore `edit.selectionId` into the selection (so undo lands the user back on the entity it touched),
   when present.
5. Commit the `next` history from the take into `historyByTab`.
6. Clear `historyReplaying` in a `finally`.

The store owns the entire async dance; `lib/undo.ts` only computed the `{ edit, next }` value.

### 5. Wire the lifecycle clears

- `closeViewTab(id)` → also clear that tab's history (`clearTabHistory(id)` — fold the history drop into
  the same `set`, or call the action). The existing `"scene"` early-return already guards the
  non-closable tab.
- `resetSceneState()` → call `clearSceneHistory()`: a project/scene load makes every captured prior
  value stale, so the scene history must clear. Because `resetSceneState` also rebuilds `viewTabs` to
  just the scene tab, every non-scene history (`materialGraph:*`, …) is now orphaned — drop them in the
  same pass so `historyByTab` never leaks a key with no live tab.

### 6. Ship `beginEdit` — the gesture-transaction helper (no caller yet)

Add `beginEdit<T>({ prior, selectionId }: { prior: T; selectionId?: string }): { commit(final: T,
build: (prior: T, final: T) => UndoableEdit): void }`. `beginEdit` captures the value held at gesture
start; `commit` is called once at gesture end with the final value and a builder that produces the
`UndoableEdit` (closing over `prior` and `final`), then calls `pushEdit` exactly once. This is the
bracket a gizmo drag (`begin` → `end`) or an Inspector field scrub (`onDragStart` → `onFieldDragEnd`)
wraps so a 60-tick drag yields one entry, mirroring the coalescer collapsing the same stream into one
in-flight send. Add a unit test that one `beginEdit`/`commit` cycle yields exactly one `pushEdit`. No
edit site calls it in this phase — phase 02 wires it in.

## Validation (done criteria)

- `bun test` passes on `lib/undo.test.ts` (all invariants in step 2 plus the `beginEdit` single-push
  test), and the frontend gate runs it (the `tools/ci/check.sh` editor step).
- `cd editor && bun run check` and `bun run lint` are clean; `make prepare-for-commit` (format + lint)
  is clean. Run inside the `saffron-build` toolbox.
- Manual smoke (a throwaway dev/console call, removed before commit):
  `useEditorStore.getState().pushEdit({ label: "smoke", undo: async () => {…}, redo: async () => {…} })`,
  then `undo()` runs the undo thunk and `redo()` re-runs the redo thunk; `canUndo`/`undoLabel` derived
  from `historyByTab["scene"]` track the stacks; `closeViewTab` on a (temporarily opened) material tab
  clears its key from `historyByTab`; calling `resetSceneState()` clears the scene history.
- No edit site references `pushEdit` yet — that is phase 02. This phase is "engine in isolation, proven."
- `tests/e2e` drives the **engine** over the wire; an editor-only history primitive has no engine
  surface, so there is nothing to assert in e2e here. (Phase 06 adds the one reachable e2e assertion —
  that a replayed inverse command lands on the engine — once edits route through history.)

## Notes / gotchas

- **Keep `lib/undo.ts` pure and async-free.** The store action owns `await edit.undo()`; the module
  only manipulates `TabHistory` values. This keeps the unit tests synchronous and keeps the store the
  single place that touches the engine, the poll, and toasts.
- **Identity discipline.** `pushEdit`/`undo`/`redo`/`clearTabHistory`/`clearSceneHistory` return a fresh
  `historyByTab` (and a new `TabHistory` for the touched tab) only on a real change, and `return {}`
  when nothing changed — matching `setExpanded`/`setGizmo`. The Edit-menu selector (phase 03) then
  re-renders only on a real stack change, not on every unrelated `set`.
- **Entity ids are opaque strings** (`editor/AGENTS.md`). A captured `prior`/`final` payload and
  `selectionId` keep ids as strings; **never `Number()`** them inside a thunk — that silently corrupts
  large u64 ids.
- **Bounded, in-memory only.** `HISTORY_CAP ≈ 200` per tab, never persisted across reloads. A
  material-graph entry carries two `MaterialGraph` objects (phase 05) — still small JSON, well within
  the cap.
- **`historyReplaying` is distinct from `dragActive`.** A replay is atomic — it commits one inverse
  command and is repaired by the next reconcile poll — so it must **not** raise `dragActive` (which
  exists to protect a *continuous* optimistic gesture from the poll). `historyReplaying` guards
  re-entrancy and, in later phases, stops an edit site from recording the replayed command as a brand
  new history entry. Do not conflate the two flags.
- **The replay re-syncs through the existing poll, with zero engine awareness.** A `client.ts` inverse
  command bumps the engine's `sceneVersion`/`selectionVersion`; the focus-gated reconcile poll
  (`startReconcile` in `store.ts`) sees the version change on its next cheap tick and re-fetches, so the
  UI converges with no special undo path. This is why the record holds inverse *commands*, not whole-
  scene snapshots — the editor already held the prior value at the edit site, and the engine owns the
  truth.
- **No new engine command.** The hard constraint: if any step here seemed to need engine-side history or
  a snapshot command, the step is wrong — rescope it to a client-held value replayed through the command
  the edit already used, or move it to the deferred list (`README.md`'s "Explicitly OUT" section). The
  engine never learns that undo exists.
