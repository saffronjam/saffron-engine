+++
title = 'Descriptor sets'
weight = 6
math = false
+++

# Descriptor sets

The descriptor sets bound by the mesh übershader (`mesh.slang`). Sets 0–4 are core: bindless textures, lighting, instances, IBL, and screen-space maps. Sets 5–7 are the GI/RT extensions. The push constant is the camera `viewProj` (`float4x4`). Each binding declares `[[vk::binding(b, set)]]`.

The tables below list every binding by set, with its Slang type and role.

## Set 0 — bindless albedo
| Binding | Slang type | Note |
|---|---|---|
| 0 | `Sampler2D albedoTextures[1024]` | one global array, indexed per-instance with `NonUniformResourceIndex`; size = `MaxBindlessTextures` |

## Set 1 — lighting
| Binding | Slang type | Note |
|---|---|---|
| 0 | `ConstantBuffer<LightGlobals> globals` | directional + ambient + eye + shadow transforms + feature flags |
| 1 | `StructuredBuffer<GpuLight> lights` | per-frame punctual light list |
| 2 | `StructuredBuffer<Cluster> clusters` | per-froxel light index lists (cap 64) |
| 3 | `ConstantBuffer<ClusterParams> clusterParams` | view, inverse-projection, grid dims, screen size, z planes |
| 4 | `Sampler2DShadow shadowMap` | directional depth map (PCF compare) |
| 5 | `Sampler2DShadow spotShadowMap` | spot depth map (PCF compare) |
| 6 | `SamplerCube pointShadowMap` | omnidirectional distance cube |

`LightGlobals.counts`: x = punctual count, y = directional shadow, z = IBL, w = SSAO. `screenFlags`: x = contact, y = SSGI, z = DDGI, w = ReSTIR.

## Set 2 — instances
| Binding | Slang type | Note |
|---|---|---|
| 0 | `StructuredBuffer<Instance> instances` | per-instance model + normalMatrix + baseColor + texture index + pbr + emissive; indexed by `SV_VulkanInstanceID` |

## Set 3 — IBL
| Binding | Slang type | Note |
|---|---|---|
| 0 | `SamplerCube irradianceMap` | diffuse irradiance |
| 1 | `SamplerCube prefilteredMap` | GGX-prefiltered specular (max mip 4) |
| 2 | `Sampler2D brdfLut` | split-sum (scale, bias) LUT |

Sampled for ambient when `globals.counts.z != 0`.

## Set 4 — screen-space maps
| Binding | Slang type | Gate | Note |
|---|---|---|---|
| 0 | `Sampler2D aoMap` | `counts.w` | AO factor (1 = open); darkens indirect |
| 1 | `Sampler2D contactMap` | `screenFlags.x` | contact-shadow factor (1 = lit); darkens directional direct |
| 2 | `Sampler2D ssgiMap` | `screenFlags.y` | one-bounce GI radiance (rgba16f) |

All sampled by screen UV (`input.position.xy / screenSize`).

## Sets 5–7 — GI / RT extensions
| Set | Binding | Slang type | Gate |
|---|---|---|---|
| 5 | 0 | `Sampler2D ddgiIrradiance` | `screenFlags.z` (DDGI octahedral irradiance atlas) |
| 5 | 1 | `Sampler2D ddgiDistance` | `screenFlags.z` (DDGI moment atlas) |
| 6 | 0 | `RaytracingAccelerationStructure rtScene` | RT device support (set omitted from layout otherwise) |
| 7 | 0 | `Sampler2D restirRadiance` | `screenFlags.w` (replaces the punctual loop) |

## Related
- [Descriptor sets](../../explanations/materials-and-pipelines/descriptor-sets/) — how the sets are laid out and bound
- [Bindless textures](../../explanations/materials-and-pipelines/material-and-pso-selection/) — the set 0 array
- [Clustered forward](../../explanations/lighting-and-brdf/clustered-forward/) — set 1's cluster lists
