+++
title = 'FXAA'
weight = 2
+++

# FXAA

FXAA is the cheap anti-aliasing option: a single compute pass that finds high-contrast edges
in the finished image and blurs along them. It never touches how the scene is rasterized, so
it costs one screen-sized dispatch and a few texture samples per pixel. It works on edges MSAA
can't see — alpha-tested cutouts, specular highlights, shader aliasing — but it softens fine
detail wherever it fires.

## How it works

When FXAA is on, the scene renders into a 1× scratch image instead of straight into the
offscreen. A compute pass reads the scratch as a sampler and writes the anti-aliased result
into the offscreen as a storage image. It is a read-from-A, write-to-B variant of the
[compute post-process pattern](../../screen-space-and-post/compute-post-process-pattern/), not
an in-place pass like tonemap. The scratch matches the offscreen format and is shared with TAA,
since both consume the scene's pre-AA result.

The graph derives the transitions: the scratch goes to a sampled-in-compute layout, the
offscreen to `GENERAL` for the storage write, then on to shader-read-only for ImGui. None of
that is written by hand.

### What the shader does per pixel

`fxaa.slang` is the FXAA 3 console variant. Each invocation works in luma (perceived
brightness, `dot(c, vec3(0.299, 0.587, 0.114))`):

1. **Sample a cross.** Read the center pixel and its four diagonal neighbours, take each luma,
   and find the local min and max. Their difference is the contrast range.

2. **Skip flat regions.** If the range is below a threshold relative to the brightest sample,
   this isn't an edge — write the original pixel and return. Most of the screen takes this
   branch, which is why FXAA is cheap.

3. **Find the edge direction.** The diagonal luma gradient gives a 2D direction perpendicular
   to the edge. It is normalized and clamped to a maximum span so the blur never reaches more
   than a few texels.

4. **Blend along it.** Sample two pairs of points along the edge direction. The wider 4-tap
   average is preferred, but when it overshoots the local luma range the shader falls back to
   the tighter 2-tap average.

The blur runs only on texels that pass the edge test, and only along the detected edge, which
keeps it from washing out the whole frame.

## In the code

| What | File | Symbols |
|---|---|---|
| Mode switch | `renderer_aa.cpp` | `setAa` · `fxaaEnabled` |
| Scratch target (shared with TAA) | `renderer_detail.cppm` | `recreateFxaaTarget`, `scratch` |
| Descriptor set (scratch → offscreen) | `renderer_detail.cppm` | `updateFxaaSet`, `fxaaSetLayout` |
| Pass + dispatch in the graph | `renderer.cppm` | `beginFrameGraph` · `fxaa` pass |
| The shader | `fxaa.slang` | `computeMain`, `luma`, `EDGE_THRESHOLD_*` |

> [!NOTE]
> FXAA treats luma contrast as "an edge" and can't tell a real geometry edge from a sharp
> texture or a noisy specular highlight, so it softens all three. That is the trade against
> MSAA, which fixes geometry edges exactly but sees nothing inside a triangle.

## Related

- [AA modes](../aa-modes/) — the full mode table and how the three are switched
- [MSAA](../msaa/) — the rasterization-time alternative
- [TAA](../../screen-space-and-post/taa/) — the temporal alternative, sharing the scratch
- [Compute post-process pattern](../../screen-space-and-post/compute-post-process-pattern/) — the shared dispatch shape
