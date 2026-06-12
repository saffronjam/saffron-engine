# Phase 05 — portal panel host (state survives moves)

**Status:** NOT STARTED

## Goal

Make panel React state independent of *where* the panel is docked, before any cross-dock
movement exists. `DockPanelsHost` renders every open panel exactly once, flat at app root,
portaled into a per-panel host div; leaf bodies claim hosts imperatively. Because the React
tree's shape never changes when a panel moves, component state, refs, and DOM survive —
the mechanism dockview uses, hand-rolled (~70 lines, no dependency). An invisible refactor.

## What exists to build on

- React reconciles by position+type+key: a panel re-rendered under a different parent
  unmounts and remounts (react.dev "Preserving and resetting state"; react#3965). Keys do
  not help across parents — hence portals into stable hosts.
- The state that must survive: `MaterialEditorPanel.tsx:48-56` — local `materials`,
  `fields`, GPU `preview` blob, coalescers; `RightSidebar.tsx:70-73` documents today's
  keep-mounted policy. Timeline (canvas), Profiler (flame chart), Assets (history/selection/
  marquee) all hold remount-sensitive local state too.
- The phase-03/04 leaves render content per-site; this phase unifies content rendering.

## Work

### 1. `components/dock/panelRegistry.tsx`

```ts
export interface DockPanelDef {
  id: DockPanelId;
  title: string;                          // absorbs TOOL_LABEL / BOTTOM_TOOL_LABEL
  icon?: LucideIcon;
  closable: boolean;
  renderer: "always" | "onlyWhenVisible"; // keep-mounted vs unmount-when-hidden
  component: React.ComponentType;
  defaultLeafId: DockNodeId;              // openPanel fallback target
}
```

The full table, declared now: `stats`, `profiler`, `material`, `timeline` → `closable: true`;
`inspector`, `environment`, `render`, `hierarchy`, `assets`, `viewport` → `closable: false`.
`renderer: "always"` for `material` (GPU preview) and `assets` (thumbnails/selection);
`"onlyWhenVisible"` for the rest. `hierarchy`/`assets`/`viewport` join the tree in phase 07;
their registry rows are inert until then.

### 2. `components/dock/DockPanelsHost.tsx`

- A module map `panelHosts: Map<DockPanelId, HTMLDivElement>` — one lazily-created
  `div.h-full.w-full` per *open* panel, owned by the map for the panel's open lifetime,
  destroyed only on `closePanel`.
- The component (mounted once in `App.tsx`) renders, flat and keyed by panel id:
  `createPortal(<Def.component />, hostFor(id), id)` for every open panel whose `renderer`
  is `"always"`, and for visible panels whose renderer is `"onlyWhenVisible"`.
  **`visible(id)` ⇔ some leaf in `dockLayout` has `activeTab === id`.** Region mount state
  and the App-level `display:none` while an asset ViewTab is active are deliberately
  ignored — CSS already hides the host there, and unmounting would lose state the current
  dock keeps (`App.tsx:214-222`).
- A leaf body claims the hosts of **all** tabs it owns via ref callback `appendChild`,
  toggling `display` so only `activeTab` shows. The renderer policy decides what is *in* a
  hidden host: `"always"` → portal content stays mounted in the hidden host (the
  RightSidebar policy, generalized); `"onlyWhenVisible"` → portal content unmounts, the
  empty host div persists, attached and hidden.
  **Claim/release semantics:** React detaches the old ref before attaching the new one
  within a commit, so a host may be momentarily detached — release callbacks must never
  destroy the host (only the map owns lifetime). The detached-rect window is safe: the
  viewport's `computeBounds` skips degenerate rects, and the phase-03 settle subscriber
  re-commits bounds after every `dockLayout` change.
- Known caveat (accepted): `appendChild` reparenting drops text selection and would reload
  iframes — we have neither in panels.

### 3. Adopt in the three sites

`LeftBottomTabs`, `RightSidebar`, `BottomDock` bodies become host-claiming divs; the
per-site content policies (Radix `TabsContent`, `display:none` blocks, conditional render)
are deleted — the renderer policy in the registry now decides. `BottomDock`'s
unmount-inactive quirk disappears; Timeline switches to `"onlyWhenVisible"` semantics
identical to today's (it already unmounted when inactive).

## Verify

- `bun test` + `cd editor && bun run check`; `make prepare-for-commit` clean.
- Invisible refactor — manual via `make run`:
  - **Gate:** Material panel survives a forced host re-claim (toggle between right-sidebar
    tabs, then close/reopen an *adjacent* tool so the leaf re-renders): selected material,
    field edits, and the preview image persist with no re-fetch (watch the control-plane
    log for absent `preview-render` bursts).
  - Timeline canvas intact across tab switches; Profiler flame chart state survives
    switching away and back.
  - Inspector/Environment/Render behave as before (unmount-when-hidden — no metrics or
    poll regressions).
  - Viewport re-glue unaffected (host claiming triggers no spurious bounds churn).
