+++
title = 'Motion vectors'
weight = 5
math = true
+++

# Motion vectors

Temporal anti-aliasing reuses the previous frame's pixels, but the camera moved, so a surface that sat
at one pixel last frame is at a different pixel now. The motion vector says, per pixel, where the same
surface point sat in the previous frame. [TAA](../taa/) follows that vector backward to the right
history sample. The pass computes camera-reprojection velocity into an `rg16f` target.

## How it works

The pass renders the instanced scene depth-tested, the same way the motion-free prepasses do. The push
constant carries the current and previous camera `viewProj` matrices. The vertex stage transforms each
world position by both, handing the fragment stage two clip-space positions. The fragment stage does
the perspective divide on both, turning clip space into NDC, and outputs the difference scaled into UV
space:

$$
\text{motionUv} = \big(\text{ndc}_\text{prev} - \text{ndc}_\text{cur}\big) \cdot 0.5,
\qquad \text{ndc} = \frac{\text{clip}_{xy}}{\text{clip}_w}
$$

The factor of $0.5$ is the NDC→UV scale: NDC spans $[-1, 1]$ over the screen and UV spans $[0, 1]$, so
a delta in NDC is half as large in UV. The result is the offset from this pixel's current UV to where
the surface was last frame, which is exactly what TAA adds to its own UV to find history (`histUv = uv
+ mv`). Both `viewProj` matrices use the same Y-flipped projection the scene renders with, so the Y
sign matches the images TAA samples — no separate flip needed.

## In the code

| What | File | Symbols |
|---|---|---|
| The reprojection | `motion.slang` | `vertexMain`, `fragmentMain`, `curViewProj`/`prevViewProj` |
| Pass declaration | `renderer.cppm` | `motion` pass, `recordMotion`, `motionDepth` |
| The consumer | `taa.slang` | `motion` sampler, `histUv = uv + mv` |

> [!NOTE]
> This version tracks camera motion only. Geometry is assumed static — the same world position is fed
> through both matrices, and only the camera's view-projection differs. A moving object reports the
> wrong velocity because its world position changed too, but the previous-model matrix isn't tracked
> yet. Per-instance previous-model tracking is a noted later addition.

> [!NOTE]
> The motion pass has its own depth attachment (`motionDepth`) and runs before the scene pass, so the
> TAA resolve — which runs after — can sample it. It's a dedicated prepass, not a reuse of the scene
> depth, because the scene's depth target may be multisampled or otherwise shaped by the active AA mode.

## Related

- [TAA](../taa/) — the only consumer of the motion buffer
- [G-buffer](../thin-gbuffer/) — the sibling prepass that records normal + depth
- [ReSTIR](../../global-illumination-and-raytracing/) — temporal reuse that also wants reprojection
