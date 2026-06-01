+++
title = 'GTAO'
weight = 2
math = true
+++

# GTAO

Ambient light fills the parts of a surface the direct lights miss. Geometry tucked into a crease or
under an overhang gets less of that fill, because nearby surfaces block it. GTAO estimates how blocked
each pixel is and darkens only its indirect term. It's a horizon-style screen-space pass reading the
[thin G-buffer](../thin-gbuffer/) — no extra geometry, just normals and depth.

## How it works

Each pixel reconstructs its view-space position $p$ and normal $n$ from the G-buffer, then samples the
neighborhood for occluders. The pass walks a few azimuth slices around the pixel, takes a few steps
along each, and turns every tap's depth into a view-space position $s$. A tap occludes if it rises
above the pixel's tangent plane and is close enough:

$$
\text{occlusion} \mathrel{+}= \max\!\big(n \cdot \hat{d} - \text{bias},\ 0\big)\;\cdot\;\text{rangeCheck},
\qquad \hat{d} = \frac{s - p}{\lVert s - p\rVert}
$$

The dot product $n \cdot \hat{d}$ is the cosine of the angle between the normal and the direction to
the tap. When it's positive the tap sits inside the hemisphere above the surface and blocks ambient
light from that direction. The `bias` (0.02) ignores nearly coplanar taps, which avoids self-occlusion
on flat ground. The `rangeCheck` fades occlusion out past the configured radius, so a distant wall
can't darken a pixel it shouldn't; only nearby geometry counts. Taps on background (`viewZ > -1e-4`) or
off-screen contribute nothing.

The loop runs four slices of six steps. The screen-space step size comes from the world radius
projected to screen, clamped so near surfaces don't over-sample a huge region and far ones still
register. A small per-pixel rotation, hashed from the pixel coordinates, jitters the slice angles so
the low sample count doesn't band:

$$
\varphi_s = \frac{s + \text{rnd}}{\text{sliceCount}} \cdot 2\pi
$$

The result is averaged, scaled by a strength knob, and stored as a single AO factor in $[0, 1]$ where
1 is fully open:

$$
\text{ao} = \operatorname{saturate}\!\big(1 - \text{occlusion} \cdot \text{strength}\big)
$$

### Denoise pass

Four slices of six steps is noisy, and the per-pixel rotation trades banding for grain. GTAO writes its
raw factor to an intermediate `aoRaw` target; a second compute pass (`ao-blur`) reads `aoRaw` plus the
G-buffer normal and writes the final `aoMap`. It's bilateral: it blurs across the noise but respects
normal discontinuities, so AO doesn't bleed across edges.

### Where the AO lands

GTAO modulates only the indirect term. The mesh fragment shader computes the ambient contribution (flat
fallback or IBL) first, then multiplies by the AO factor. Direct lights are untouched: AO is a coarse
stand-in for the visibility of the ambient hemisphere, and a direct light already has a known direction
and its own shadow term, so applying AO there would double-darken.

## In the code

| What | File | Symbols |
|---|---|---|
| AO pass | `gtao.slang` | `computeMain`, `viewPosFromUv`, `sliceCount`, `stepCount`, `bias` |
| Pass + denoise wiring | `renderer.cppm` | `gtao` pass, `ao-blur` pass, `aoRaw`, `aoMap` |
| Where AO is applied | `mesh.slang` | `aoMap` (set 4), `counts.w` gate |

> [!NOTE]
> The AO factor is `r8` (one 8-bit channel). It's a visibility scalar, not a color, so an `rgba16f`
> target would waste three channels and twice the bits. The output is written as a storage image in
> `GENERAL`, the same in-place compute shape as every other screen-space pass.

## Related

- [G-buffer](../thin-gbuffer/) — the normal + depth it reads
- [Image-based lighting](../../image-based-lighting/) — the indirect term AO darkens
- [Compute post-process](../compute-post-process-pattern/) — the dispatch + RMW shape
