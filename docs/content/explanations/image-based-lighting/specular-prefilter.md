+++
title = 'Specular prefilter'
weight = 5
math = true
+++

# Specular prefilter

The specular prefilter is a cubemap whose mip chain stores the environment pre-blurred by the GGX lobe at increasing roughness. Mip 0 holds the sharp environment, and each coarser mip is blurred by a higher roughness. A mirror reflects the environment sharply; a rough surface reflects a smeared average, and the prefilter precomputes that smear so the shader can read it back with a single sample.

This is the first term of the [split-sum approximation](../ibl-overview/). The second term is the [BRDF LUT](../brdf-lut/).

## What one mip computes

For a fixed roughness, the prefiltered value in a direction is the environment convolved with the GGX distribution, importance-sampled:

$$
\text{prefiltered}(r) \approx \frac{\sum_k L_i(l_k)\,(n\cdot l_k)}{\sum_k (n\cdot l_k)}
$$

The split-sum makes one simplifying assumption: view equals normal equals reflection. The bake has no single view vector $v$, so the prefilter assumes the view aligns with the normal (`float3 v = n;` in the shader). This is the trade that makes the result a function of direction and roughness alone. The cost is that grazing reflections lose their stretched, anisotropic shape, which is invisible on most surfaces.

## GGX importance sampling

Uniform sampling of the environment would spend nearly all samples on directions the GGX lobe barely weights. Instead the prefilter draws half-vectors $h$ from the GGX distribution itself, so samples concentrate where the lobe carries energy. Each sample is a low-discrepancy [Hammersley](../brdf-lut/) pair turned into a half-vector and reflected to a light direction:

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

then rotates it into the normal's tangent frame. Samples below the horizon ($n\cdot l \le 0$) are discarded. The rest accumulate weighted by $n\cdot l$, and the sum is normalized by total weight. When every sample misses, the fallback samples straight along $n$.

## One dispatch per mip

Roughness is a push constant, set per mip by the [bake](../ibl-bake-pass/), not a value the shader holds internally. The renderer dispatches the shader once per mip level, binding that mip's storage view and pushing the matching roughness:

```cpp
for (u32 m = 0; m < preMips; ++m) {
    f32 roughness = preMips > 1 ? float(m) / float(preMips - 1) : 0.0f;
    cmd.pushConstants(/* ... */, &roughness);
    cmd.dispatch(group(IblPrefilterSize >> m), group(IblPrefilterSize >> m), 6);
}
```

Mip 0 bakes at roughness 0 over the full `128²`. Each coarser mip halves resolution and raises roughness, up to mip 4 at roughness 1. The lower resolution at higher roughness costs no visible quality, since a blurrier reflection carries no detail to lose.

## How the mesh shader reads it

The fragment samples the prefiltered cube along the reflection vector, choosing the mip from roughness, then applies the BRDF LUT scale and bias. Trilinear filtering between mips blends a roughness that falls between two baked levels smoothly.

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
