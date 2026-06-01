+++
title = 'Directional light'
weight = 3
math = true
+++

# Directional light

The directional light is the sun: one parallel light with no position and no falloff. It is
the first term the fragment shader accumulates, evaluated through the same
[BRDF](../cook-torrance-brdf/) as every punctual light, then attenuated by its shadow.

## A single direction, no attenuation

With no distance there is no attenuation and no cone — the incoming radiance is just the
light's color times intensity, the same for every fragment. The only per-fragment work is the
BRDF and the shadow. The shader stores the light's travel direction in
`globals.directionAmbient.xyz` and flips it to get the direction toward the light, which is
what the BRDF wants:

```hlsl
float3 lDir = -normalize(globals.directionAmbient.xyz);
float3 lo = brdf(n, v, lDir, albedo, metallic, roughness,
                 globals.colorIntensity.rgb * globals.colorIntensity.a) * shadow;
```

The `radiance` argument is `color * intensity` directly. Compare that with a
[punctual light](../punctual-lights-and-attenuation/), where the same slot also carries
`attenuation * cone`. Same function, fewer factors.

## Shadow: map, contact, or ray

The `shadow` scalar multiplies the whole direct term. The directional light is the one light
with a full shadowing stack:

- **Shadow map.** When `counts.y` is set, `pcfShadow` projects the fragment into the sun's
  light-space `shadowViewProj`, does a 3×3 PCF comparison against a 2048² depth map, and
  returns a `[0, 1]` visibility. Off-map and beyond-far-plane samples count as lit.
- **Contact shadows.** When `screenFlags.x` is set, a screen-space contact-shadow factor
  multiplies on top, adding fine detail the coarse map misses.
- **Ray-traced.** When RT shadows are enabled (`pointShadowMeta.z`), the map path is skipped
  and a long ray-query traces toward the sun instead (`rayQueryShadow`, `1e4` max distance).

```hlsl
float shadow = 1.0;
if (globals.pointShadowMeta.z != 0)
    shadow = rayQueryShadow(input.worldPos, lDir, 1e4);   // RT shadow
else if (globals.counts.y != 0)
    shadow = pcfShadow(shadowMap, globals.shadowViewProj, input.worldPos);
if (globals.screenFlags.x != 0)
    shadow *= contactMap.SampleLevel(screenUv, 0.0).r;    // fine contact detail
```

Contact shadows are directional-only in v1; the map and ray paths are mutually exclusive.

## The ambient companion

The directional component is the only one carrying an `ambient` scalar
(`directionAmbient.w`). When [IBL](../ibl-ambient-term/) is off, that scalar is the flat
indirect fallback, a constant fill so unlit surfaces are not pure black. It is not part of the
direct term above; it is added later with the rest of the ambient.

## In the code

| What | File | Symbols |
|---|---|---|
| Direct term | `mesh.slang` | `fragmentMain` — `lDir`, the `brdf` call |
| Shadow map PCF | `mesh.slang` | `pcfShadow`, `globals.shadowViewProj` |
| Contact + RT shadow | `mesh.slang` | `contactMap`, `rayQueryShadow` |
| Direction + ambient upload | `renderer_lighting.cpp` | `setSceneLighting` — `directionAmbient`, `colorIntensity` |

> [!TIP]
> `globals.directionAmbient.xyz` is the direction the light travels, not the direction toward
> the sun. The shader negates it. A sun pointing straight down is `direction = (0, -1, 0)`.

## Related

- [Cook-Torrance BRDF](../cook-torrance-brdf/) — the shared shading model
- [Light components](../light-components/) — where the direction and intensity come from
- [Punctual lights and attenuation](../punctual-lights-and-attenuation/) — the same BRDF with falloff and a cone
- [IBL ambient term](../ibl-ambient-term/) — what replaces the flat `ambient` scalar
