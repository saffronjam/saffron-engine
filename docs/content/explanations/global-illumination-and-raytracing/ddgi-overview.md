+++
title = 'DDGI overview'
weight = 1
+++

# DDGI overview

Dynamic Diffuse Global Illumination gives the engine a multi-bounce indirect term that tracks
moving geometry. [Image-based lighting](../../image-based-lighting/ibl-overview/) only gives a
static ambient, and [screen-space GI](../../screen-space-and-post/) can only bounce what's on
screen. DDGI re-traces a grid of irradiance probes every frame against a coarse voxel copy of
the scene; the mesh fragment then samples the nearest probes for diffuse indirect light. The
trace is software, so DDGI runs on any GPU including the llvmpipe dev device.

## The per-frame pipeline

DDGI is an all-compute prelude before the scene pass. Five passes rebuild the probe state each
frame, then the mesh fragment reads it. The grid is fixed at 8×4×8 probes (`DdgiProbesX/Y/Z`),
each tracing 64 rays (`DdgiRaysPerProbe`).

```mermaid
flowchart LR
    V[ddgi-voxelize<br/>scene → 32³ voxels] --> T[ddgi-trace<br/>64 rays/probe]
    T --> BI[ddgi-blend-irr<br/>irradiance atlas]
    T --> BD[ddgi-blend-dist<br/>moment atlas]
    BI --> BO[ddgi-border<br/>octahedral gutter]
    BO --> M[mesh fragment<br/>ddgiSampleIrradiance]
    BD --> M
```

1. **Voxelize** rasterizes each draw's world-space AABB into a 32³ `rgba16f` 3D image — the
   geometry the rays march against ([voxel proxy](../voxel-scene-proxy/)).
2. **Trace** marches 64 Fibonacci-sphere rays from each probe through the voxels, returning
   radiance and hit distance per ray ([software trace](../software-ray-trace/)).
3. **Blend irradiance** and **blend distance** integrate those rays into two octahedral atlases
   — directional irradiance and Chebyshev distance moments — with temporal hysteresis.
4. **Border** copies each probe tile's octahedral gutter so bilinear sampling wraps correctly.
   These three live in [the atlases](../irradiance-and-moment-atlases/).

The mesh fragment then calls `ddgiSampleIrradiance(worldPos, n)`, which blends the eight probes
around the surface using trilinear, backface, and Chebyshev weights
([probe sampling](../probe-volume-and-sampling/)) and adds the result to the diffuse ambient
term (gated on `screenFlags.z`).

## Why probes over screen-space

Screen-space GI is bounded by the framebuffer: light from off-screen or back-facing geometry
contributes nothing, and the term flickers as the camera turns. DDGI stores irradiance in world
space, so a surface lit by a wall behind the camera stays lit. The cost is a fixed per-frame
budget (five compute passes regardless of view) and coarse spatial resolution — one probe every
couple of meters. That's why DDGI is the *diffuse* indirect term only; specular still comes from
IBL and screen-space reflections.

## Multi-bounce, almost for free

The trace samples last frame's irradiance atlas at each ray hit and folds it back in. A ray that
hits a lit wall picks up that wall's bounce, which was itself fed by the bounce before it. Each
frame adds one bounce, and the temporal blend converges to many bounces over a fraction of a
second with no extra rays. That feedback is why the volume is re-traced rather than baked.

## A self-fitting volume

The probe cage isn't authored. `setDdgiScene` runs from `renderScene` each frame with the
scene's world AABB, and the volume fits to it (padded slightly so probes sit just outside the
geometry). Move or resize the scene and the cage follows, which keeps DDGI dynamic.

## In the code

| What | File | Symbols |
|---|---|---|
| Five-pass per-frame pipeline | `renderer.cppm` | the `doDdgi` block (`ddgi-voxelize` … `ddgi-border`) |
| Probe / grid constants | `renderer_detail.cppm` | `DdgiProbesX/Y/Z`, `DdgiRaysPerProbe`, `DdgiVoxelRes` |
| Volume fit + box upload | `renderer.cppm` | `setDdgiScene`; `assets.cppm` · `renderScene` |
| Sampling into shading | `mesh.slang` | `ddgiSampleIrradiance`, the `screenFlags.z` branch |
| State + toggle | `renderer_types.cppm` | `Ddgi`; `renderer.cppm` · `setDdgi`, `ddgiEnabled` |

> [!NOTE]
> DDGI adds five compute passes every frame whether or not the scene changed, so it's off by
> default (`Ddgi::useDdgi`). Enabling it (or resizing) sets `historyReset`, which zeroes the
> temporal blend for one frame so the probes re-converge instead of ghosting in from stale data.

## Related

- [Voxel proxy](../voxel-scene-proxy/) — the geometry the rays march against
- [Software ray trace](../software-ray-trace/) — how each probe gathers radiance
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — the shading the irradiance feeds
- [Image-based lighting](../../image-based-lighting/ibl-overview/) — the static-ambient term DDGI augments
