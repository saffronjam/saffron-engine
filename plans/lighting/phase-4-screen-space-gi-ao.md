# Phase 4: Screen-Space GI / AO (GTAO, SSGI, SSR)

**Status:** IN PROGRESS
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

<!--
DONE 2026-06-01 (the MRT infra + thin G-buffer + GTAO ambient-occlusion slice;
validation-clean under headless weston):
- MRT (the load-bearing infra, commit 22e53fc): RgPass.color (single optional) ->
  RgPass.colors (std::vector<RgAttachment>); executeRenderGraph loops them for barriers +
  the dynamic-rendering color array; per-attachment MSAA resolve kept. All call sites
  (scene, ui) updated. Other phases (5 TAA, SSR) reuse this.
- Thin G-buffer (commit c241947): a prepass renders view-space normal (rgb) + view-Z (.a)
  into one rgba16f target (gbuffer.slang + makeGbufferPipeline), with its own depth scratch.
  Targets gNormal/gDepth/aoMap recreate with the viewport (recreateSsaoTargets) + refresh
  descriptors. NOTE: kept as a separate prepass rather than a 2nd attachment on the main
  scene pass — simpler, and the scene pass stays single-color (MSAA/forward unaffected);
  the MRT widening is still in place for TAA/SSR. The mesh fragment was NOT changed to emit
  SV_Target1 (the prepass owns normals); revisit if a full G-buffer is wanted.
- GTAO (gtao.slang): HBAO-style hemisphere occlusion (4 slices x 6 steps, screen radius
  scaled by view distance, range falloff), output an r8 AO factor. A Compute graph pass
  after the G-buffer prepass. The mesh multiplies AO into the INDIRECT (ambient) term only
  (counts.w gate), never direct. se set-ssao; render-stats reports ssao.
- Verified: cubes on a ground plane, IBL ambient-dominant — AO on vs off darkens contact
  creases (18% px changed, 100% darkening), VAL=0.

REMAINING for COMPLETED:
- Spatial denoise (XeGTAO 5x5) — current AO has mild per-pixel noise (no denoise yet).
- Screen-space contact shadows (Step 3).
- SSGI (Step 4) — needs phase 5 temporal history (do phase 5 first).
- SSR (Step 5, optional) — Hi-Z march + cubemap fallback.
-->


## Goal

Add the cheapest tier of dynamic indirect lighting — ground-truth ambient occlusion
(GTAO), one-bounce screen-space global illumination (SSGI), screen-space contact
shadows, and optionally screen-space reflections (SSR). This is **literally the first
trace stage of UE5 Lumen** ("screen traces first, fall back to a more complete
representation"). It is all pure compute, runs on llvmpipe at low budget, and
establishes the **thin G-buffer (depth + normal)** and temporal-history infra that
DDGI (phase 6) and RT (phase 7) reuse.

**Depends on:** phase 1 (HDR color to gather), phase 2 (AO/SSGI multiply/add into the
IBL ambient term). SSGI's denoiser depends on phase 5 (temporal history) — do GTAO
first (no temporal needed), then pull in phase 5 before SSGI.

## Current state (verified)

- The scene pass writes a single color attachment (`RgPass.color` is one
  `std::optional<RgAttachment>`, `render_graph.cppm:75`); there is **no normal buffer**
  and no G-buffer.
- Post-process compute passes already exist as the pattern to copy: `addTonemapPass`
  (`renderer.cppm:619`) and the FXAA pass (`renderer.cppm:590-606`) — both
  `RgPassKind::Compute`, declaring `StorageImageRWCompute` / `SampledReadCompute`
  against imported images, dispatched at `(w+7)/8, (h+7)/8`.
- The depth target exists (`targets.depth`, `renderer_types.cppm:563`).

## Step 1 — thin G-buffer (the one infra add)

Add a **view-space normal** target as a second color attachment on the scene pass.
This does **not** make the renderer deferred — it keeps MSAA, transparency, and
per-draw material freedom (forward+); it just also stores normals.

- Octahedral-encode into `eR16G16Snorm` (or `eR8G8B8A8`); a renderer-owned persistent
  `Image` (`targets.normal`), recreated with the viewport like the offscreen.
- **Widen `RgPass.color`** from a single `std::optional<RgAttachment>` to a small
  `std::vector<RgAttachment>` and loop it in `executeRenderGraph` (the color
  attachment handling at `render_graph.cppm:293-303` and `:325-340`). Keep `resolve`
  per-attachment for MSAA. This is the cross-cutting MRT change other phases also need.
- The mesh fragment (`mesh.slang:149`) outputs `SV_Target0 = color`, `SV_Target1 =
  octEncode(n)`.

## Step 2 — GTAO (start here; no temporal dependency)

Port XeGTAO (MIT, near-drop-in): a `PrefilterDepths` pass (depth mip pyramid) → a
`MainPass` (3 directional slices × 6 taps, horizon-based) → a `5×5 spatial denoise`.
Output an AO factor (and optional bent normals). Multiply AO into the **phase 2 IBL
diffuse ambient** term (AO darkens indirect, not direct). New compute pipelines +
`gtao.slang`, dispatched like FXAA.

## Step 3 — screen-space contact shadows

A short depth-ray-march per light from the shaded fragment toward the light; a cheap
always-on supplement to phase 3's shadow maps for fine contact detail. Fold into the
mesh fragment or a dedicated compute pass reading depth.

## Step 4 — SSGI (needs phase 5 temporal history)

One cosine-hemisphere ray per pixel marched against the depth buffer; on a hit, gather
the **previous frame's** HDR color as incoming indirect radiance; add it into the
indirect term (the phase 2 helper). Denoise with a bilateral + temporal pass (this is
where phase 5's history image + motion vectors are required). New `ssgi.slang`.

## Step 5 — SSR (optional)

Build a Hi-Z (min-depth) mip pyramid in compute, march reflection rays coarse-to-fine
against it, sample the color buffer on a hit, fall back to the phase 2 prefiltered
cubemap on a miss. New `ssr.slang` + `hiz.slang`.

## Control commands

- `se set-ssao {0|1}`, `se set-ssgi {0|1}`, `se set-contact-shadows {0|1}`,
  `se set-ssr {0|1}` (+ `se gi-stats`). Setters in `renderer.cppm`, `registerCommand`
  in `control.cppm` (template `set-clustered`, `:231`).

## Done when

- [ ] scene pass writes a normal target via the widened MRT path; validation-clean.
- [ ] GTAO grounds objects with visible contact AO, multiplied into IBL ambient,
      toggleable.
- [ ] SSGI adds plausible one-bounce color bleed (after phase 5 lands), temporally
      stable; contact shadows + optional SSR work.
- [ ] all passes pure compute; runs on llvmpipe at reduced resolution; PNGs verified.

## Notes / risks

- Screen-space methods only know on-screen geometry — they leak/darken at screen edges
  and behind occluders. That limitation is *why* phase 6 (world-space DDGI) follows.
- Keep resolution/sample budgets low on llvmpipe; expose a downsample factor.
- The MRT widening (`RgPass.color` → vector) is the load-bearing infra piece; do it
  cleanly since phases 5/6/7 depend on it.
