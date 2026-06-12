# Phase 3 — Keybindings + Edit menu

**Status:** NOT STARTED

**Depends on:** phase 01 (the pure history primitive + the store's `undo`/`redo`/`pushEdit` actions
and the `historyByTab` slice), phase 02 (the in-scope scene edits record `UndoableEdit`s, so the
scene history actually has entries to replay).

## Goal

Make undo/redo reachable. Phases 01–02 built the engine of the feature and filled the scene tab's
history, but nothing yet *triggers* an undo. This phase adds the two reach surfaces a desktop editor
needs: the keyboard (`Ctrl+Z` undo, `Ctrl+Shift+Z` + a fixed `Ctrl+Y` alias redo, the first two
rebindable) and an Edit menu with live enabled/disabled state and a next-entry label ("Undo Move
entity"). Both surfaces dispatch into the **same** store action — `store.undo()` / `store.redo()` —
which resolves the active tab and decides what (if anything) to replay. After this phase scene-tab
undo works end to end: edit, `Ctrl+Z` restores, `Ctrl+Shift+Z`/`Ctrl+Y`/the menu re-applies, and the
focus-gated reconcile poll converges the UI from the engine's bumped version stamps. The engine stays
entirely unaware — every replay is an ordinary control command it has already seen.

This phase does **not** touch the mouse side buttons or the webview's `Alt+Left`/`Alt+Right`
navigation; that is phase 04. Here the handler reuses one window `keydown` listener exactly as the
gizmo shortcuts do.

## What exists to build on

- **`lib/keybindings.ts`** — the rebindable command registry. `CommandId` is a closed union;
  `CommandDef { id, label, category, kind, default, scope }`; `COMMANDS` is the ordered registry and
  **its order is the settings-modal order** (the modal reads `COMMANDS` directly). `matchesBinding(event, id, overrides)`
  does exact-modifier matching for `kind: "press"` via `normalizePressEvent` (which builds the
  `ctrl+`/`shift+`/`alt+`/`meta+` prefix string in that fixed order, lowercased key). `bindingFor(id, overrides)`
  returns the effective binding (override else default); `findConflict(forId, candidate, overrides)`
  flags a clash **within one `scope`** only; `formatBinding(def, value)` renders a chip-ready label
  ("Ctrl+Z", "Shift+Z"); `COMMANDS_BY_ID` indexes a def by id. `scope: "global"` commands all share
  the single window listener; a new `CommandDef` is picked up by the settings modal and the
  `setKeyBinding` → settings.json persistence for free — no per-command wiring to add.
- **`app/useGizmoShortcuts.ts`** — the global `keydown` pattern to mirror. It is one
  `window.addEventListener("keydown", …)` in a `useEffect`, gated in order by `isTextEntryFocused()`
  (INPUT / TEXTAREA / SELECT / contentEditable), then `store.settingsOpen`, then
  `store.engineStatus.phase !== "ready"`. The `Ctrl+P` play-family block
  (`(event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "p"`, `event.preventDefault()`)
  is the in-handler chord pattern to copy for the `Ctrl+Y` alias and the `metaKey`-covers-macOS
  detail. `isTextEntryFocused()` lives in this file and is the helper to reuse (lift it to a shared
  spot if the new hook lives in its own file — do not duplicate it).
- **`state/store.ts`** — `activeViewTabId: string` and the `viewTabs: ViewTab[]` lookup
  (`viewTabs.find((t) => t.id === activeViewTabId)`) resolve the active tab + its `kind` at dispatch
  time. The `ViewTab` union ids are stable strings (`"scene"`, `"flamegraph"`, the per-material
  `materialGraph` id, the asset / rigEditor ids). Phase 01 added `historyByTab: Record<string, TabHistory>`
  plus the `undo`/`redo` actions that resolve `tabId ?? activeViewTabId` and **no-op on read-only
  tabs** (flamegraph / asset / rigEditor that never got a history). This phase's handlers and menu do
  not reason about tab kind — they call `store.undo()` / `store.redo()` and let the action decide.
- **`app/App.tsx`** — the shell. `useGizmoShortcuts()` is called once in the `App` body; the new hook
  mounts the same way, right beside it. The Topbar's left cluster (`panels/Topbar.tsx`, the
  `min-w-0 items-baseline` flex) mounts `<ProjectMenu />`, which is itself a `DropdownMenu`
  (`app/ProjectMenu.tsx`) — the precedent for an Edit menu mounting as a sibling there.
- **`components/ui/dropdown-menu.tsx`** — re-exports `DropdownMenu`, `DropdownMenuTrigger`,
  `DropdownMenuContent`, `DropdownMenuItem`, `DropdownMenuSeparator`, and `DropdownMenuShortcut`
  (the right-aligned dim chip slot, ideal for the keybinding label inside an item). `Button`
  (`components/ui/button.tsx`) is the menu trigger element, matching `ProjectMenu`.
- **`lib/undo.ts`** (phase 01) — the pure history readers `emptyHistory()`, `canUndo` / `canRedo`,
  `undoLabel` / `redoLabel`, used by the menu so it never re-derives the stack shape inline.

## Work

### 1. Register `edit.undo` / `edit.redo` in the keybinding registry

In `lib/keybindings.ts`:

1. Add `"edit.undo"` and `"edit.redo"` to the `CommandId` union.
2. Add two `CommandDef` entries **at the top of `COMMANDS`**, so the Edit category leads the settings
   modal (the array order is the modal order):
   - `{ id: "edit.undo", label: "Undo", category: "Edit", kind: "press", default: "ctrl+z", scope: "global" }`
   - `{ id: "edit.redo", label: "Redo", category: "Edit", kind: "press", default: "ctrl+shift+z", scope: "global" }`
3. The `Ctrl+Y` redo alias is **not** a registered command — it is a fixed, non-rebindable alias, like
   the `Ctrl+P` play family that lives entirely inside the gizmo handler. `Ctrl+Z` and `Ctrl+Shift+Z`
   are exact-modifier strings, so they never collide; `Ctrl+Y` is matched separately in the handler
   (Work §2). Add a brief `///` note near the `edit.redo` entry so the single registered binding stays
   honest for `findConflict` — the alias is intentionally invisible to conflict detection (one
   registered redo binding, plus a hardwired alias). No change to `matchesBinding` / `findConflict` is
   needed; `scope: "global"` makes both new commands collide with the existing global press commands
   (gizmo W/E/R, focus, deselect) and only those.

### 2. A `useUndoRedoShortcuts` hook (the key dispatcher)

Add a sibling hook to `useGizmoShortcuts`, mounted in `App.tsx` next to the existing
`useGizmoShortcuts()` call (prefer a sibling to keep the gizmo hook focused). Put it in
`app/useUndoRedoShortcuts.ts` and lift `isTextEntryFocused()` into a small shared module both import,
or co-locate and export both from `useGizmoShortcuts.ts` — pick the smaller diff and do **not**
duplicate `isTextEntryFocused`.

The hook is one `window.addEventListener("keydown", …)` in a `useEffect`, reusing the exact gate
order from `useGizmoShortcuts`:

1. `if (isTextEntryFocused()) return;` — `Ctrl+Z` while typing in a field belongs to the field's own
   native text undo, never the scene history.
2. `const store = useEditorStore.getState();`
3. `if (store.settingsOpen) return;` — the settings modal owns the keyboard (its capture widget binds
   keys); never run undo underneath it.
4. `if (store.engineStatus.phase !== "ready") return;` — replays drive engine control commands.

Then, with `const overrides = store.keyBindings;`:

5. **Undo:** `if (matchesBinding(event, "edit.undo", overrides)) { event.preventDefault(); void store.undo(); return; }`
6. **Redo (registered binding + fixed alias):**
   ```
   const redoAlias = (event.ctrlKey || event.metaKey) && !event.shiftKey && !event.altKey
     && event.key.toLowerCase() === "y";
   if (matchesBinding(event, "edit.redo", overrides) || redoAlias) {
     event.preventDefault();
     void store.redo();
     return;
   }
   ```
   (`metaKey` covers macOS, mirroring the `Ctrl+P` block.)

The handler stays **tab-agnostic**: it never reads `activeViewTabId` or a tab kind. `store.undo()` /
`store.redo()` (phase 01) resolve the active tab, no-op on read-only tabs, and own the
`historyReplaying` guard and the `await edit.undo()`. The hook does not need a `.catch` like
`useGizmoShortcuts` uses for a single control call — the store action owns the replay's error
handling (a failed replay surfaces through the store action's own `notifyError`, per phase 01); the
hook just fires the action. Keep the `void` so the floating-promise lint stays clean.

> Always `event.preventDefault()` before dispatching, even when the active tab has nothing to undo. A
> focused-but-non-text element (a button, a panel div) can still let `Ctrl+Z` reach a webview-level
> edit-undo; preventing default stops that. `store.undo()` no-ops cheaply when there is nothing to
> replay, so the early `preventDefault` is safe.

### 3. The Edit menu

Add an Edit menu as a `DropdownMenu` sibling of `<ProjectMenu />` in the Topbar's left cluster
(`panels/Topbar.tsx`), trigger styled like the project menu's `Button`. A small dedicated component
(`app/EditMenu.tsx`, mirroring `ProjectMenu`) keeps Topbar lean.

The menu has two items, Undo and Redo. Each shows a live enabled/disabled state, the next-entry label
inline, and the keybinding chip. Read the active tab's history with **identity-stable selectors** —
subscribe to the derived booleans/labels, never the whole `historyByTab` map (a map subscription
re-renders the menu on every unrelated edit, violating the per-surface render discipline):

```
const overrides = useEditorStore((s) => s.keyBindings);
const canUndo = useEditorStore((s) => canUndo(s.historyByTab[s.activeViewTabId] ?? emptyHistory()));
const canRedo = useEditorStore((s) => canRedo(s.historyByTab[s.activeViewTabId] ?? emptyHistory()));
const nextUndo = useEditorStore((s) => undoLabel(s.historyByTab[s.activeViewTabId] ?? emptyHistory()));
const nextRedo = useEditorStore((s) => redoLabel(s.historyByTab[s.activeViewTabId] ?? emptyHistory()));
```

(`emptyHistory` / `canUndo` / `canRedo` / `undoLabel` / `redoLabel` are the pure `lib/undo.ts`
helpers from phase 01 — use them, do not re-derive inline. For a read-only tab the map has no entry,
so `emptyHistory()` yields `canUndo === false` and the items are disabled automatically. Alias the
selector locals if the helper names collide.)

For each item:

1. **Disabled** when its `can*` is false (`<DropdownMenuItem disabled={!canUndo} …>`). Read-only tabs
   are disabled by construction (no history entry). `onSelect={() => void store.undo()}` /
   `void store.redo()`.
2. **Label** is `canUndo ? \`Undo ${nextUndo}\` : "Undo"` (and likewise for redo). The label suffix is
   the recorded `UndoableEdit.label` ("Undo Move entity", "Redo Set albedo factor").
3. **Keybinding chip** via `DropdownMenuShortcut` showing
   `formatBinding(COMMANDS_BY_ID["edit.undo"], bindingFor("edit.undo", overrides))` ("Ctrl+Z") — read
   the *effective* binding (override-aware), so a rebind shows the new chord. The redo chip shows the
   registered `edit.redo` binding only; the `Ctrl+Y` alias is not surfaced (one chip per item, the
   primary binding).

No `title=` tooltips (editor/AGENTS.md). The keybinding chip already says what the shortcut is, so an
item tooltip would be redundant; if a "why disabled" hint is ever wanted, use the `Tooltip` primitive
on the trigger, not `title=`.

## Validation (done criteria)

Run inside the `saffron-build` toolbox: `cd editor && bun run check` (gen:protocol + `tsc --noEmit`)
and `bun run lint` (oxlint), both clean; then `make prepare-for-commit` clean (format + lint over the
touched TS). No new `bun run gen:protocol` output is expected — this phase adds **no** control DTOs
(it touches no engine wire), so `se-types.ts` is unchanged.

Behavioral checks (manual, via `tauri:dev` on a Wayland session):

- The settings modal lists **Undo** and **Redo** under an **Edit** category at the top; rebinding
  `edit.undo` to another chord takes effect on the very next keypress (the handler reads
  `store.keyBindings` live); `findConflict` flags a clash if a rebind collides with another global
  press command (e.g. binding `edit.undo` to `w`).
- In the scene tab: make an in-scope edit (move an entity, change a material factor), then `Ctrl+Z`
  restores the prior value and `Ctrl+Shift+Z` **and** `Ctrl+Y` both re-apply it; the Edit menu's
  Undo/Redo items enable/disable to match and show the right next-entry label ("Undo Move entity").
- `Ctrl+Z` does **not** fire while typing in an INPUT/TEXTAREA (native field undo wins) or while the
  settings modal is open.
- On a flamegraph or asset tab, `Ctrl+Z` / the Edit menu items are a no-op (the items render
  disabled).

This is editor-only and drives no new wire command, so it has **no `make e2e` reach** — the e2e suite
drives the *engine* over the socket and never simulates a webview keypress. Leave e2e untouched; the
poll-driven convergence of an undo replay (an ordinary control command) is exercised by the existing
per-command e2e coverage and is asserted directly in phase 06.

## Notes / gotchas

- **Exact-modifier matching keeps the two chords apart.** `edit.undo = "ctrl+z"` will not fire on
  `Ctrl+Shift+Z` (the normalized strings differ), and `edit.redo = "ctrl+shift+z"` will not fire on
  plain `Ctrl+Z`. Register redo as exactly `"ctrl+shift+z"`. The `Ctrl+Y` alias is matched by hand (a
  separate boolean), never through the registry, so `findConflict` only ever sees the one registered
  redo binding.
- **The handler dispatches; the store decides the tab.** Keep tab resolution inside `store.undo()` /
  `store.redo()` so the keyboard handler here, the menu items, and the phase-04 mouse handler all
  reuse one decision (active-tab lookup + read-only no-op + `historyReplaying` guard). Duplicating tab
  resolution in the hook would drift from the menu and the mouse path.
- **Always `preventDefault` `Ctrl+Z`,** even on a no-op tab, to stop a webview-level edit-undo
  reaching a focused non-text element. The store no-ops cheaply, so there is no cost.
- **No `title=` tooltips** (editor/AGENTS.md). The `DropdownMenuShortcut` chip makes an item tooltip
  redundant; reach for the `Tooltip` primitive only if a disabled-reason hint is later wanted.
- **Identity-stable menu selectors.** Subscribe to the derived `canUndo`/`canRedo` booleans and the
  string labels, not to `historyByTab` itself — a whole-map subscription re-renders the Edit menu on
  every unrelated tab's edit.
- **Nothing engine-side.** This phase registers commands, mounts a listener, and renders a menu — all
  in the editor. No new control command, no `control_dto.cppm` change, no engine awareness of undo. If
  a step seemed to need an engine call, the dispatch belongs in the store action replaying an
  *existing* command, not in a new one.
