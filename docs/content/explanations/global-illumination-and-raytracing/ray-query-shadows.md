+++
title = 'Ray-query shadows'
weight = 8
+++

# Ray-query shadows

When ray tracing is on, the mesh fragment shadows lights by tracing one inline ray-query shadow ray
per light against the [TLAS](../raytracing-foundation/) — no shadow maps, no separate pass.
`RayQuery` runs entirely inside the fragment shader, so there's no ray-tracing pipeline and no
shader binding table.

> [!NOTE]
> This path is feature-gated and runs at ~1 FPS on the software dev GPU. It's correctness-validated
> and waits on real ray-tracing hardware.

## One inline ray, in the fragment

`rayQueryShadow` in `mesh.slang` traces a ray from the shaded point toward the light and returns 1
(lit) or 0 (occluded). It uses inline `RayQuery`: the shader sets up a `RayDesc`, calls
`TraceRayInline`, `Proceed()`, and reads the committed status.

```hlsl
ray.Origin = worldPos + toLight * 0.02;  // bias off the surface to avoid self-hit
ray.TMax   = maxDist;
RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
q.TraceRayInline(rtScene, /* same flags */, 0xFF, ray);
q.Proceed();
return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
```

`ACCEPT_FIRST_HIT_AND_END_SEARCH` is the shadow-ray optimization: any hit shadows the point, so
traversal stops at the first triangle instead of finding the closest. The `0.02` origin bias along
the light direction pushes the ray off the surface so it doesn't immediately hit the triangle it
started on.

## Where it plugs into shading

A single runtime flag (`globals.pointShadowMeta.z`) switches the whole engine between shadow maps
and ray-query shadows. When it's set, the directional light traces one long ray toward the sun
(`maxDist = 1e4`) instead of a PCF lookup, and every punctual light traces one ray toward that
light inside the `punctual` function. The result feeds the same `shadow` scalar the BRDF
multiplies.

## Why this beats the map paths

The shadow-map paths shadow exactly one spot light and one point light (the cube map) plus the
directional; the rest of the punctual lights are unshadowed. Ray-query shadows every punctual light
with one ray each, no per-light shadow-map budget, and the ray distance is the actual light
distance, so it can't report an occluder past the light. It's the correctness baseline the map paths
approximate, A/B-exact in spirit: same `shadow` scalar, same BRDF, just sourced from a ray instead
of a depth comparison.

## No pipeline, no SBT

Inline ray-query means traversal happens in the existing graphics pipeline. There's no
`VkRayTracingPipeline`, no shader binding table, no hit/miss shaders — just the `RayQuery` object
and the TLAS bound in set 6. The only RT-specific frame work is building the TLAS
([the foundation](../raytracing-foundation/)); the shadow itself is a few instructions in the
fragment shader.

## In the code

| What | File | Symbols |
|---|---|---|
| The inline shadow ray | `mesh.slang` | `rayQueryShadow` |
| TLAS binding (set 6) | `mesh.slang` | `rtScene` |
| Directional / punctual switch | `mesh.slang` | `fragmentMain`, `punctual` (the `pointShadowMeta.z` branches) |
| The runtime toggle | `renderer.cppm` | `setRtShadows`, `rtShadowsEnabled` |
| TLAS supply | `renderer.cppm` | `buildTlas`, `tlasReady` |

> [!WARNING]
> The mesh PSO declares `rtScene` and `rayQueryShadow` unconditionally, so the compiled SPIR-V
> carries the `RayQueryKHR` capability even on a non-RT GPU. The binding is never *accessed* there
> (the runtime flag stays off), but the capability is in the module — a driver that rejects the
> capability outright would fail to create the PSO.

## Related

- [Acceleration structures](../raytracing-foundation/) — the TLAS this traces against
- [RT device gating](../raytracing-device-gating/) — why the flag exists and when it's safe to set
- [Directional shadows](../../shadows-and-culling/directional-shadows/) — the shadow-map path this replaces
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — what the `shadow` scalar multiplies
