+++
title = 'Cubemaps and mips'
weight = 2
math = true
+++

# Cubemaps and mips

IBL stores the environment, the diffuse irradiance, and the roughness-prefiltered specular as **cubemaps**: six square faces sampled by a 3D direction. Two of them carry mip chains. The catch is that a compute shader can't write a cube view; it writes a 2D-array storage image. So each IBL cube is one image carried with two kinds of view — a cube view for sampling, and per-mip 2D-array views for the bake to write through.

## One image, two view shapes

`newCubeImage` creates a `CUBE_COMPATIBLE` image with 6 array layers, `N` mip levels, and `eR16G16B16A16Sfloat` HDR format, requiring both sampled and storage format features up front. The image's own view is `eCube`, covering all mips and all six layers — this is what the mesh fragment samples with `SamplerCube`. A direction in, a filtered color out, with trilinear mip selection for the prefiltered cube.

The cube view can't be a storage target. Cube sampling is a read-only abstraction; the hardware writes layers, not faces. So the bake builds transient `e2DArray` views, one per mip it writes:

```cpp
v.viewType = vk::ImageViewType::e2DArray;
v.subresourceRange = { vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6 };
```

The compute shaders bind these as `RWTexture2DArray<float4>` and address them by `tid.z` = face index 0..5. Same memory, seen as a cube to sample and as a layered 2D array to write.

## Why mips, and how many

The diffuse irradiance and environment cubes are single-mip — irradiance is already fully blurred by its hemisphere convolution, and the environment is only sampled at level 0. The prefiltered specular cube is the one that needs a chain: each mip holds the environment blurred by an increasing roughness, so a rough surface samples a coarse mip and a mirror samples mip 0.

The chain is `IblPrefilterMips = 5` levels over a `128²` base. Roughness maps linearly onto it — mip $m$ is baked at roughness $m/(\text{mips}-1)$, so mip 0 is roughness 0 (sharp) and mip 4 is roughness 1 (fully rough). The mesh shader picks the mip with `roughness * IblPrefilterMaxMip`, where `IblPrefilterMaxMip = 4.0`.

## Bake sizes

The cubes are kept small on purpose. IBL is a low-frequency signal, so a tiny irradiance cube is indistinguishable from a large one, and the one-time bake on a software rasterizer stays quick.

| Texture | Size | Mips |
|---|---|---|
| Environment | `128²` × 6 | 1 |
| Irradiance | `32²` × 6 | 1 |
| Prefiltered | `128²` × 6 | 5 |
| BRDF LUT | `256²` (2D) | 1 |

The BRDF LUT is the odd one out — a flat 2D image, not a cube, since it doesn't depend on direction. It comes from the ordinary `newColorImage` path with storage usage.

## In the code

| What | File | Symbols |
|---|---|---|
| Cube image + cube view | `renderer_detail.cppm` | `newCubeImage` |
| Per-mip storage views | `renderer_detail.cppm` | `makeStorageView` (in `bakeEnvironment`) |
| Sizes and mip count | `renderer_detail.cppm` | `IblEnvSize`, `IblIrradianceSize`, `IblPrefilterSize`, `IblPrefilterMips`, `IblLutSize` |
| Mip ↔ roughness constant | `mesh.slang` | `IblPrefilterMaxMip` |

> [!NOTE]
> The `4.0` in `mesh.slang` and `IblPrefilterMips = 5` in `renderer_detail.cppm` are coupled by hand (`MaxMip == Mips - 1`). There is no compile-time check across the shader/C++ boundary, so changing the mip count means editing both. A comment in each file flags the pairing.

## Related

- [Specular prefilter](../specular-prefilter/) — what fills the mip chain
- [IBL bake pass](../ibl-bake-pass/) — where the transient views are created and freed
- [Lighting and BRDF](../../lighting-and-brdf/) — the other cube-image user (point shadows)
