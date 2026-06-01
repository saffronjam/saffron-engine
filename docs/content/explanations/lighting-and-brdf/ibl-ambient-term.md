+++
title = 'IBL ambient term'
weight = 9
math = true
+++

# IBL ambient term

Direct lights only account for light arriving straight from a source. Everything bounced off
the environment — the sky, nearby surfaces — is the indirect (ambient) term. The fragment
shader adds it on top of the direct sum, either as image-based lighting (the split-sum
approximation) or, when IBL is off, as a flat scalar fill.

## Two ambient paths

The shader branches on `globals.counts.z` (the IBL-enabled flag). When IBL is ready it
evaluates the split-sum; otherwise it falls back to a constant:

```hlsl
if (globals.counts.z != 0)
    ambient = /* split-sum IBL */;
else
    ambient = albedo * (1.0 - metallic) * globals.directionAmbient.w;  // flat fallback
```

The fallback is the [directional light's](../directional-light/) `ambient` scalar
(`directionAmbient.w`) times the diffuse albedo: a cheap constant fill so unlit surfaces are
not pure black. No directionality, no specular.

## Split-sum IBL

The IBL term splits ambient into diffuse and specular, each precomputed into a cubemap or LUT
so the fragment only does a few texture fetches. The diffuse half samples a convolved
irradiance cube $E(n)$ in the normal direction:

$$
\text{diffuse}_\text{IBL} = k_d \cdot \text{albedo} \cdot E(n)
$$

with $k_d = (1 - F)(1 - \text{metallic})$ — the same energy-conservation factor the direct BRDF
uses, here with a roughness-aware Fresnel. The specular half is the split-sum approximation: a
prefiltered environment cube sampled in the reflection direction at a mip chosen by roughness,
scaled by a two-term BRDF integration LUT:

$$
\text{specular}_\text{IBL} = \text{prefiltered}(R,\ \text{roughness}) \cdot (F_0 \cdot \text{LUT}.x + \text{LUT}.y)
$$

```hlsl
float3 F          = fresnelSchlickRoughness(ndotv, F0, roughness);
float3 kd         = (1.0 - F) * (1.0 - metallic);
float3 diffuseIBL = kd * irradianceMap.SampleLevel(n, 0.0).rgb * albedo;
float3 R          = reflect(-v, n);
float3 prefiltered = prefilteredMap.SampleLevel(R, roughness * IblPrefilterMaxMip).rgb;
float2 ab          = brdfLut.SampleLevel(float2(ndotv, roughness), 0.0).rg;
ambient = diffuseIBL + prefiltered * (F0 * ab.x + ab.y);
```

Roughness selects the prefilter mip: a mirror reads the sharp top mip, a rough surface reads a
blurred low one. The maps themselves (irradiance cube, prefiltered cube, BRDF LUT) are bound on
set 3; how they are convolved is the [image-based lighting](../../image-based-lighting/) topic.

## Modulation: AO, SSGI, DDGI

Whichever path produced `ambient`, several screen- and world-space effects refine it. Each is
gated by its own flag and touches only the indirect term, never the direct lights:

- **AO** (`counts.w`) multiplies the ambient by a screen-space occlusion factor, darkening
  creases the IBL cube cannot see.
- **SSGI** (`screenFlags.y`) adds one-bounce screen-space indirect onto the diffuse albedo.
- **DDGI** (`screenFlags.z`) adds world-space multi-bounce irradiance from a probe cage.

The final color is `lo + ambient + emissive`: direct plus indirect plus the surface's own
emission, all in linear HDR.

## In the code

| What | File | Symbols |
|---|---|---|
| Ambient branch | `mesh.slang` | `fragmentMain` — `globals.counts.z` |
| Split-sum specular | `mesh.slang` | `prefilteredMap`, `brdfLut`, `IblPrefilterMaxMip` |
| Roughness Fresnel | `mesh.slang` | `fresnelSchlickRoughness` |
| Flat fallback | `mesh.slang` | `directionAmbient.w` |
| Indirect modulation | `mesh.slang` | `aoMap`, `ssgiMap`, `ddgiSampleIrradiance` |

> [!TIP]
> AO, SSGI, and DDGI only modulate the ambient term — they never touch the direct lights.
> Darkening direct lighting with AO is a common mistake that double-counts shadowing the direct
> shadow maps already handle.

## Related

- [Image-based lighting](../../image-based-lighting/) — how the irradiance/prefilter/LUT maps are built
- [Cook-Torrance BRDF](../cook-torrance-brdf/) — the direct term the ambient is added to
- [Directional light](../directional-light/) — the `ambient` scalar used as the fallback
- [HDR & exposure](../hdr-and-exposure/) — where this linear ambient ends up
