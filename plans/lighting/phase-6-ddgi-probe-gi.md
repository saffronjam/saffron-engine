# Phase 6: DDGI Probe GI (Software SDF/Voxel Trace)

**Status:** NOT STARTED
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Real multi-bounce, fully-dynamic global illumination with **no baking and no
ray-tracing hardware required** — the headline "Lumen-without-RT" demonstrator.
Dynamic Diffuse Global Illumination (DDGI, Majercik et al. 2019): irradiance probe
volumes updated by tracing a few rays per probe per frame, sampled cheaply (~O(1)) in
the mesh fragment. On llvmpipe the trace runs as a **software** compute ray-march
against a voxel/SDF scene proxy — the same signed-distance-field idea UE5 Lumen-
software and Godot SDFGI use — with a `VK_KHR_ray_query` trace variant gated behind a
device-feature check for when hardware lands (phase 7).

Chosen over voxel cone tracing (VXGI): DDGI is temporally stable, has near-zero
shading-time cost, reuses the phase 2 IBL irradiance interface, and its only genuinely
new primitive is the 3D-image scene proxy + probe atlas.

**Depends on:** phase 1 (BRDF), phase 2 (IBL interface to feed + cubemap/3D `Image`
work), phase 5 (temporal history). The SDF/voxel proxy it builds is reusable as the
phase 7 HW-RT fallback and for future SDF reflections.

## Current state (verified)

- The only "GI" is the flat scalar ambient (phase 2 upgrades it to IBL; this phase
  upgrades it to true multi-bounce indirect).
- The `Image` wrapper (`renderer_types.cppm:114`) is 2D — **3D storage images are net
  new** (the one genuinely new resource type; phase 2 adds cubemap/array, this adds 3D).
- The compute-pass + auto-barrier machinery (`light_cull` → scene,
  `renderer.cppm:519-558`) is the exact shape every DDGI update pass takes.

## Storage

Per-volume, as 2D atlases with 1-texel gutters (octahedral probe encoding):

- Irradiance atlas: 8×8 per probe, `eR11G11B10Float`.
- Distance/moment atlas: 16×16 per probe, `eRG16Sfloat` (mean `r` and `r²` for the
  Chebyshev visibility test that kills light leaking).
- A probe-data image (offsets/state for relocation + classification).
- A **scene proxy** for the trace: a coarse global SDF or a voxelized scene in a 3D
  storage `Image` (needs the new 3D `Image` variant).

## Passes (engine-internal graph compute passes, before the scene pass)

Same dependency shape as `light_cull` → scene. Five passes:

1. **Voxelize / build SDF** — a compute voxelizer rasterizes the `SceneDrawList`
   geometry into the 3D proxy each frame (or incrementally). Conservative
   rasterization is likely unavailable on llvmpipe → dilate triangles in a geometry
   shader, or splat into the voxel grid in compute.
2. **Trace** — the only hardware-sensitive pass. Per probe, trace N rays:
   - **Software (llvmpipe default):** compute ray-march against the SDF/voxel proxy;
     shade hits from last-frame probe irradiance (free multi-bounce) + direct light.
   - **Hardware (phase 7 seam):** `RayQuery` against the TLAS, behind a
     `rayQuery`-feature check.
3. **Blend irradiance** — temporally accumulate ray results into the irradiance atlas.
4. **Blend distance** — accumulate `r`/`r²` into the moment atlas.
5. **Border copy** (+ optional **relocate/classify**) — fix octahedral gutters; move
   probes out of geometry; cull probes in solid/empty space.

## Sampling (the cheap, hardware-agnostic win)

~40–60 lines in the mesh fragment, fed through the **phase 2 IBL irradiance helper**:

```hlsl
// find the 8-probe cage around worldPos
// for each: weight = trilinear * max(0, dot(n, dirToProbe)) (soft backface)
//                  * chebyshev(distAtlas, distToProbe)^3     (visibility / leak kill)
// sum weighted irradiance, normalize, * albedo  -> add into the indirect term
```

Multi-bounce is automatic (probe updates shade hits with the previous frame's probes).

## Control command

- `se set-gi {off|ddgi}`, `se visualize-probes {0|1}`, `se ddgi-stats` (probe count,
  active probes, rays/frame). Setter + `registerCommand` per the `set-clustered`
  template (`control.cppm:231`).

## Done when

- [ ] 3D storage `Image` variant exists; voxel/SDF scene proxy builds each frame.
- [ ] one DDGI volume updates (trace → blend → border) as graph compute passes;
      software trace works on llvmpipe.
- [ ] fragment samples the probe cage with Chebyshev visibility; indirect color bleed
      visible (e.g. a red wall tints a nearby white floor), temporally stable, no
      obvious leaking through thin walls.
- [ ] `se set-gi ddgi` toggles vs IBL-only; probe visualization works; PNGs verified.

## Notes / risks

- **Leaking** is the classic DDGI failure: needs the Chebyshev moment test + a normal
  bias + probe relocation. Saffron's convention of separate-mesh walls helps.
- **Memory** scales O(grid³). Start with one small coarse volume + classification
  culling; add cascaded volumes later (Godot SDFGI style).
- The software trace is correct-but-slow on llvmpipe; the **sampling** side runs
  interactively now, so the visual result is verifiable via captures even if the update
  is not real-time until hardware.
- Diffuse-only is the v1 scope. Glossy/specular GI would push toward VXGI or RT
  reflections (phase 7/8) — an explicit open question.
