# Phase 4 — Mouse Back/Forward → undo/redo + kill the webview default navigation

**Status:** NOT STARTED

**Depends on:** phase 03 (the `undo`/`redo` dispatch path already exists — the keybinding registry
entries, the global key handler, and the Edit menu all route through the store's tab-resolving
`undo`/`redo` actions). This phase adds a second input source for that same path; it builds no new
dispatch.

## Goal

Bind the mouse side buttons across the whole app: **Back (W3C `button` 3) = undo**, **Forward
(`button` 4) = redo**, working over every panel rather than only the viewport. In the same stroke,
stop the webview's default Back/Forward navigation — both the side-button
`history.back()`/`history.forward()` and the `Alt+Left`/`Alt+Right` keyboard equivalent — which would
otherwise tear the single-page app off its one URL. After this phase the side buttons drive the
*active tab's* history exactly as `Ctrl+Z`/`Ctrl+Shift+Z` do, and no input ever navigates the webview.

The mouse mapping is **fixed, not rebindable**: it does not enter the keybinding registry, which keys
off `event.key`/`event.code` and has no notion of a pointer button (`lib/keybindings.ts`).

## What exists to build on

- **The viewport's pointer model** (`panels/ViewportPanel.tsx`): the fly-cam effect's `onPointerDown`
  early-returns unless `event.button === 2` (RMB); the pick/gizmo effect's `onPointerDown`
  early-returns unless `event.button === 0` (LMB) and then takes `el.setPointerCapture(event.pointerId)`
  for the rest of the gesture. So the viewport already consumes only buttons 0 and 2 and is
  deliberately silent on buttons 3/4 — a window handler for the side buttons cannot collide with it.
  The viewport captures the pointer during a drag, which is exactly why this phase must key off
  `pointerdown` (capture redirects `auxclick` away per spec; see gotchas). W3C numbering: 0 = LMB,
  1 = MMB, 2 = RMB, 3 = Back, 4 = Forward.
- **The global key handler from phase 03** — a `window` `keydown` listener (the planned sibling
  `useUndoRedoShortcuts` hook mounted in `App.tsx` beside `useGizmoShortcuts()`) that already calls
  `event.preventDefault()` + `void useEditorStore.getState().undo()`/`.redo()` on the `edit.undo` /
  `edit.redo` bindings. It is the natural home for the `Alt+Left`/`Alt+Right` trap: same listener, same
  guards, same dispatch.
- **The guard pattern** (`app/useGizmoShortcuts.ts`): `isTextEntryFocused()`
  (INPUT/TEXTAREA/SELECT/contentEditable), `store.settingsOpen`, and
  `store.engineStatus.phase !== "ready"` are the three gates every global input handler in the editor
  applies before acting.
- **The store undo/redo actions** (phase 01, `state/store.ts`): `undo(tabId?)` / `redo(tabId?)` resolve
  `tabId ?? activeViewTabId`, look up `historyByTab`, and no-op on a read-only tab (`flamegraph`/`asset`)
  or an empty stack. The mouse path calls these unchanged — it carries no tab knowledge.
- **`editor/AGENTS.md`** records that the webview is locked to one URL with no real browser history, so
  `history.back()`/`history.forward()` are effectively no-ops in WebKitGTK; the reliable guard is a DOM
  `preventDefault()` on the originating input, and **no Rust-side handler is needed** (`tauri.conf.json`
  is already a single-page app). Both prevention layers in this phase are pure DOM.

## Work

### 1. The window-level pointer handler (a hook mounted in `App.tsx`)

Add a small hook (e.g. `app/useMouseHistoryNav.ts`) and mount it in `App.tsx` next to
`useGizmoShortcuts()` and the phase-03 `useUndoRedoShortcuts()`.

1. Register a single `pointerdown` listener on **`window`** (not the viewport `hostRef` div) so the
   side buttons act over Hierarchy, Inspector, Assets, the material-graph canvas — every panel — not
   only the transparent viewport hole.
2. Branch on `event.button`:
   - `3` (Back) → `event.preventDefault()` + `void useEditorStore.getState().undo()`.
   - `4` (Forward) → `event.preventDefault()` + `void useEditorStore.getState().redo()`.
   - any other button → return immediately, untouched, so the viewport's own `button === 0`
     pick/gizmo and `button === 2` fly-cam handlers run exactly as before.
3. Gate it like the key handler: read `useEditorStore.getState()`, return early while
   `store.settingsOpen`, and act only when `store.engineStatus.phase === "ready"` (a replay issues
   engine commands; an undo with no live engine is meaningless). **Do not** gate on
   `isTextEntryFocused()` — a side-button click is not text entry, and the user expects Back to undo
   even while a field has focus. This is the one deliberate divergence from the key handler's guards.
4. The store actions own tab resolution and the read-only no-op ("the handler dispatches; the store
   decides the tab"), so this hook stays tab-agnostic and is a near-duplicate of the key handler's two
   dispatch lines — by design, so both input sources share one decision.

### 2. Trap `Alt+Left`/`Alt+Right` in the phase-03 key handler

In the phase-03 global key handler, **before** the `edit.undo`/`edit.redo` `matchesBinding` checks:

1. Match `event.altKey && (event.key === "ArrowLeft" || event.key === "ArrowRight")`.
2. On a match, `event.preventDefault()` — this is what actually stops WebKitGTK's history navigation,
   `Alt+Left`/`Alt+Right` being the keyboard form of the side buttons — then route `Alt+Left` →
   `undo()` and `Alt+Right` → `redo()`, consistent with the mouse mapping (Back = undo, Forward =
   redo). This both kills the navigation and gives a keyboard equivalent of the side buttons.
3. Place the trap above the `Ctrl+Z`/`Ctrl+Shift+Z` matches so the nav chord is consumed regardless of
   which `edit.*` bindings the user has set. It is fixed (not rebindable), like the `Ctrl+Y` redo alias
   phase 03 hard-codes — document it in the same place phase 03 documents that alias.

### 3. Confirm no SPA navigation slips through

The defense is exactly two DOM layers — the button-3/4 `preventDefault()` (step 1) and the
`Alt+Left`/`Alt+Right` `preventDefault()` (step 2). There is **no Rust change**: the webview has no
real history to navigate, so `preventDefault()` on the originating input is sufficient.

1. Verify the project stays loaded and the reconcile poll keeps running after pressing Back/Forward
   repeatedly (the poll is the focus-gated loop `startReconcile(client)` starts in `App.tsx`; a page
   reload would reset the store and re-probe readiness — that must never happen).
2. Confirm in devtools that no `popstate`/navigation event fires on a side-button press or `Alt+Arrow`.

## Validation (done criteria)

- `cd editor && bun run check` + `bun run lint` clean (run inside the `saffron-build` toolbox);
  `make prepare-for-commit` clean.
- Manual via `make run`: mouse **Back undoes** and **Forward redoes** the *active tab's* last edit
  identically to `Ctrl+Z`/`Ctrl+Shift+Z`; `Alt+Left`/`Alt+Right` do the same and never navigate the
  webview.
- Pressing Back/Forward (and `Alt+Left`/`Alt+Right`) repeatedly never reloads the page or stalls the
  reconcile poll; devtools confirms no `popstate`/navigation occurred.
- The viewport's left-click pick and right-click fly-cam still work — the new window handler consumes
  only buttons 3/4, and the viewport's existing button-0/button-2 handlers are untouched.
- On a read-only tab (`flamegraph`/`asset`) a side-button press is a no-op (the store action bails),
  matching `Ctrl+Z` there.
- **e2e reach: none.** `tests/e2e` drives the *engine* over the wire; pointer-button input, webview
  navigation, and `preventDefault` are editor-only DOM concerns with no control-plane surface, so there
  is nothing for the bun e2e suite to assert here. This phase is validated by `bun run check`/`lint`
  plus the manual `make run` checks above.

## Notes / gotchas

- **Use `pointerdown`, not `auxclick` or `mouseup`.** Under the viewport's `setPointerCapture` (active
  during a gizmo/fly drag) `auxclick` does not fire per spec, and `mouseup` is the wrong moment; the
  viewport already proves `pointerdown`/`pointerup` fire reliably under capture. `pointerdown` is also
  early enough to `preventDefault()` the webview's default Back/Forward navigation.
- **Listen on `window`, not the viewport `hostRef` div.** The viewport div is a transparent hole over
  the subsurface; binding there would make Back/Forward work only when the cursor is over the viewport
  rectangle. The side buttons must work over Hierarchy/Inspector/Assets/material-graph too, so the
  listener belongs on `window` — exactly like the key handlers.
- **The mapping is fixed, not rebindable.** Mouse buttons are absent from `lib/keybindings.ts` (its
  `CommandId`s and `matchesBinding` key off `event.key`/`event.code`); rebinding the side buttons is
  later polish, listed in the README's deferred section, not this phase.
- **No double-handling with the viewport.** The viewport's pointer effects act only on `button === 0`
  (pick/gizmo) and `button === 2` (fly-cam), so a window handler scoped to buttons 3/4 cannot consume a
  button the viewport wants, and vice versa. Keep the `button` branch strict (`=== 3` / `=== 4`, else
  return) so the contract holds even if the viewport later claims another button.
- **Not gated on text focus, by design.** Unlike `Ctrl+Z`, a side-button Back must undo even while an
  Inspector field has focus — clicking a mouse button is not typing. This is the intended deviation; do
  not copy `isTextEntryFocused()` into the pointer hook.
- **No engine awareness.** A replay through `undo()`/`redo()` is an ordinary edit to the engine that
  bumps `sceneVersion`/`selectionVersion`; the reconcile poll re-syncs on its next tick. Nothing in
  this phase touches the engine, the control DTOs, or `client.ts` — it is two DOM listeners feeding the
  existing store actions.
