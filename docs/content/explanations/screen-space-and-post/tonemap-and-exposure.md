+++
title = 'Tonemapping'
weight = 5
math = true
+++

# Tonemapping

The scene is rendered in linear HDR: the [BRDF](../../lighting-and-brdf/cook-torrance-brdf/) accumulates
radiance into an `rgba16f` offscreen with no ceiling, so a bright light can push a pixel well past 1.0.
A display can't show that. Tonemapping maps the unbounded linear signal down to a displayable image,
and in SaffronEngine it always runs on the offscreen before ImGui samples it. It's a single compute
pass, one invocation per pixel, that applies exposure, tonemaps, gamma-corrects, and writes the result
back in place.

## What each pixel does

```hlsl
float3 hdr    = color.rgb * push.exposure;     // 1. exposure
float3 mapped = hdr / (hdr + 1.0);             // 2. Reinhard
mapped = pow(mapped, float3(1.0 / 2.2));       // 3. gamma 2.2
```

**Exposure** is a single linear multiplier in the push constant. It is $2^{EV}$ — the UI works in EV
stops and the engine raises 2 to that stop, so +1 EV doubles brightness. This is the one knob that
decides how much of the HDR range lands in the visible band.

**Reinhard** is the tone curve, applied per channel:

$$
c_\text{mapped} = \frac{c}{c + 1}
$$

It maps $[0, \infty)$ into $[0, 1)$, compressing highlights smoothly so nothing clips hard to white.
It's the simplest operator that does the job, and the pass is a clean place to swap in ACES later.

**Gamma** raises the result to $1/2.2$ to encode it for an sRGB-ish display, since everything upstream
lived in linear light. Alpha is passed through untouched.

## Why a compute pass, in place

The offscreen is an `rgba16f` storage image. The tonemap shader binds it as an `RWTexture2D` (set 0,
binding 0), reads each texel, and writes the mapped value back to the same texel — no second target. An
8×8 thread group covers the image, with a bounds check so edge groups don't write past the extent.

Doing it in place means the [render graph](../../frame-and-render-graph/render-graph-overview/) moves
the offscreen through two layouts around the pass, both derived from the declared usage: **Color →
General** before (a storage image is written in `GENERAL`, declared `StorageImageRWCompute`), then
**General → ShaderReadOnly** after, so ImGui can sample it as the viewport texture. Neither transition
is written by hand. This pass is the template every other [compute post-process](../compute-post-process-pattern/)
follows — FXAA and the demonstrator tonemap layer use the same read-modify-write shape.

## In the code

| What | File | Symbols |
|---|---|---|
| The shader | `tonemap.slang` | `computeMain`, `Push.exposure`, the `target` RWTexture2D |
| HDR offscreen format | `renderer_types.cppm` | the `rgba16f` offscreen color format |
| Layout transitions | `render_graph.cppm` | `RgUsage::StorageImageRWCompute`, `SampledRead` |
| Exposure control | `control_commands_render.cpp` | the exposure setter (EV stops → $2^{EV}$) |

## Related

- [HDR and exposure](../../lighting-and-brdf/hdr-and-exposure/) — where the linear radiance comes from
- [Compute post-process](../compute-post-process-pattern/) — the shared RMW shape
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — how the layout moves are derived
