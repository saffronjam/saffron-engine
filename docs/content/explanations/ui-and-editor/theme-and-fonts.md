+++
title = 'Theme & fonts'
weight = 10
+++

# Theme & fonts

The editor's React UI is built with shadcn/ui (Radix primitives copied into the repo) on Tailwind CSS v4, not hand-rolled CSS. The dark `theme::` palette from the old engine survives — it is mapped one-to-one onto shadcn's CSS tokens, and the app is forced dark. The two fonts carry over too: Roboto for the chrome, Roboto Mono for data fields.

## The palette as shadcn tokens

shadcn styles every component from a small set of CSS custom properties (`--background`, `--primary`, `--border`, …). The editor sets those variables to the engine's exact theme hexes, so the React UI reads as the same dark editor the C++ build did:

```css
:root {
  --background: #242424;   /* theme::background */
  --foreground: #c0c0c0;
  --primary:    #ec9e24;   /* the accent orange */
  --input:      #0f0f0f;   /* propertyField */
  --border:     #4d4d4d;   /* muted */
  --muted:      #2f2f2f;   /* groupHeader */
  --popover:    #3f464d;
  --ring:       #ec9e24;
  --radius:     0.15rem;
}
```

`index.html` pins `class="dark"` on `<html>` and the same hexes drive both `:root` and `.dark`, so there is no light mode to fall into — the editor runs in one known-modern webview, so a single forced-dark palette is all it needs. An `@theme inline` block re-exports each variable as a Tailwind color (`--color-background: var(--background)`) so utilities like `bg-background` and `border-border` resolve to the palette.

## Two fonts, one monospace

Data reads better in a monospace face, so the chrome uses Roboto and number/data fields use Roboto Mono — the same two TTFs the C++ editor loaded. Tauri runs offline (no font CDN), so both are bundled and declared with `@font-face`; Vite fingerprints the `.ttf` into the build:

```css
@font-face { font-family: "Roboto";      src: url("./assets/fonts/Roboto-Regular.ttf") format("truetype"); }
@font-face { font-family: "Roboto Mono";  src: url("./assets/fonts/RobotoMono-Regular.ttf") format("truetype"); }
```

`--font-sans` names Roboto and `--font-mono` names Roboto Mono. Chrome (labels, buttons, menus, hierarchy rows) uses `--font-sans`; data — the inspector number/vector/color inputs, the entity and asset names, the render-stats counters — uses `font-mono`, mirroring the old `uiMonoFont` rule.

## Layout: a resizable dock

The old ImGui DockBuilder default is reproduced with `react-resizable-panels` (shadcn's `resizable`): Hierarchy plus a tabbed Inspector/Environment/Stats column on the left, Assets along the bottom, [Viewport](../viewport-panel/) in the center. The split ratios mirror the DockBuilder splits (left 0.20, bottom 0.28, the left column split 0.45/0.55). The C++ editor floated Render Stats as its own window; tabbing it next to Inspector and Environment is the accepted parity choice and keeps every panel in a non-viewport region.

> [!NOTE]
> Every panel, handle, and Radix portal lives outside the viewport rect on purpose. The reparented native window always paints over its rect, so the dock arrangement is also the occlusion strategy — see [the native bridge](../tauri-editor-and-x11-bridge/) page.

## In the code

| What | File | Symbols |
|---|---|---|
| Palette → shadcn tokens | `editor/src/styles.css` | `:root` / `.dark` vars, `@theme inline` |
| Bundled fonts | `editor/src/styles.css` | `@font-face` Roboto / Roboto Mono, `--font-sans` / `--font-mono` |
| The dock layout | `editor/src/app/Layout.tsx` | `Layout`, `LeftBottomTabs`, the panel split sizes |
| Layout-settled bus | `editor/src/app/layoutBus.ts` | `emitLayoutSettled`, `onLayoutSettled` |

## Related

- [Tauri editor and the X11 bridge](../tauri-editor-and-x11-bridge/) — the shell the theme dresses + the occlusion rule
- [Viewport panel](../viewport-panel/) — the panel that fills the dock center
- [Inspector](../inspector/) — uses the mono font for its data fields
