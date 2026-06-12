# Undo/redo — per-main-tab edit history

**Status:** NOT STARTED

Undo/redo is implemented **entirely in the Tauri/React editor**. The engine stays unaware of it: no
new engine command exists to support undo, no engine-side history, no engine-side snapshot. Undo is
the editor **replaying inverse control commands it already has** — for each edit the editor pushes
"how to undo it" onto a client-side history, and undo dispatches that recorded inverse against the
existing control surface in `client.ts`. The history is **per main tab** (one `TabHistory` per
`ViewTab.id`): the scene tab tracks scene edits, each material-graph tab tracks its own graph, and the
two never bleed into each other.

The same plan delivers the second todo item: **mouse Back (button 3) = undo and mouse Forward
(button 4) = redo**, routed to the active tab's history, while **killing the webview's default
Back/Forward navigation** (`history.back()` / `history.forward()` and Alt+Left/Right) which would
otherwise reload the single-page app and lose all editor state.

If an operation cannot be inverted with the existing control surface, it is scoped OUT and listed
honestly in **Explicitly OUT (deferred)** — the plan never proposes adding engine support to make
something undoable.

## The load-bearing decisions

These were settled by enumerating the full mutation surface (`client.ts`), the per-tab `ViewTab` model
(`store.ts`), and the optimistic/coalescer/poll machinery, then judging each operation's invertibility
against the existing control surface.

1. **Undo = inverse-command replay against the existing control surface — never an engine feature.**
   An `UndoableEdit` records the call(s) that revert an edit: the inverse `client.*` call with the
   prior value, captured at edit time (the Inspector already holds it via `componentsBySelected`, the
   hierarchy via `entities`, the environment panel via `environment`). Inverting an edit is "call the
   same control command with the value I held before" — `setTransform(id, priorT)`,
   `renameEntity(id, priorName)`, `setParent(id, priorParentId)`. Undo dispatches the inverse; redo
   re-dispatches the forward call; the reconcile poll confirms the result. Nothing in
   `control_dto.cppm` changes and `make engine` stays trivially green.

2. **History is keyed by `ViewTab.id`, one independent `TabHistory` per main tab.** The store holds
   `historyByTab: Record<string, TabHistory>` keyed on the stable `tab.id` string (`"scene"`,
   `"materialGraph:<materialId>"`), not on `kind`. The scene tab (`id: "scene"`, never closable) owns
   the scene-edit history for the app's life; a material-graph tab owns its own graph history.
   Undo/redo always act on the *active* tab's history (`activeViewTabId`), so Ctrl+Z in the scene tab
   never touches a graph's stack and vice-versa — matching "per main-tab basis." Read-only tabs
   (`flamegraph`, `asset`) never get a history (their edits don't exist); a history is created lazily
   on the first edit in a tab and deleted when the tab closes (`closeViewTab`).

3. **One gesture = one history entry, mirroring the write coalescer.** A gizmo drag or an Inspector
   field scrub is a burst of `set-transform`/`set-material` ticks already collapsed to one in-flight
   send by `makeCoalescer` (`coalesce.ts`). History collapses the same burst into a single
   `UndoableEdit`: capture the prior value at the drag *begin* bracket, push one entry at the *end*
   bracket — never one per coalesced tick. Discrete edits (rename, reparent, toggle, add) push one
   entry each. Undo itself replays through the same `client.ts` calls a normal edit uses, so the poll
   re-syncs from the engine's bumped `sceneVersion`/`selectionVersion` exactly as after any edit.

4. **The history primitive is pure and unit-tested in isolation (`lib/undo.ts`).** The `UndoableEdit`
   record + the `TabHistory` push/undo/redo/transaction state machine carry no React or control-client
   dependency — they take/return plain inverse descriptors over already-captured JSON-shaped values
   (ids are strings, transforms are numbers) — so they get `bun` unit tests, and the store slice wires
   them to `client.ts` + the reconcile poll.

5. **The engine mints unstable entity ids, so create is undo-only and delete is out.** `add-entity` /
   `create-entity` / `copy-entity` / `instantiate-model` return a fresh id; undoing a create is
   `destroy-entity` on that id (reachable, sound). But *redoing* a create would mint a **new** id that
   dangles every later history entry referencing the old one — so redo of a create is dropped/truncated
   (phase 06 makes the create record self-invalidating). `destroy-entity` has no inverse on the existing
   surface (re-creating a subtree needs a client snapshot + downstream id remap the engine can't help
   with under the constraint), so entity deletion is OUT.

6. **Selection is restored as UX context, not as a history entry.** Each entry records the selection
   active when the edit fired; undo restores that selection so the user sees what changed. But
   selection itself is editor view-state mirrored from the engine, not an authored-scene edit, so a
   bare select/deselect is never its own undo step.

7. **The material-graph tab snapshots the JS `MaterialGraph` model around each debounced apply.** The
   graph's canonical state between the debounced `material-set-graph` saves is the React Flow
   `nodes`/`edges` state (`materials/graph.ts`) — the engine only ever sees the debounced result. So a
   graph-tab record carries `{ before: MaterialGraph, after: MaterialGraph }` (via `flowToGraph`); undo
   loads `before` into the canvas (`graphToFlow` → `setNodes`/`setEdges`) and pushes it to the engine
   (`materialSetGraph`), re-rendering the preview. This is the second concrete per-main-tab history and
   the one place a local snapshot, not an inverse command, is the right shape.

8. **Mouse Back/Forward and Alt+Left/Right must be trapped app-wide or the SPA dies.** The webview
   treats button 3/4 and Alt+Arrow as history navigation; an unhandled one calls `history.back()` and
   reloads the React app, dropping all editor state. The handler `preventDefault()`s them globally and
   routes them to the active tab's history exactly like the keyboard bindings.

## Dependency / coordination notes

- **No engine changes anywhere in this plan.** `make engine` stays trivially green; no `se` command is
  owed (the engine is unaware of undo). Editor validation is `cd editor && bun run check` +
  `bun run lint`, plus `make prepare-for-commit` at phase boundaries.
- **Build on today's `ViewTab` system** (`store.ts`: `viewTabs` / `activeViewTabId`; kinds `scene` |
  `flamegraph` | `materialGraph` | `asset` | `rigEditor`). Coordinate with, but do not gate on,
  `plans/tabsystem-revamp` (NOT STARTED): it replaces the *panel dock* slices and leaves `ViewTab`
  untouched, and keying history by the stable `ViewTab.id` survives the revamp.
- **Mirror the coalescer, don't replace it.** `makeCoalescer` keeps its single-in-flight role for the
  wire; history brackets ride the *same* drag begin/end the gizmo and scrub fields already emit
  (`onDragStart`/`onFieldDragEnd` in `InspectorPanel`, gizmo `begin`/`end` in `ViewportPanel`). The two
  are independent: coalescing throttles sends, transactions group entries.
- **Respect the focus-gated poll + `dragActive`.** Undo dispatches an ordinary control call and lets
  the poll reconcile from the bumped version — it is atomic, so it needs no `dragActive` gate and no
  special poll path. Do not push history entries from the poll; only from user-initiated edits. Phase 6
  closes the poll/replay race (an in-flight reconcile must not clobber a just-replayed inverse; because
  the engine commits the inverse before replying, the poll that runs afterward reads the undone state).
- **e2e reach is limited and stated honestly.** `tests/e2e` drives the **engine** over the wire and has
  no React/Zustand, so an editor-only history has no direct e2e. The reachable engine-side assertion is
  "replaying inverse `client` commands restores prior state" — phase 6 issues a command, then its
  inverse, and asserts the `inspect`/`get-environment` round-trip. The history bookkeeping itself is
  covered by the `lib/undo.ts` bun unit tests. Add an undo/redo explanation page under `docs/content/`
  with its hub row (the concept is new).
- **No legacy / no compat shims:** there is one history path. When a phase routes an edit site through
  history, it routes *that* site, not a parallel copy.

## What done looks like

- In the scene tab: edit a transform (gizmo drag or Inspector scrub), material factor, component
  field, name, reparent, environment value, or render toggle, then Ctrl+Z restores the prior value and
  Ctrl+Shift+Z (and Ctrl+Y) re-applies it — each gesture is exactly one step, and the selection active
  at the edit is restored on undo.
- Creating an entity then Ctrl+Z removes it (undo-only; redo of a create is disabled).
- In a material-graph tab: add/connect/move/edit nodes, then Ctrl+Z reverts to the prior graph (canvas
  + engine preview), independently of the scene tab's history.
- Mouse Back = undo and Forward = redo, routed to the active tab's history; the webview never navigates
  (no SPA reload) and Alt+Left/Right are trapped too.
- The Edit menu shows Undo/Redo with correct enabled/disabled state and the next entry's label, scoped
  to the active tab; the bindings are rebindable in the settings registry.
- History clears on project/scene load, on scene swap, and on entering play mode; deferred operations
  are documented as such in the docs page.
- `cd editor && bun run check` + `bun run lint` clean; the `lib/undo.ts` bun tests pass; `make engine`
  stays trivially green (no engine changes); the reachable inverse-round-trip e2e passes under
  `make e2e`.
- No engine command is added for undo and no engine-side history/snapshot exists — the hard constraint
  is honored end to end.

## Phases

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`). Mark a
phase `COMPLETED` only when it is validation-clean for what it touches (editor phases: `bun run check`
+ `bun run lint` + the `lib/undo.ts` tests, plus `make prepare-for-commit` at the boundary; the e2e
assertion lands in phase 6). Phases are dependency-ordered and non-overlapping.

| Phase | Delivers | Status |
|---|---|---|
| [1 — history core + store slice](phase-01-history-core-and-store.md) | the pure `lib/undo.ts` `UndoableEdit`/`TabHistory` primitive (with bun tests) + the store slice holding one `TabHistory` per `ViewTab.id` with `pushEdit`/`undo`/`redo`/`beginEdit` + lifecycle clears. | NOT STARTED |
| [2 — route the in-scope scene edits through history](phase-02-route-scene-edits.md) | every in-scope scene-tab mutation records an `UndoableEdit` when it fires; gizmo/scrub bursts collapse to one entry per gesture via the existing drag brackets, discrete edits push one each. | NOT STARTED |
| [3 — keybindings + Edit menu](phase-03-keybindings-and-menu.md) | rebindable `edit.undo`/`edit.redo` (Ctrl+Z, Ctrl+Shift+Z + Ctrl+Y), a global handler dispatching to the active tab's history, and an Edit menu with correct enabled/disabled + next-entry label. | NOT STARTED |
| [4 — mouse Back/Forward → undo/redo + kill the webview default navigation](phase-04-mouse-back-forward.md) | mouse button 3 = undo / 4 = redo across the app, and the webview's default Back/Forward (incl. Alt+Left/Right) is suppressed so no input ever navigates the webview. | NOT STARTED |
| [5 — per-tab material-graph history](phase-05-material-graph-history.md) | each material-graph tab gets its own history keyed by its `ViewTab.id`, snapshotting the `MaterialGraph` before/after each debounced apply; undo loads the prior graph into the canvas + pushes it to the engine. | NOT STARTED |
| [6 — edge cases, hardening, docs, e2e](phase-06-edge-cases-and-docs.md) | entity-create redo handling, history invalidation on project load / scene swap / play mode, the poll/replay race, the docs page, and the one reachable e2e assertion. | NOT STARTED |

## Explicitly OUT (deferred)

- **Entity deletion (`destroy-entity`)** — undo needs a client subtree snapshot + downstream id
  remapping that breaks pure inverse-command replay (the cross-engine norm); the engine cannot grow a
  snapshot under the hard constraint, so deletion stays non-undoable.
- **Redo of entity creation / duplication / instantiate** — re-creation mints new unstable entity ids
  that dangle later history entries; v1 undoes a create but drops/truncates its redo.
- **Selection / deselection** — editor view-state mirrored from the engine, not an authored-scene
  edit; undo restores the selection *context* as UX, but selection changes are not entries.
- **Gizmo op / space / preserve-children** — UI mode, not scene state, so not undoable.
- **Play transport (play/pause/stop/step) and animation preview (play/seek/pause/stop-preview)** —
  transport is not an edit, `step` is irreversible, and entering play also invalidates the scene
  history (the play duplicate is a different scene).
- **Editor camera (`set-camera` / `focus`)** — the editor holds no reliable prior camera; this is view
  navigation, not a scene edit.
- **All Assets-panel + material-asset + filesystem mutations** — rename/move/create-folder/extract/
  delete (asset + folder)/import (model + texture)/reimport/rescan/material-create/update/assign/
  compile. Kept whole-or-nothing out so the user-facing rule stays "scene edits and the open material
  graph are undoable": these are filesystem/catalog operations whose inverses either don't exist on the
  surface or are destructive/side-effectful on disk. The material *graph* model inside a graph tab is
  in (it is the graph, not a catalog row).
- **Cross-tab / global undo, a visible history panel, undo-merge of consecutive same-field edits,
  persisting history across reloads** — later polish, out of v1.
