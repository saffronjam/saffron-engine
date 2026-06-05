# Phase 5 — Editor: playback toolbar, store, hotkeys, tint + docs page

**Status:** NOT STARTED

**Depends on:** phase 3 (regenerated protocol types). Independent of phase 4.

The user-facing surface: a Topbar playback group, the store slice fed by the existing
reconcile poll, the Ctrl+P hotkey family, an unmistakable play-mode tint, and the explanation
page. The decided model is live-tune-and-discard: panels stay interactive during play (they
address the running scene), the tint + discard semantics are the guardrail, and only
save/load grey out.

## Store + client

- `editor/src/control/client.ts`: typed wrappers `play()`, `pause()`, `stop()`,
  `step(frames?)`, `getPlayState()`; extend the hand-kept `CommandResultMap` with the five
  commands → `PlayStateResult` (types arrive from the regenerated `@saffron/protocol`).
- `editor/src/state/store.ts`: `playState: "edit" | "playing" | "paused"` + `playVersion`
  slices with an optimistic `setPlayState` action. The fast poll already destructures
  `get-selection` (`store.ts:643`); the extended `SelectionResult` now carries
  `playState`/`playVersion` — apply on change, and let the `stop`-driven `sceneVersion` bump
  flow through the existing `refreshHeavyState` branch (`store.ts:529`) so the tree/inspector
  snap back to the authored scene with no new plumbing. Engine-side state changes (e.g.
  `se play` from a shell) reconcile in through the same poll, exactly like the gizmo buttons.

## Topbar (`editor/src/panels/Topbar.tsx`)

A playback group to the left of the gizmo group, same chrome
(`flex items-center gap-0.5 rounded-md border border-border bg-background p-0.5`,
`role="group"`, `size="icon-sm"`, lucide icons, `disabled={!ready}`, tooltips with the
hotkey):

- **Play / Pause** (context-sensitive primary): `Play` icon in `edit` and `paused` (resume),
  `Pause` icon while `playing`. Active styling (`variant="default"`) while not in `edit` —
  the button is also the state indicator, UE-style.
- **Stop** (`Square`): enabled when `playState !== "edit"`.
- **Step** (`StepForward`): enabled only when `paused`; click = `step()`.

Clicks follow the established optimistic pattern (`Topbar.tsx:24-31`): set the store, fire
the command, `.catch` rolls back, the poll reconciles. When a `play` result carries
`hasPrimaryCamera: false`, raise the existing notification surface ("No primary camera — using
the editor camera"; `Notifications` is already mounted in the Topbar).

## Hotkeys (`editor/src/app/useGizmoShortcuts.ts`)

The Ctrl+P family (locked decision), gated like W/E/R on `phase === "ready"` and not-typing:

- `Ctrl+P` — play/stop toggle (`edit → play`, otherwise `stop`).
- `Ctrl+Shift+P` — pause/resume (only meaningful outside `edit`).
- `Ctrl+Alt+P` — step (only when `paused`).

Note `Ctrl+P` shadows the browser print dialog inside the webview — `preventDefault()` like
the existing handlers.

## Play tint + locks

- **Tint** (`editor/src/app/Layout.tsx`): when `playState !== "edit"`, an unmissable but
  non-obstructive marker on the chrome — an amber inner ring/top border on the dock root
  (e.g. `ring-2 ring-inset ring-amber-500/50`) and an amber tinge on the Topbar. The viewport
  itself stays untinted (it is the game view). This is Unity's playmode-tint lesson: the tint
  is the single defense against "edited in play, lost it on stop".
- **Locks**: `ProjectMenu` save/load/new entries disabled while playing (matching the
  engine-side load guard; save is greyed to avoid "saved my play tweaks" confusion even though
  saving is harmless under the duplicate). Gizmo T/R/S + world/local buttons and W/E/R
  shortcuts disabled (the overlay is hidden during play, phase 2). Everything else —
  hierarchy, inspector, environment, selection in the viewport — stays live against the
  running scene; their writes are discarded on stop by construction.

## Docs (same change)

- New explanation page `docs/content/explanations/ui-and-editor/play-mode.md`: the play/pause/
  stop/step model, the scene-duplication discard semantics ("the authored scene is never
  touched during play"), camera handover + fallback, live-tune-and-discard, hotkeys. Slim
  code-pointer table (`scene_edit_play.cpp`, `host.cppm`, `Topbar.tsx`, `store.ts`). Run the
  humanizer pass.
- Hub row in `docs/content/explanations/ui-and-editor/_index.md`.
- Cross-link from the `editor-camera` and `gizmo` pages where behaviour changed (gizmo hidden
  during play; viewport camera source).
- Mark this plan's phases `COMPLETED` as they land; delete the folder only after all are.

## Touched

| What | File | Symbols |
|---|---|---|
| Wrappers + result map | `editor/src/control/client.ts` | `play/pause/stop/step/getPlayState` |
| Slice + poll | `editor/src/state/store.ts` | `playState`, `playVersion`, fast-poll apply |
| Buttons | `editor/src/panels/Topbar.tsx` | playback group |
| Hotkeys | `editor/src/app/useGizmoShortcuts.ts` | Ctrl+P family |
| Tint | `editor/src/app/Layout.tsx` | play-mode ring |
| Locks | `editor/src/app/ProjectMenu.tsx`, `Topbar.tsx` | disabled states |
| Docs | `docs/content/explanations/ui-and-editor/` | `play-mode.md` + `_index.md` |

## Verify

- `bun run check` + `bun run build` clean; `make lint` clean.
- Manual `bun run tauri:dev`: Ctrl+P cuts the viewport to the scene camera and tints the
  chrome; pause freezes, step advances one tick at a time; tweak a light in the Inspector
  during play, stop — the tweak reverts and the tree re-syncs; the no-camera warning fires in
  an empty scene; `se play` from a shell flips the toolbar within a poll cycle.
