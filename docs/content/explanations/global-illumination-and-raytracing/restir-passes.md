+++
title = 'ReSTIR passes'
weight = 10
math = true
+++

# ReSTIR passes

[ReSTIR](../restir-overview/) is three compute passes over per-pixel reservoir buffers: initial
candidate sampling, spatiotemporal reuse, and resolve-and-shade. This page walks each, the SSBO
layout that wires them together, and the M-clamping that keeps the temporal feedback unbiased.

> [!NOTE]
> ReSTIR is feature-gated on ray-query support and runs at ~1 FPS on the software dev GPU —
> correctness-validated, awaiting hardware.

## The reservoir buffers

Three SSBOs hold one `Reservoir` (32 bytes) per pixel, sized to the offscreen resolution
(`reservoirCapacity`):

- `initial` — this frame's RIS result (written by pass 1, read by pass 2)
- `combined` — after reuse (written by pass 2, read by pass 3)
- `previous` — last frame's combined (the temporal source; pass 3 writes it for next frame)

Consecutive passes serialize through a sentinel buffer access in the
[render graph](../../frame-and-render-graph/render-graph-overview/): pass 1 declares
`StorageWriteCompute` on `combined`, passes 2 and 3 declare `StorageReadCompute`, so the graph
derives the write→read barriers between them. All three dispatch one thread per pixel (8×8 groups).

## Pass 1 — initial (RIS)

`restir_initial.slang` reconstructs the world surface from the G-buffer (view normal + view-Z),
finds the pixel's froxel cluster, and draws $K$ candidate lights from that cluster's light list
(reusing the [clustered](../../lighting-and-brdf/clustered-forward/) candidate set; $K = 16$ by
default, `candidateCount`). Each candidate is weighted by its unshadowed target contribution
$\hat p$ and kept by weighted reservoir sampling.

`targetContribution` is the scalar luminance of the light's diffuse contribution: intensity ×
$n\cdot l$ × distance attenuation × spot cone, no shadow. The pass finishes by computing the
unbiased weight $W = \tfrac{1}{K}\,\text{wSum} / \hat p_\text{chosen}$ and writing the reservoir.
Background pixels (no surface) write an empty reservoir and bail.

## Pass 2 — reuse (temporal + spatial)

`restir_reuse.slang` starts the combined reservoir from this pixel's initial one, then merges in
more samples via `combineInto` — WRS over reservoirs, where each incoming reservoir is reweighted by
*its chosen light's* target function at this pixel and contributes $\hat p \cdot W \cdot M$ to the
running sum.

**Temporal**: the previous frame's reservoir is fetched by reprojecting the pixel through the motion
vector (`uv + mv`) and merged if the reprojected UV is on-screen.

**Spatial**: four random neighbours within a 16-pixel radius are merged, each skipped unless its
depth and normal are similar (`abs(Δdepth) < 0.5` and normal dot > 0.9), so reuse only borrows from
surfaces that share lighting.

The final unbiased weight is recomputed from the merged state:

$$
W = \frac{\text{wSum}}{M \cdot \hat p_\text{chosen}}
$$

## M-clamping

The temporal source is last frame's combined reservoir, which itself absorbed the frame before it.
Left unbounded, $M$ grows without limit and the estimator becomes badly biased — old, stale samples
dominate and lighting lags. So before merging, the history's $M$ is clamped (`maxM = 20`). Clamping
caps how much weight any past frame carries, trading a little variance for a bounded bias and keeping
the lighting responsive to change. It's the standard ReSTIR bias control.

## Pass 3 — resolve and shade

`restir_resolve.slang` reads the combined reservoir and does the work deferred from pass 1: it traces
*one* ray-query shadow ray toward the chosen light (the only visibility ray ReSTIR needs), and if
lit, shades the light's contribution scaled by the reservoir weight $W$:

```hlsl
float vis = rayShadow(worldPos, l, dist);   // single ACCEPT_FIRST_HIT ray
if (vis <= 0.0) { radianceOut[tid.xy] = 0; return; }
float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.a
                * atten * cone * ndotl * res.a.y * vis;   // res.a.y = W
```

The output is geometry × visibility × $W$, *without* the surface albedo — the mesh fragment
multiplies by `albedo / PI` when it samples the radiance, so the material stays with the material.
The pass also copies the combined reservoir into `previous` for next frame's temporal reuse, closing
the loop.

## In the code

| What | File | Symbols |
|---|---|---|
| Candidate sampling + RIS | `restir_initial.slang` | `computeMain`, `targetContribution`, `clusterIndexFor` |
| Reservoir merge | `restir_reuse.slang` | `combineInto`, the temporal + spatial blocks |
| M-clamping | `restir_reuse.slang` | `prevM = min(prev.a.w, maxM)`, `nbM` |
| Resolve ray + shade | `restir_resolve.slang` | `computeMain`, `rayShadow` |
| The three graph passes | `renderer.cppm` | `restir-initial`, `restir-reuse`, `restir-resolve` |
| Reservoir SSBOs | `renderer_types.cppm` | `Restir::initial`, `combined`, `previous` |

> [!WARNING]
> The three passes serialize through a single *sentinel* buffer access (`combined`) rather than
> declaring each reservoir buffer to the graph. That's enough to force the RAW barriers between
> consecutive passes, but the graph doesn't track the individual reservoir buffers — the ping-pong of
> `previous` is managed by hand in the resolve pass, not derived.

## Related

- [ReSTIR](../restir-overview/) — the reservoir + RIS theory these implement
- [Clustered forward](../../lighting-and-brdf/clustered-forward/) — where the candidate light pool comes from
- [Ray-query shadows](../ray-query-shadows/) — the `rayShadow` used in resolve
- [Motion vectors](../../screen-space-and-post/) — the temporal reprojection input for reuse
