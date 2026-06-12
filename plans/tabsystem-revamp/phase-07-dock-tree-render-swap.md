# Phase 07 — DockRoot: render the tree

**Status:** NOT STARTED

## Goal

Swap `Layout`'s hard-coded region composition for a recursive `DockRoot` that renders the
`dockLayout` tree: branch → `ResizablePanelGroup` + `ResizableHandle`s, leaf →
`TabStrip size="dock"` + host-claiming body. Hierarchy, Assets, and the Viewport join the
tree as leaves; the interim `persistent` flags drop; the left trio unpins. The largest
phase, deliberately de-risked by 01 (rrp spike), 05 (hosts), and 06 (drag) — it changes
*rendering*, not the model, the drag layer, or panel content.

## What exists to build on

- `app/Layout.tsx` today: pixel-width `aside` sidebars with hand-rolled resizers
  (`:135-147`), two rrp groups (`useDefaultLayout` ids `saffron.layout.left/right:`,
  `:72-73`), conditional right/bottom regions (`:208-238`), `clampSidebarWidth` (`:243-249`,
  `VIEWPORT_MIN_WIDTH = 520`).
- The phase-01 spike's verdict on dynamic rrp structure + the chosen reconciliation
  mechanism (imperative `setLayout` or keyed remount by structure hash + rAF-force settle).
- The viewport contract: exactly one `viewport-host` div as the rect source
  (`ViewportPanel.tsx:622`); its ResizeObserver + the debounced end tier handle splitter
  drags. `viewportHidden` stays out of the dock system's hands — its drivers are the
  scene-tab effect (`App.tsx:190-197`) and the existing modal park paths
  (`ProjectStartupModal`, the asset View modal); the dock swap must not add one.
- Dead code to retire: `persistBottomDockHeight`/`loadBottomDockHeight`
  (`store.ts:918-945`, no callers), the sidebar width helpers (`store.ts:854-913`) once
  pixel sidebars die.

## Work

1. **`components/dock/DockRoot.tsx`** — recursive: a `DockBranch` renders a
   `ResizablePanelGroup` (orientation from the node) whose children render in
   `ResizablePanel`s sized from `branch.sizes`, `onLayoutChanged` → `setBranchSizes` →
   (debounced) persist + `emitLayoutSettled()`; a `DockLeaf` renders the strip + the
   host-claiming body (locked leaf: strip hidden). Each leaf wrapper carries
   `data-dock-leaf="<leafId>"` — the phase-06 attribute moves off `Layout`'s fixed slots,
   or the drag layer silently loses every target.
2. **The default tree** reproduces today's layout exactly: root horizontal →
   [left vertical: hierarchy / leftBottom-trio leaf] · [center vertical: viewport /
   assets (/ bottom when occupied)] · [right when occupied]. Default percents match
   today's (`72/28` center, `45/55` left, `Layout.tsx:164/173/194/200`); the root branch
   converts today's pixel sidebars (280 left / 320 right, `Layout.tsx:45-49`) to percents
   at first mount against the window width, with px `minSize` doing the real guarding.
3. **Viewport leaf:** `locked` — non-closable, tab not draggable, no strip chrome, no merge
   drops; px `minSize` 520 replaces `VIEWPORT_MIN_WIDTH`; px minimums on the sidebar leaves
   preserve the cannot-collapse-while-attaching guarantee (spiked at 1280×720 in phase 01).
   Edge splits *beside* the viewport remain legal (phase 08) — they insert siblings, never
   occlude.
4. **Unpin the trio:** drop `persistent` from the three well-known leaves; `normalize` now
   collapses any emptied leaf; Inspector/Environment/Render become draggable
   (`closable: false` stands — a closed-nowhere structural panel cannot exist), and the
   interim `acceptsTabs: false` on `leaf:leftBottom` is lifted — the trio's leaf becomes a
   normal merge target. The `openPanel` terminal fallback (append a leaf to the root)
   becomes reachable — exercise it in the bun tests. **Reveal bands switch to
   recreate-on-drop:** with `persistent` gone, `leaf:right`/`leaf:bottom` can vanish from
   the model, so `movePanel` targeting a missing well-known leaf id re-inserts that leaf at
   its canonical position in the default tree before inserting the panel (pure-model
   change, bun-tested) — the phase-06 bands keep working without the persistence crutch.
5. **Panels menu:** a registry-driven section in the Topbar wrench menu
   (`panels/Topbar.tsx:299-312`) — one `openPanel(id)` item per closable panel — plus
   **Reset layout** → `resetDockLayout()` + rAF-force settle. Every panel now has a
   no-drag reopen path; flipping a structural panel to `closable: true` later is a
   one-field registry change.
6. **Retire** the pixel-sidebar resizers and width persistence, the rrp `useDefaultLayout`
   ids, and the dead bottom-dock-height helpers; sizes now live solely in
   `DockBranch.sizes` under the phase-06 key.

## Verify

- `bun test` (terminal fallback, normalize-collapse paths) + `bun run check`;
  `make prepare-for-commit` clean.
- Manual via `make run`, the full viewport checklist:
  - Split-drag every handle around the viewport: subsurface tracks live, exact-glues on
    release; no per-tick engine resizes (watch the log).
  - Open/close right and bottom regions via drops and closes; project reload; 1280×720
    window: viewport never collapses below its min, sidebars hold their minimums.
  - Drag the Inspector into the bottom dock; its old leaf collapses; reopen via the
    Hierarchy deep-link (`openPanel`) → it lands at `lastLocation`.
  - Persisted round-trip incl. a hand-reorganized tree; a tree with stale `lastLocation`
    entries loads with them pruned (`validate`).
  - Asset/flamegraph/materialGraph main tabs still park the viewport; returning to Scene
    restores it (the `display:none`-not-unmounted dock contract, `App.tsx:214-222`).
