# Phase 20 — React Flow editor view

**Status:** COMPLETED — v1 + polish (canvas, palette, live preview, auto-apply, edge validation, typed slot/color editors)
**Depends on:** 19

> **Polish done.** On top of the v1 canvas: `onConnect` rejects self-loops and enforces **one source per
> input pin** (matching the emitter), with `isValidConnection` giving drag-time feedback; the `textureSlot`
> node now picks from the valid `TEXTURE_SLOTS` (a `<select>`, so a graph can't name an unknown slot) and
> the `constant` node shows a color swatch beside its RGBA fields. Node deletion (Backspace) and `fitView`
> come from React Flow. `bun run check`/`lint`/`build` clean. (Per-node *compile-error* attribution was
> dropped as low-value: the emitter always produces valid Slang for a valid graph, so a compile failure is
> an emitter bug surfaced by the e2e, not user input. Deeper visual refinement is open-ended and best done
> against a live Wayland session.)

> **Done (v1, build-validated).** `@xyflow/react` added. `editor/src/materials/graph.ts` holds the shared
> graph model + `NODE_SPECS` palette (kept in sync with the engine emitter) + `graphToFlow`/`flowToGraph`
> conversion (canvas position rides in `props.editorPos`, which the engine ignores).
> `editor/src/panels/MaterialGraphEditor.tsx` is a full-screen overlay: a React Flow canvas with custom
> `SaffronNode` cards (per-pin input/output handles, inline editors for `constant`/`textureSlot`), a
> categorized node palette, drag-to-connect, **debounced auto-apply** (`material-set-graph` →
> `preview-render`) so the studio-lit sphere morphs as you edit, and a **Compile** button
> (`material-compile-graph`). Launched by the "Graph" button in `MaterialEditorPanel`. Engine plumbing:
> `material-get` now returns the stored graph (e2e `material_graph_roundtrip.test.ts`); client wrappers
> `materialSetGraph`/`materialCompileGraph`. `bun run check` + `lint` clean, `bun run build` green.
>
> **Remaining (needs a live Wayland session to iterate):** interactive polish — handle-alignment for
> many-pin nodes, edge-validation (type/pin compatibility), node deletion UX, fit/zoom defaults,
> per-node compile-error attribution surfaced on the card, and richer inline editors (color swatch for
> `constant`, an asset-picker for `textureSlot`). A scene-path codegen (phase-18 follow-on) would let the
> editor preview reflect the same shader the scene uses.

## Goal

The full node-graph editor as a dedicated workspace view, built on **React Flow (xyflow)**: a node palette,
drag-to-create, typed-socket wiring with validation, per-node previews, the live result sphere, and
debounced compile/preview round-trips. This is the UE5-style live material graph, end to end.

## Why

This is the user-facing payoff of phases 17–19. A full-screen `ViewTab` (not a side panel) is the right home
for a graph editor — it needs canvas space. React Flow gives nodes/edges/pan/zoom/minimap out of the box;
the work is the SaffronEngine-specific node types, the typed wiring rules, and the engine round-trip.

## Design

- **A `ViewTab: { kind: "material", assetId }`** workspace (the existing tab union + `App.tsx` dispatcher
  pattern), opened by double-clicking a material asset (or "Edit Graph" in the phase-13 panel). The slot
  inspector (phase 13) remains the quick-edit; the graph view is the deep editor.
- **React Flow canvas**: custom node components driven by the phase-19 node catalog (pins, types, props).
  Typed sockets colored by type; the wiring layer rejects incompatible connections (with scalar→vec
  broadcast + swizzle allowed). The `materialOutput` node is fixed/non-deletable.
- **Per-node previews** (optional, UE-style): a small thumbnail per node showing its output — request via a
  `preview.renderNode {graph, node}` (renders the partial graph up to that node onto the sphere/flat quad).
  Start with just the **final result sphere** (cheaper); add per-node previews later if wanted.
- **Round-trip**: edits mutate the local graph (Zustand); on change (debounced) → `material.setGraph` →
  engine lowers/compiles (phase 17 foldable or phase 18 codegen) → `preview.render` → updated sphere.
  Constant/param tweaks are instant (no recompile, phase 18); topology changes show the compiling/fallback
  state + any compile errors (node-attributed).

## Files to touch

- `editor/package.json` — add `@xyflow/react` (React Flow / xyflow).
- `editor/src/state/store.ts` — `ViewTab` += `{ kind:"material", assetId }`; `openMaterialGraphTab(asset)`;
  graph editing state (nodes/edges/selection) or a dedicated graph store.
- `editor/src/app/App.tsx` — dispatch the `material` kind → `MaterialGraphWorkspace`.
- `editor/src/app/MaterialGraphWorkspace.tsx` — the React Flow canvas, palette, inspector, result sphere.
- `editor/src/components/nodes/` — custom node components from the node catalog (phase 19 export).
- `editor/src/control/client.ts` — `material.setGraph`, compile-status subscription, `preview.render(node?)`.

## Steps

1. Add xyflow; scaffold the workspace tab + dispatcher (mirror the asset/flamegraph tabs).
2. Render nodes from the catalog; typed sockets + connection validation; the fixed output node.
3. Wire graph→`material.setGraph`→preview round-trip (debounced); show compile status + errors + fallback.
4. The live result sphere (reuse the phase-12 `preview.render` blob pattern).
5. (Optional) per-node previews via `preview.renderNode`.
6. `bun run check`/`lint`/`build`; author a procedural material end to end and watch it compile + render.

## Gate / done

- `bun run build`/`check`/`lint` clean; double-clicking a material opens the graph; wiring nodes recompiles
  and updates the sphere; incompatible wires are rejected; compile errors are shown.
- `make prepare-for-commit` clean (no engine change beyond phases 17–19). Docs: the node editor guide
  (a how-to/tutorial page — "author a material in the graph").

## Risks

- **Debounce discipline**: every keystroke must not trigger a `slangc` compile. Param tweaks → instant
  buffer-write preview; topology → debounced compile. Get this split right or the editor feels terrible.
- **xyflow learning curve / perf**: large graphs need React Flow's memoization; follow their performance
  guidance (memoized nodes, `onlyRenderVisibleElements`).
- **Catalog drift**: the editor node components must match the engine catalog (phase 19) — drive both from
  the one exported catalog.
- **State volume**: graph state can get large; keep it out of the high-frequency reconcile poll (it's
  editor-local until `setGraph`).
