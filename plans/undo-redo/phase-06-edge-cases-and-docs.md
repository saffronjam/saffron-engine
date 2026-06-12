# Phase 6 — Edge cases, hardening, docs, e2e

**Status:** NOT STARTED

**Depends on:** phases 01–05 (the full feature exists — the pure history primitive, the routed
scene edits, the keybindings + Edit menu, mouse Back/Forward, and the per-tab material-graph
history). This phase hardens the boundaries those open and documents the result; it adds no new
behaviour, only correctness guards, docs, and one engine-side e2e assertion.

## Goal

Undo/redo is now reachable everywhere, but it opens three correctness boundaries that the earlier
phases left to here: an undone entity-create cannot be cleanly redone (re-creation mints a new id),
the history must be invalidated whenever the scene under it is replaced (project/scene load, scene
swap, play enter), and a poll tick that lands mid-replay must read consistent state. Close all three,
then write the docs page that explains the feature honestly (the engine has no idea undo exists) and
add the single e2e assertion that is genuinely reachable — the engine round-tripping an inverse
command. After this phase the feature is safe at its edges and explained.

## What exists to build on

- `resetSceneState()` (`state/store.ts`) is the hard scene reset on every project/scene load: it
  clears entities/selection/`componentsBySelected`/assets/environment, rebuilds `viewTabs` to just
  the non-closable scene tab, and forces `sceneVersion`/`selectionVersion` to `-1` so the next
  reconcile tick re-fetches against the new scene. Phase 01 added `clearSceneHistory()` and hooked it
  in here; this phase confirms it covers every orphaned-tab case.
- The reconcile poll's scene-swap signal already exists: in `refreshHeavyState` (`state/store.ts`)
  the branch `if (sceneVersion < previousSceneVersion)` fires `invalidateThumbnails()` — a backwards
  version step is the engine telling the editor "this is a different scene", the exact moment a stale
  inverse command must be dropped.
- The play-mode mirror lives in the poll's `tick` (`state/store.ts`): on a `selection.playVersion !==
  knownPlayVersion` change it calls `live.setPlayState(selection.playState as PlayState)`;
  `setPlayState` is the optimistic setter the Topbar also calls. Entering play duplicates the scene
  engine-side and Stop drops the duplicate (`scene_edit_play.cpp`, see the play-mode doc), so the
  authored scene the editor's inverse commands target is *not* the one running during play.
- The `redoable` flag on `UndoableEdit` and the non-redoable entity-create entries (phases 01–02):
  an entity-create records an undo (destroy the created id) but is marked `redoable: false`.
- The `historyReplaying` flag and the `if (historyReplaying) return;` guards at the edit sites
  (phases 02 and 05): a replay must not record itself as a fresh entry.
- `tests/e2e` (bun, drives the engine over the wire via `@saffron/protocol` through `harness.ts`;
  entities addressed by name per `scene.test.ts`). The only automated reach here is asserting the
  engine round-trips an inverse command — it cannot see the React history.
- `docs/content/explanations/ui-and-editor/_index.md` (the hub row table) and its siblings
  (`gizmo.md`, `inspector.md`, `selection.md`, `play-mode.md`) for house style and cross-links.

## Work

### 1. Entity-create redo policy (truncate-on-undo)

An entity-create entry can be undone (its `undo` thunk destroys the created entity via
`client.destroyEntity`) but its redo is meaningless: re-creating mints a *new* id, so any later
history entry that closed over the destroyed id (a transform on it, a rename of it, a component write)
would replay against a vanished entity.

1. When `undo` pops a `redoable: false` entry, do **not** push it onto `future`. Drop it, and clear
   the rest of `future` at that point — the redo branch above an unredoable undo is unreachable by
   construction. Implement this in the pure primitive (`lib/undo.ts`) so the store action stays a thin
   caller: `takeUndo` already returns `{ edit, next }`; have it omit a non-redoable edit from
   `next.future` (and the store action simply does not re-push it).
2. The Edit menu's redo item (phase 03) reflects this: `redoLabel`/`canRedo` already read the top of
   `future`, so once the unredoable create is dropped the menu shows the next real redo or "Nothing to
   redo". No extra menu wiring — the truncation makes the menu correct for free.
3. Sanity: a normal (redoable) edit undone *after* a create-undo behaves normally — only the
   create entry itself is non-restorable. Document the rule plainly (see step 4): "undo removes a
   created entity; you cannot redo it back."

### 2. History invalidation when the scene is replaced

Three distinct paths replace the scene the inverse commands target. Each must clear the **scene**
history (and any orphaned per-tab histories), never the material-graph histories, which are local JS
snapshots independent of the scene.

#### 2a. Project / scene load

Confirm `clearSceneHistory()` (wired into `resetSceneState()` in phase 01) runs on every load path.
Because `resetSceneState` already rebuilds `viewTabs` to just the scene tab, a material-graph tab open
against the old project is closed by that rebuild — so its history is now orphaned in
`historyByTab`. Make `clearSceneHistory()` (or a sibling called alongside it) drop every entry in
`historyByTab` whose tab id is no longer present in the rebuilt `viewTabs`, not only the `"scene"`
entry. This keeps `historyByTab` from leaking stacks keyed on tab ids that no tab will ever resolve
again.

#### 2b. Scene swap inside a session

A scene swap with no full reset shows up in the poll as `sceneVersion < previousSceneVersion` in
`refreshHeavyState` — the same backwards step that already drives `invalidateThumbnails()`. Add a
`clearSceneHistory()` call in that branch so a stale inverse can never target an entity that the
swapped-in scene does not contain. This is the in-session counterpart to 2a (which covers the
explicit load).

#### 2c. Play mode (conservative Option A)

On the `edit → playing` transition, clear the scene history. Hook it where the play state flips into
play: in the poll's `setPlayState(...)` call site in `tick` (guard on the previous value being
`"edit"` and the new value not being `"edit"`), or in a dedicated store subscriber on `playState`.
Whichever, the rule is: entering play once empties the scene history. Pre-play and in-play edits must
never share a stack — an inverse captured against the authored scene replayed during play would hit
the play duplicate (and vice versa on Stop), and bridging the two would mean capturing the authored
scene on play-enter and stepping back through the engine's play mechanics — exactly the engine
coupling the hard constraint forbids. Material-graph histories are untouched by play (the graph is not
the playing scene), so they survive a play session.

### 3. Poll/replay race guard

A replay is atomic — it `await`s one inverse command, the engine commits it before replying, and the
poll re-syncs from the bumped `sceneVersion`/`selectionVersion` on its next cheap tick. Confirm the
invariants the earlier phases established hold under a burst:

1. `historyReplaying` must **not** raise `dragActive`. A replay has no mid-gesture optimistic state to
   protect; `dragActive` is only the gate for a live drag. A poll tick that runs after a replay reads
   the engine's already-committed inverse state — race-free — because the engine commits before
   replying (the inverse is an ordinary command, drained like any other).
2. Verify the `if (historyReplaying) return;` guards (phases 02/05) mean no edit site records a new
   entry while a replay is in flight. A replay that fired through `client.setTransform` etc. must not
   re-enter `pushEdit`.
3. A rejected replay surfaces through `notifyError(errorText(err))` (the `lib/flash.ts` toast rule)
   plus a `console.error`, and leaves the stacks consistent: the failed edit stays where it was
   (a rejected undo leaves the edit on `past`; a rejected redo leaves it on `future`), so the next
   Ctrl+Z retries the same boundary rather than silently swallowing it. `historyReplaying` is cleared
   in a `finally`.

### 4. Docs: `undo-redo.md`

Add `docs/content/explanations/ui-and-editor/undo-redo.md` in house style (TOML front matter; the
sentence-case `# H1` must equal the front-matter `title`; lead with the concept and why, not "file X
does Y"; a slim `What | File | Symbols` table of code pointers). Cover:

- **The model:** undo/redo is purely the editor replaying inverse control commands it already holds —
  the engine has no undo concept, no history, no snapshot. State this as flatly as the play-mode doc
  states "Play does not mutate the scene you authored." Cross-link the control-plane page.
- **Per-ViewTab keying:** one undo/redo stack per main tab, keyed on `ViewTab.id`; Ctrl+Z in the scene
  tab never touches a material graph's stack. The scene tab's history lives for the app session
  (cleared only on the invalidation events); a material-graph tab's history is its own.
- **What's undoable in v1 vs deferred, and why:** scene edits (transform, material factors, asset
  assignment, component writes, add/remove component, script overrides, rename, reparent, environment
  + atmosphere, render toggles) and the open material graph are in; deletes, selection, play
  transport, and all asset/filesystem ops are out — an op is undoable only when its inverse is the
  same existing command fed a value the editor already held. Be honest about the create case: undo
  removes a created entity but cannot redo it.
- **Gesture-as-one-entry:** a gizmo drag or field scrub is one undo entry, not one per coalesced
  tick — the history hooks share the boundary the coalescer already uses.
- **Mouse Back/Forward:** mouse Back (button 3) = undo, Forward (button 4) = redo, and the webview's
  default Back/Forward navigation (and Alt+Left/Right) is suppressed so the single-page app stays
  alive.
- **Invalidation:** loading a project, swapping the scene, or entering play clears the scene history;
  material-graph histories survive play.

Add the hub row to `_index.md` (an `undo-redo` row in the Pages table, code column
`state/store.ts · lib/undo.ts · lib/keybindings.ts`). Run the `humanizer` pass over the prose.

### 5. e2e: the inverse-command round-trip

Add one `tests/e2e` case (a new file or a block in `scene.test.ts`) that proves the engine-side
replay *primitive* the React history depends on:

1. Create a named entity, issue `set-transform` to a value A, read it back via `inspect` (the
   `scene.test.ts` pattern).
2. Issue `set-transform` again to the prior value B — the same command an editor undo would replay —
   and assert `inspect` reports B and the run's log is validation-clean (the harness's standard log
   assertion). A `rename-entity` → inverse `rename-entity` pair is an equally valid second oracle.

State plainly in the test's header comment that this proves only the engine round-trips an inverse
command; it does **not** exercise the React history bookkeeping (the `historyByTab` stacks, gesture
grouping, truncate-on-create-undo), which the phase-01 bun unit tests cover. This matches the e2e
charter: it drives the engine over the wire, so an editor-only feature has limited reach here.

## Validation (done criteria)

- Inside the `saffron-build` toolbox, `cd editor && bun run check` + `bun run lint` are clean; the
  phase-01 bun history unit tests still pass; `make prepare-for-commit` is clean.
- The new e2e case passes under `make e2e` (the inverse `set-transform`/`rename-entity` round-trips and
  the log is validation-clean).
- Undoing an entity create removes the entity; redo after a create-undo is disabled (the Edit menu
  shows the next real redo or "Nothing to redo" — never a divergent resurrection).
- Loading a project, swapping the scene mid-session, and entering play each leave the Edit menu with
  nothing to undo in the scene tab; an open material-graph tab keeps its history across a play session.
- A burst of Ctrl+Z / mouse-Back never desyncs the UI from the engine — the poll reconciles after each
  replay; a rejected replay raises one toast and leaves the stack at the same boundary for a retry.
- `docs/`: the new page renders under `hugo server`, the hub `_index.md` row links it, and the
  humanizer pass has been applied.

## Notes / gotchas

- **Conservative play invalidation is the deliberate choice, not a shortcut.** Preserving undo across a
  play session would mean capturing the authored scene on play-enter and replaying inverses back
  through the engine's play mechanics — the engine coupling the hard constraint forbids. Clearing the
  scene history on `edit → playing` keeps the editor unaware of how play works.
- **Truncate-on-create-undo keeps ids honest.** Once a created entity is destroyed by undo, any later
  redo entry referencing its id is unsafe, so the redo branch is dropped. Full create/redo with id
  remapping is explicitly deferred (see the README's "Explicitly OUT" section); v1 undoes a create and
  drops its redo.
- **e2e cannot exercise the React history — say so.** The bun e2e suite drives the engine over the
  wire; it can only assert the engine replays an inverse command. The editor-only history (stacks,
  grouping, invalidation) is validated by the phase-01 unit tests plus the manual checklist above.
- **The docs must not imply engine awareness of undo.** State clearly that undo is editor-only replay
  of inverse commands and the engine has no undo concept — consistent with `AGENTS.md` and the hard
  constraint. The play-mode page's voice ("the answer every time — nothing") is the model to match.
- **No new engine command anywhere in this phase.** Every guard here is client-side store logic, a
  docs page, and one e2e that uses only existing commands (`set-transform`/`rename-entity`/`inspect`).
  If a step seems to want engine support, it is mis-scoped — rework it client-side or move it to the
  README's deferred list.
