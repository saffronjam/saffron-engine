# Phase 2: Visible Skybox Rendering

**Status:** COMPLETED

<!--
COMPLETED 2026-06-01 (commit 211d0ae), validation-clean. sky.slang: a fullscreen-triangle
(SV_VertexID, no vertex buffer) graphics pass; the fragment reconstructs the world view ray
from the inverse view-projection (far - near, so translation cancels => camera-locked), yaws
it by skyRotation, then shades by mode: 0 Color (flat clearColor*intensity), 1 Texture
(equirect panorama from the bindless array, set 0, indexed by a push-constant slot), 2
Procedural (the baked IBL envCube, set 1 => background matches the lighting). Output is linear
HDR; the existing tonemap pass handles display.
- Renderer: a Sky state struct (mode/clearColor/intensity/rotation/visible/textureIndex +
  pipeline + set + setLayout), SkyRenderSettings + submitSky (renderer_lighting.cpp) + recordSky
  (renderer_drawlist.cpp). The sky descriptor set (set 1 = envCube) is written after the IBL
  bake, reusing the IBL sampler; the panorama is sampled from the existing bindless set 0 (no
  per-frame descriptor writes). makeSkyPipeline: no vertex input, depth test+write off, cull
  none, sample-count aware; built at init and rebuilt with the other PSOs in setAa.
- Graph: a "sky" graphics pass added before the scene pass in beginFrameGraph, writing the SAME
  color target the scene chose (offscreen / msaaColor / scratch) so MSAA resolve + FXAA/TAA
  filtering treat sky and geometry alike; the scene pass switches its color loadOp Clear->Load
  when the sky is present. recordSky pushes inverse(sceneDrawList.viewProj) so the sky aligns
  with the geometry exactly. renderScene resolves Scene.environment into submitSky (loads the
  panorama for Texture mode; falls back to Color if missing).
- Verified headless: procedural sky visible by default (skytop ~[155,157,162]); Color mode
  distinct; visible=false falls back to the clear; all four AA modes (off/fxaa/msaa4/taa) render
  the sky consistently; proc-vs-hidden 44% px changed (the sky pass paints the background); VAL=0.
- Control: driven via se set-environment / the Environment panel (the scene env is the source of
  truth), not a dedicated sky command.
- NOTE: skyRotation is plumbed but does not visibly change the *procedural* sky in a typical view
  because that sky is a near-vertical gradient (yaw-invariant); it rotates a Texture-mode
  panorama. HDR (.hdr) panoramas are LDR-only for now (phase 4). No depth interaction (sky never
  writes/tests depth), so picking + depth prepass are unaffected.
-->


## Goal

Render a visible sky background from `SceneEnvironment` through a dedicated renderer path. This phase should support solid color and texture sky appearance without adding full IBL or atmosphere.

> **Post-lighting note — reuse the existing `envCube`.** Lighting already bakes a procedural
> HDR sky into `envCube` (128² rgba16f, `ShaderReadOnlyOptimal`, view type `eCube`,
> `renderer_detail.cppm` bake at `:3172-3440`). The visible sky's default/`ProceduralAtmosphere`
> mode should **sample that same cube** by world-space view direction — the background then
> matches the IBL lighting exactly, with no extra resource. So the three modes are:
> - **`Color`** → output `clearColor * intensity`.
> - **`ProceduralAtmosphere`** (recommended default) → sample `envCube` by ray direction.
> - **`Texture`** → sample a user LDR equirectangular panorama (sRGB RGBA8 via the existing
>   `loadTextureAsset`/bindless path; no `.hdr` yet — that's phase 4) by lat/long.
>
> Concrete anchors from the recon:
> - **HDR + tonemap are already in place** — `OffscreenColorFormat` is `eR16G16B16A16Sfloat`
>   and a mandatory tonemap compute pass runs last; the sky writes *linear HDR* into the scene
>   color target. Do **not** add a second clear/tonemap.
> - **Scene color target per AA mode** (`renderer.cppm:669-688`): off→`offscreen`;
>   FXAA/TAA→`scratch` (then a compute pass resamples to `offscreen`); MSAA orthogonal→scene
>   renders to `msaaColor` with `RgAttachment.resolve = sceneOutput`. The sky pass must write
>   **the same target the scene pass chose** so it is resolved/filtered identically.
> - **The clear today** is `frame.clearColor = {0.05,0.06,0.08,1}` (`renderer_types.cppm:562`),
>   a static default with no setter. The sky pass takes over the clear: it uses `loadOp=Clear`
>   and the **scene pass switches its color `loadOp` from `eClear` to `eLoad`** (the scene
>   color clear is at `renderer.cppm:1254-1255`; flip it when the sky pass is present).
> - **No fullscreen graphics pass exists yet** without vertex input, but `triangle.slang` is
>   the `SV_VertexID` fullscreen-triangle reference and `makeShadowPipeline`
>   (`renderer_detail.cppm:1317-1411`) / `newMeshPipeline` (`renderer_pipelines.cpp:40-174`)
>   are the PSO templates. Sky PSO: empty vertex input, triangle-list, depth test+write OFF,
>   cull NONE, color = `OffscreenColorFormat`, `rasterizationSamples = targets.sampleCount`
>   (so MSAA works), one combined-image-sampler set + push constants (inverse-viewProj + sky
>   params). Rebuild it on AA sample-count change (the `pipelines.cache.clear()` flow at
>   `renderer.cppm:310-328`).

## Renderer Data

Add renderer-facing state in `renderer_types.cppm`:

```cpp
struct SkyRenderSettings
{
    SkyMode mode = SkyMode::Color;
    glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };
    Ref<GpuTexture> texture;
    f32 intensity = 1.0f;
    f32 rotation = 0.0f;
    f32 exposure = 1.0f;
    bool visible = true;
};
```

Add to `Renderer`:

```cpp
SkyRenderSettings sky;
Ref<Pipeline> skyPipeline;
vk::DescriptorSetLayout skySetLayout;
vk::DescriptorSet skySet;
```

Public API:

```cpp
void submitSky(Renderer& renderer, const SkyRenderSettings& sky);
```

`renderScene` should resolve `Scene.environment` and loaded texture assets into `SkyRenderSettings`, then call `submitSky`.

## Shader Approach

Use a fullscreen triangle rather than a cube mesh for the first implementation.

Benefits:

- No cube mesh dependency.
- No vertex buffer.
- No interaction with mesh draw batching.
- Easy equirectangular lookup from ray direction.
- Camera translation is naturally ignored.

Shader inputs:

- Inverse projection.
- Inverse view rotation or full inverse view with translation ignored.
- Sky rotation.
- Intensity/exposure.
- Optional sampled texture.

For `Texture` mode, sample equirectangular panorama:

```text
direction -> longitude/latitude -> uv -> texture sample
```

For `Color` mode, output the clear/background color times intensity.

## Render Graph Placement

Preferred first implementation:

1. Scene color attachment load op remains clear for color mode.
2. Add `sky` graphics pass after depth prepass and before opaque scene pass, or merge sky draw at the start of scene pass before `recordSceneDrawList`.
3. For texture mode, scene color load op clears, sky pass writes every pixel, then scene pass loads color.

Cleaner render graph version:

- If sky is visible and not pure clear color, make the opaque scene pass use color load `Load` instead of `Clear`.
- Add a `sky` pass:
  - color: same scene color target
  - load: `Clear`
  - store: `Store`
  - depth: none
  - access: sampled sky texture if used
- Then opaque `scene` pass:
  - color load: `Load`
  - depth clear/load behavior unchanged

Depth:

- Sky pass should not write depth.
- Opaque scene pass owns depth clear/load rules.
- Depth prepass remains unaffected.

MSAA/FXAA:

- Sky should render to the same color target that opaque geometry uses.
- With MSAA enabled, sky should render to `msaaColor` and resolve with the scene.
- With FXAA enabled, sky should render into `offscreenScratch` because the whole scene should be filtered consistently.

## Pipeline State

Sky graphics pipeline:

- No vertex input.
- Triangle list.
- Depth test disabled.
- Depth write disabled.
- Cull disabled.
- Color format: `OffscreenColorFormat`.
- Sample count: match `renderer.sampleCount`.
- Descriptor set: sky texture sampler if texture mode uses one.
- Push constants: camera inverse matrices and sky params.

Pipeline cache:

- Keep sky pipeline separate from mesh PSO cache.
- Rebuild when sample count changes, like mesh/depth pipelines.

## Asset Loading

Phase 2 can use existing `loadTextureAsset`, which uploads sRGB RGBA8 textures. That is acceptable for LDR PNG/JPG sky panoramas.

Do not block Phase 2 on HDR support.

## Implementation Steps

1. Add `SkyRenderSettings` and `submitSky`.
2. Resolve environment in `renderScene`.
3. Add `sky.slang`.
4. Add `newSkyPipeline` in renderer detail code.
5. Add `recordSky` helper.
6. Insert sky pass into `beginFrameGraph`.
7. Handle MSAA and FXAA target selection.
8. Recreate sky pipeline when AA sample count changes.

## Verification

- Color mode matches previous clear color output.
- Texture mode displays an imported panorama behind meshes.
- Camera translation does not move the sky.
- Camera rotation changes sky view.
- Mesh picking does not hit sky.
- Depth prepass still works.
- FXAA and MSAA paths still render.
- Captures include sky.

