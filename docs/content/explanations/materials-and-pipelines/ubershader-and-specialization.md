+++
title = 'Übershader'
weight = 2
+++

# Übershader

An übershader is a single shader source that covers many material features, with each feature toggled by a compile-time constant rather than a separate file. The engine builds every variant from one mesh shader, `mesh.slang`, that almost every renderable passes through.

The lit and unlit paths compile as two specializations of the same SPIR-V, selected by a Vulkan specialization constant. The pipeline cache stays small: N materials share one PSO, and each extra variant adds one more.

## A constant-gated branch

The übershader declares a boolean specialization constant and branches on it at the top of the fragment stage:

```hlsl
[[vk::constant_id(0)]] const bool kUnlit = false;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float4 tex = albedoTextures[NonUniformResourceIndex(input.textureIndex)].Sample(input.uv0);
    float3 albedo = tex.rgb * input.baseColor.rgb;

    if (kUnlit)
    {
        return float4(albedo + input.emissive, tex.a * input.baseColor.a);
    }
    // ... full Cook-Torrance lighting, IBL, shadows, screen-space terms ...
}
```

`kUnlit` is not a uniform. A specialization constant resolves when the pipeline is created, so the branch folds away at PSO compile time. The lit PSO holds no unlit code, and the unlit PSO holds none of the [BRDF](../../lighting-and-brdf/cook-torrance-brdf/), IBL, or shadow work. There is no per-fragment branch cost and no dynamic-uniform read.

The C++ side supplies the value through `vk::SpecializationInfo` when `newMeshPipeline` builds the fragment stage, with `constantID = 0` matching `[[vk::constant_id(0)]]`. The same shader module produces a lit pipeline when `unlit` is false and an unlit one when true, and each value becomes its own cache entry (`shader` vs `shader|unlit`).

## Why specialization, not a branch or two files

A uniform branch keeps one PSO but pays a runtime branch and forces both code paths to stay live in the binary, raising register pressure and lowering occupancy. Two separate shader files remove the runtime cost but duplicate the shared code — vertex layout, vertex stage, the bindless sample — and become two files to edit in lockstep.

A specialization constant keeps a single source of truth and gives each variant a fully specialized binary with the dead path compiled out. The approach generalizes: a new variant such as vertex-color or alpha-test is one more `vk::constant_id`, one more shader branch, and one more cache key. No new file and no new pipeline-building code.

## In the code

| What | File | Symbols |
|---|---|---|
| Constant + gated branch | `mesh.slang` | `kUnlit`, `fragmentMain` |
| Baking the constant | `renderer_pipelines.cpp` | `newMeshPipeline` — `SpecializationInfo`, `constantID` |
| Variant → cache key | `renderer_pipelines.cpp` | `requestMeshPipeline` (`\|unlit`) |

## Related

- [Materials & PSOs](../material-and-pso-selection/) — how a variant maps to a cache entry
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — the lit path the constant compiles out
- [Bindless textures](../bindless-textures/) — the albedo sample shared by both variants
