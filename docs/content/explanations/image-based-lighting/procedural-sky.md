+++
title = 'Procedural sky'
weight = 3
math = true
+++

# Procedural sky

IBL needs an environment to convolve. Rather than load an HDR panorama, the engine generates one analytically — a horizon-to-zenith gradient over a dim ground plane, plus a bright sun disk. It's written straight into the environment cube as HDR radiance, then the irradiance and prefilter passes treat it as the light source.

## What the sky function returns

`proceduralSky(dir)` takes a world direction and returns linear HDR radiance in three parts.

**The gradient.** Above the horizon ($\text{dir}.y \ge 0$) it blends horizon color into zenith color by a softened upward factor; below, it blends toward a dark ground color. The `pow(up, 0.6)` curve pushes the blend so most of the visible dome reads as sky rather than horizon, and a `* 1.6` lifts the whole thing into a brighter exposure range. The ground side ramps quickly to a near-flat dark, a floor for downward-facing reflections.

**The sun disk.** A fixed sun direction drives two power lobes added on top:

```hlsl
float s = max(dot(normalize(dir), sunDir), 0.0);
col += pow(s, 1200.0) * float3(22.0, 20.0, 17.0);   // tight bright core
col += pow(s, 6.0)    * float3(0.30, 0.26, 0.20);    // soft surrounding glow
```

The $\cos^{1200}$ lobe is an extremely tight, very bright disk — values above 20, genuine HDR that the prefilter smears into bright specular reflections. The $\cos^6$ lobe is a wide, dim halo. The sun direction `normalize(float3(-0.4, 0.7, -0.5))` roughly matches the default directional light, so IBL and the sun agree.

## Writing it into the cube

The compute shader runs one invocation per output texel, with `tid.z` selecting the cube face. It reconstructs the face direction, evaluates the sky, and stores it:

```hlsl
float2 uv  = (float2(tid.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
float3 dir = cubeFaceDir(tid.z, uv);
outCube[tid] = float4(proceduralSky(dir), 1.0);
```

`cubeFaceDir` is the hardware cube convention shared by every IBL shader: a face index 0..5 and an in-face uv in $[-1, 1]$ map to the world direction that face represents. The `+ 0.5` keeps sampling aligned to texel centers. The result is bound as an `rgba16f` `RWTexture2DArray`.

## Why analytic, not a loaded HDR

A procedural sky has no asset dependency, costs nothing to ship, and is deterministic across platforms — the bake produces the same environment every run. It's a stand-in. The pipeline downstream doesn't care where the environment came from, so swapping in a loaded HDR equirect or a real skybox capture is just a matter of changing what fills the environment cube. The skybox plan reserves that seam.

## In the code

| What | File | Symbols |
|---|---|---|
| Sky model + face direction | `ibl_skygen.slang` | `proceduralSky`, `sunDir`, `cubeFaceDir` |
| The write | `ibl_skygen.slang` | `computeMain`, `outCube` |
| Dispatched once | `renderer_detail.cppm` | `bakeEnvironment` — skygen dispatch over `IblEnvSize` |

## Related

- [Diffuse irradiance](../diffuse-irradiance/) — convolves this environment for diffuse
- [Specular prefilter](../specular-prefilter/) — blurs this environment per roughness
- [IBL bake pass](../ibl-bake-pass/) — runs skygen first, then the convolutions
