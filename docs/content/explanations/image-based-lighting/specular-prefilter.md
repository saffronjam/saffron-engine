+++
title = 'Specular prefilter'
weight = 5
math = true
+++

# Specular prefilter

The specular half of IBL needs the environment blurred by the GGX lobe of a given roughness. A mirror reflects the environment sharply; a rough surface reflects a smeared average. The prefilter precomputes that smear into the mip chain of a cubemap — mip 0 is the sharp environment, each coarser mip is blurred by a higher roughness. This is the first term of the [split-sum approximation](../ibl-overview/).

## What one mip computes

For a fixed roughness, the prefiltered value in a direction is the environment convolved with the GGX distribution, importance-sampled:

$$
\text{prefiltered}(r) \approx \frac{\sum_k L_i(l_k)\,(n\cdot l_k)}{\sum_k (n\cdot l_k)}
$$

The split-sum makes one simplifying assumption: view equals normal equals reflection. There is no single $v$ at bake time, so the prefilter can't know the real view — this is the trade that lets it be a function of direction and roughness alone (`float3 v = n;` in the shader). The cost is that grazing reflections lose their stretched, anisotropic shape. For most surfaces it's invisible.

## GGX importance sampling

Sampling the environment uniformly would waste nearly all samples on directions the GGX lobe barely weights. Instead the prefilter draws half-vectors $h$ from the GGX distribution itself, so samples concentrate where the lobe has energy. Each sample is a low-discrepancy [Hammersley](../brdf-lut/) pair turned into a half-vector, reflected to a light direction:

```hlsl
float2 xi = hammersley(i, 64);
float3 h  = importanceSampleGGX(xi, n, push.roughness);
float3 l  = normalize(2.0 * dot(v, h) * h - v);   // reflect v about h
if (dot(n, l) > 0.0) { prefiltered += envCube.SampleLevel(l, 0.0).rgb * ndotl; totalWeight += ndotl; }
```

`importanceSampleGGX` maps the uniform pair $\xi$ onto a half-vector whose polar angle follows the GGX cumulative distribution:

$$
\cos\theta_h = \sqrt{\frac{1 - \xi_y}{1 + (\alpha^2 - 1)\,\xi_y}}, \qquad \alpha = r^2
$$

then rotates it into the normal's tangent frame. Samples below the horizon ($n\cdot l \le 0$) are discarded; the rest accumulate weighted by $n\cdot l$, and the sum is normalized by total weight. The degenerate fallback (every sample missed) samples straight along $n$.

## One dispatch per mip

The prefilter doesn't know roughness internally — it's a push constant, set per mip by the [bake](../ibl-bake-pass/). The renderer dispatches the shader once per mip level, binding that mip's storage view and pushing the matching roughness:

```cpp
for (u32 m = 0; m < preMips; ++m) {
    f32 roughness = preMips > 1 ? float(m) / float(preMips - 1) : 0.0f;
    cmd.pushConstants(/* ... */, &roughness);
    cmd.dispatch(group(IblPrefilterSize >> m), group(IblPrefilterSize >> m), 6);
}
```

Mip 0 bakes at roughness 0 over the full `128²`; each coarser mip halves resolution and raises roughness, up to mip 4 at roughness 1. Lower resolution at higher roughness is free quality — a blurrier reflection doesn't need the detail.

## How the mesh shader reads it

The fragment samples the prefiltered cube along the reflection vector, choosing the mip from roughness, then applies the BRDF LUT scale/bias. Trilinear filtering between mips means a roughness between two baked levels blends smoothly.

```hlsl
float3 prefiltered = prefilteredMap.SampleLevel(reflect(-v, n), roughness * IblPrefilterMaxMip).rgb;
float2 ab          = brdfLut.SampleLevel(float2(ndotv, roughness), 0.0).rg;
float3 specularIBL = prefiltered * (F0 * ab.x + ab.y);
```

## In the code

| What | File | Symbols |
|---|---|---|
| Prefilter + GGX sampling | `ibl_prefilter.slang` | `computeMain` (`view = normal = reflection`), `importanceSampleGGX`, `hammersley`, `radicalInverseVdC` |
| Per-mip dispatch | `renderer_detail.cppm` | `bakeEnvironment` — prefilter mip loop, `roughness` push constant |
| Mip count / size | `renderer_detail.cppm` | `IblPrefilterMips` (5), `IblPrefilterSize` (128) |
| Consumed as specular | `mesh.slang` | `fragmentMain` — `prefiltered * (F0*ab.x + ab.y)` |

## Related

- [BRDF LUT](../brdf-lut/) — the second split-sum factor, sharing the same Hammersley/GGX helpers
- [Cubemaps and mips](../cubemaps-and-mips/) — the mip chain this fills and the `MaxMip` coupling
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — the GGX lobe being prefiltered
