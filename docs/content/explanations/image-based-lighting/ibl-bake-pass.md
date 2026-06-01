+++
title = 'Baking'
weight = 7
+++

# Baking

The four IBL shaders all run inside one function, `bakeEnvironment`, called once at startup. It's synchronous one-time work — its own command buffer, its own transient descriptors, a `waitIdle` at the end — not part of the [per-frame render graph](../../frame-and-render-graph/render-graph-overview/). The output is the persistent set 3 the mesh shader samples every frame after.

## Why bake once, not per frame

The convolutions are heavy: the irradiance integral is a thousand environment samples per texel, the prefilter and LUT each importance-sample 64 to 512 directions per texel. None of it changes once the environment is fixed. Doing it per frame would burn the whole budget for a result identical to last frame's. Amortized to startup, the runtime cost collapses to three texture fetches in the [ambient block](../ibl-overview/). This mirrors the engine's other one-time GPU jobs — `uploadTexture` and `renderMeshThumbnail` follow the same own-command-buffer-plus-`waitIdle` shape.

## The four stages

The bake runs the shaders in dependency order — the sky must exist before it can be convolved. The BRDF LUT is independent and runs last.

```mermaid
flowchart TD
    A[skygen → environment cube<br/>128² × 6] --> B[irradiance convolution<br/>→ irradiance cube 32²]
    A --> C[prefilter, one dispatch per mip<br/>→ prefiltered cube 128² × 5 mips]
    D[BRDF integration<br/>→ LUT 256²] -.->|independent| E
    B --> E[write persistent set 3]
    C --> E
```

Each stage transitions its target to `eGeneral`, dispatches, then transitions to `eShaderReadOnlyOptimal`. The environment is special: after skygen writes it, it transitions to `eShaderReadOnlyOptimal` so the irradiance and prefilter passes can *sample* it. The barriers are written by hand here (outside the render graph), one `pipelineBarrier2` per transition. The dispatch grid is `(size+7)/8` groups in X and Y to match the shaders' `[numthreads(8,8,1)]`, and 6 in Z, one per cube face.

## Transient resources, freed at the end

The bake builds a private descriptor pool, two set layouts (storage-only for skygen/LUT, sampler+storage for the convolutions), the four compute pipelines, and the per-mip 2D-array storage views described in [cubemaps and mips](../cubemaps-and-mips/). All of it exists only for the bake; after the `waitIdle`, the transient views and layouts are destroyed.

The four images — environment, irradiance, prefiltered, LUT — persist; they're owned by `renderer.ibl`. The environment cube outlives the bake even though only the convolutions read it, kept around as the source if a re-bake is ever wired up.

## Writing the persistent set

The last step writes the three samplers the mesh fragment binds as set 3 — irradiance, prefiltered, BRDF LUT — into `renderer.ibl.set`, then flips `ibl.ready = true`. The set layout and the empty set were allocated earlier in `initDescriptorResources`, so the mesh pipeline layout can reference set 3 before the bake fills it. The shared `ibl.sampler` is a linear/trilinear clamp-to-edge sampler with `maxLod = VK_LOD_CLAMP_NONE` so the prefiltered mip chain filters across all levels.

## The runtime gate

The mesh shader only samples set 3 when `globals.counts.z != 0`, which is `useIbl && ibl.ready` — both the toggle and the bake-completed flag. So IBL contributes the moment the bake finishes, and `se set-ibl 0` flips back to the flat scalar ambient without touching the baked textures.

## In the code

| What | File | Symbols |
|---|---|---|
| The whole bake | `renderer_detail.cppm` | `bakeEnvironment` |
| Called at startup | `renderer.cppm` | after `initDescriptorResources` |
| Persistent set + sampler | `renderer_detail.cppm` | `renderer.ibl.set`, `ibl.sampler`, `ibl.ready` |
| Runtime gate | `renderer_lighting.cpp` | `iblFlag` → `counts.z` |
| Toggle | `renderer.cppm` / `control_commands_render.cpp` | `setIbl`, `iblEnabled`, `set-ibl` |

> [!NOTE]
> The bake submits on the graphics queue and calls `device.waitIdle()` to finish before returning. Fine at startup, but it would stall a running frame — deliberately a one-time init step, not a render-graph pass. A re-bake on a changed environment would need to fold these passes into the graph or schedule them off the main loop.

## Related

- [IBL overview](../ibl-overview/) — what set 3 feeds at runtime
- [Cubemaps and mips](../cubemaps-and-mips/) — the image + transient-view setup the bake uses
- [Procedural sky](../procedural-sky/) — stage one of the bake
- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — the per-frame system this bake sits outside of
