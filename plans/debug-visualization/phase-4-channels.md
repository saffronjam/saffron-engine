# Phase 4 — Buffer-channel view modes

**Status:** IN PROGRESS — 4a (in-shader channels) DONE; 4b/4c remaining.

4a shipped: `ViewModeDto`/`ViewMode` grew with `Albedo, Normal, Roughness, Metallic, Emissive`; the
active channel rides the spare `LightGlobals.pointShadowMeta.w` slot (no UBO layout change), packed in
`renderer_lighting.cpp` from `renderer.viewMode`; `lighting.slang` exposes `debugViewChannel()` and
`mesh.slang`'s `fragmentMain` branches to output the surface value before lighting. Panel dropdown +
e2e (`view-mode.test.ts` albedo pixel-diff) + docs done. The channels pass through tonemap (documented).

4b (screen-space: Depth, Motion Vectors, AO) + 4c (Overdraw, Light Complexity) remain: each is a
fullscreen debug-blit `RgPass` reading a producer target (`gNormal`/`motion`/`aoMap`/cluster counts),
gated on that producer being enabled. Grow `ViewModeDto` per channel as it lands.

Grow `ViewModeDto` — no new command. The enum widens (with `enumWireNames` kebab entries
`motion-vectors`, `light-complexity`) and **only ever lists implemented modes**.

## 4a — in-shader channels (S each)
Push-constant `uint debugMode` on `mesh.slang` (extend the push block in lockstep with the C++ struct and
all mesh PSOs); thread `viewMode(renderer)` in `renderer_drawlist.cpp`; in `fragmentMain` branch to output
`s.albedo`/`s.roughness`/`s.metallic`/`s.emissive` instead of `evalLighting`. Tonemap stays. Adds
`Albedo, Roughness, Metallic, Emissive`.

## 4b — screen-space channels (M/L)
`Normal` (`gNormal`), `Depth`, `MotionVectors` (`motion`), `Ao` (`aoMap`) as a fullscreen debug-blit
`RgPass` that declares its `RgUsage` (read source, write `sceneColor`). **Gate availability on the
producing feature being enabled** — `gNormal`/`motion`/`aoMap` exist only when the G-buffer/TAA/SSAO paths
are on; a channel selected while its producer is off must force-enable the producer or be unavailable, and
the e2e must not assume the target exists.

## 4c — accumulation passes (L)
`Overdraw` (additive count) and `LightComplexity` (cluster light counts) as dedicated passes, last.

Codegen/editor/tests/docs per the checklist; the panel `Select` auto-lists the grown enum; e2e adds
per-channel round-trips + opportunistic pixel-diffs. Validation-error count must be zero (the corruption
net for the push-constant work).

## Gate (per sub-phase)
`make engine` + `make prepare-for-commit`; e2e; contract green.
