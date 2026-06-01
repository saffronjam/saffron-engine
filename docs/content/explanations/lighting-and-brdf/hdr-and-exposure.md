+++
title = 'HDR & exposure'
weight = 8
math = true
+++

# HDR & exposure

The whole lighting pipeline runs in linear HDR. The [BRDF](../cook-torrance-brdf/) sums
radiance with no ceiling, so a bright light or a tight specular highlight can push a pixel well
past 1.0. That signal lives in a floating-point offscreen until the tonemap pass brings it down
to a displayable range.

## The format that makes it possible

The scene pass writes into an offscreen color target whose format is `rgba16f`
(`R16G16B16A16_SFLOAT`). Half-float gives 16 bits per channel and a floating exponent, so
values above 1.0 survive without clamping. An `rgba8` target would saturate every overbright
pixel to white the moment it was written, throwing away the very range tonemapping exists to
compress. The HDR format defers that decision to the end of the frame.

## Linear all the way down

Every light contribution the fragment shader accumulates is linear radiance:

$$
L_o = \sum_{\text{lights}} \big(f_\text{diffuse} + f_\text{specular}\big)\,L_i\,(n\cdot l) \;+\; \text{ambient} \;+\; \text{emissive}
$$

No gamma, no clamp, no tonemap in `mesh.slang`. Adding lights is plain addition, which is only
physically meaningful in linear space. Encoding (gamma) and range compression (tonemap) are
both deferred to the display step, not the shading step.

## Exposure in EV stops

Before tonemapping, the HDR value is scaled by a single linear exposure multiplier. The editor
and control plane work in EV stops, and the engine raises 2 to that stop:

$$
\text{exposure} = 2^{EV}
$$

So $+1$ EV doubles brightness and $-1$ EV halves it, the photographic convention. The
multiplier is a push constant on the tonemap shader; it is the one knob that decides how much
of the unbounded HDR range lands in the visible $[0, 1]$ band.

## The mandatory tonemap

The tonemap is not optional. A compute pass always runs on the offscreen before ImGui samples
it, reading each HDR texel, applying exposure, a Reinhard curve, and gamma, writing the result
back in place:

```hlsl
float3 hdr    = color.rgb * push.exposure;     // exposure
float3 mapped = hdr / (hdr + 1.0);             // Reinhard
mapped = pow(mapped, float3(1.0 / 2.2));       // gamma 2.2
```

The full operator — why it is a compute pass, how the render graph moves the offscreen through
its layouts — is covered in
[tonemapping and exposure](../../screen-space-and-post/tonemap-and-exposure/). Here the point is
that the lighting side produces unbounded linear radiance and trusts that step to land it on
screen.

## In the code

| What | File | Symbols |
|---|---|---|
| HDR offscreen format | `renderer_types.cppm` | `OffscreenColorFormat` (`eR16G16B16A16Sfloat`) |
| Linear radiance accumulation | `mesh.slang` | `fragmentMain` — `lo + ambient + emissive` |
| Exposure + tone curve | `tonemap.slang` | `computeMain`, `Push.exposure` |
| EV → $2^{EV}$ | `control_commands_render.cpp` | the exposure setter |

> [!TIP]
> Exposure is a plain multiply applied before the Reinhard curve, so it shifts which part of
> the HDR range the curve compresses. Cranking exposure does not clip — Reinhard maps
> $[0, \infty)$ into $[0, 1)$ — it rolls more of the highlight band into the visible portion.

## Related

- [Cook-Torrance BRDF](../cook-torrance-brdf/) — where the linear radiance is produced
- [Tonemapping and exposure](../../screen-space-and-post/tonemap-and-exposure/) — the operator and its render-graph layout moves
- [IBL ambient term](../ibl-ambient-term/) — the other HDR contributor on the offscreen
