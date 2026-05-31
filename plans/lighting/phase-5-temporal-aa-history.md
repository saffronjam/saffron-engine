# Phase 5: Temporal AA + History + Motion Vectors

**Status:** NOT STARTED
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Add the temporal substrate every modern denoiser leans on: ping-pong history color,
per-pixel motion vectors, and a TAA resolve pass. Directly it gives cleaner edges and
less aliasing under motion than FXAA; as an *enabler* it unblocks cheap temporal
denoising for SSGI/SSR (phase 4), DDGI (phase 6), and ReSTIR (phase 8). Small but
load-bearing.

**Depends on:** phase 4's thin G-buffer (shares the MRT widening). Can be done
alongside phase 4 since SSGI needs it. Independent of phases 6–8 but required by them.

## Current state (verified)

- There is exactly **one** offscreen color `Image` (`targets.offscreen`,
  `renderer_types.cppm:563`), recreated (not ping-ponged) on resize; nothing persists
  previous-frame color.
- There are **no motion vectors** and **no stored previous view-proj** — `renderScene`
  builds `viewProjection` fresh each frame (`assets.cppm:416`) and discards it.
- The offscreen carries its layout across frames via `importImage(..., &offscreen.layout)`
  (`renderer.cppm:486`) — the cross-frame `externalLayout` mechanism
  (`render_graph.cppm:247-265`) is the pattern history images use.
- AA today is MSAA or FXAA, mutually exclusive (`setAa`, `renderer.cppm:1378`);
  TAA becomes a third mode.

## Steps

1. **History color** — two persistent renderer-owned `Image`s (ping-pong), imported
   each frame like the offscreen, layout carried via `externalLayout`. Add to
   `Targets` (`renderer_types.cppm:561`).
2. **Motion vectors** — a third thin-G-buffer MRT output (`eR16G16Sfloat`): per-pixel
   `prevClip.xy/prevClip.w − curClip.xy/curClip.w`. Requires storing `prevViewProj` on
   the `Renderer` (and per-object previous model matrices for moving entities — start
   with camera-only motion, add per-instance prev-model later). Uses the phase 4 MRT
   widening.
3. **TAA resolve compute pass** — reproject history through the motion vector,
   neighborhood-clamp (min/max of the 3×3 current neighborhood) to reject ghosting,
   exponential accumulation (~0.9 history weight). Add to the graph before tonemap,
   shaped like the FXAA pass (`renderer.cppm:590-606`). New `taa.slang`.
4. **Control** — extend `se set-aa` to accept `taa` (`aaMode`, `renderer.cppm:1409`),
   or add `se set-taa {0|1}`.

## Done when

- [ ] history images ping-pong correctly across frames; motion vectors written via MRT.
- [ ] TAA resolve produces a stable, anti-aliased image; ghosting controlled by
      neighborhood clamp; selectable vs MSAA/FXAA.
- [ ] history + motion exposed as inputs the SSGI/DDGI denoisers consume.
- [ ] validation-clean; runs on llvmpipe; PNGs verified.

## Notes / risks

- **Editor workflow conflict**: temporal ghosting under fast fly-cam motion may smear
  the gizmo/picking workflow. Make TAA toggleable and default the editor to the MSAA
  path; let TAA be the "GI/denoise" mode. This is an explicit open question from the
  investigation.
- Disocclusion (newly revealed pixels have no valid history) needs a fallback to the
  current frame — handle via the neighborhood clamp + a validity check on the motion
  vector.
- Per-instance previous-model tracking (for moving meshes) can be deferred; camera
  motion covers the common editor case first.
