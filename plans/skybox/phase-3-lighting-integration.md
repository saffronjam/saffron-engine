# Phase 3: Lighting Integration

**Status:** COMPLETED (heavily revised — most of the original scope was already shipped)

<!--
COMPLETED 2026-06-01 (commit 93117eb), validation-clean. Most of the original phase-3 scope
(IBL diffuse/specular, PBR, colored ambient via the env) was already shipped by the lighting
roadmap, and sky<->lighting coherence is already achieved by phase 2 reusing the envCube. The
genuinely-remaining, in-scope work done here: RGB fallback ambient driven by SceneEnvironment.
- Appended ambientColor (vec4) to the light UBO + mesh LightGlobals; the non-IBL fallback now
  uses globals.ambientColor.rgb (directionAmbient.w kept as a legacy luminance). setSceneLighting
  takes a premultiplied glm::vec3 ambient (was a scalar); setDirectionalLight passes grayscale to
  preserve old behavior. renderScene drives it from environment.ambientColor*ambientIntensity when
  useSkyForAmbient, else the directional light's legacy scalar ambient.
- Verified A/B: IBL OFF -> red vs blue environment ambient tints the mesh strongly
  (center [186,86,86] vs [86,86,186]); IBL ON -> ambientColor ignored (red vs blue pixel-identical
  [150,160,176], diff 0.0), so existing IBL behavior is unchanged. VAL=0.

DONE as a follow-up (commit b863151): the procedural skygen sun now follows the scene's
DirectionalLight via an on-demand IBL re-bake. ibl_skygen takes a sun push constant
(direction = -lightDir, color, intensity); bakeEnvironment gained a SkygenParams + firstBake
flag (a re-bake reuses the existing images — waitIdle first, Undefined->General barriers discard
old contents — and skips image creation + descriptor writes); requestSkyBake (from renderScene)
flags a re-bake only when the sun inputs change, and beginFrameGraph consumes it at a GPU-idle
point. The visible sky + IBL share the envCube, so moving the light re-tints both together.
Verified: init bakes once, each light move = exactly one re-bake, steady state never re-bakes,
validation-clean. (skyIntensity stays in the visible-sky pass, not baked, to avoid double-counting.)

STILL deferred to phase 4: routing environment sky color into the off-by-default DDGI skyColor,
and the user-equirect IBL re-bake (a different env *source*, sharing this re-bake machinery).
-->


## Goal

Connect `SceneEnvironment` to mesh lighting. The original phase planned to *build* IBL here;
the lighting roadmap already built it. So this phase shrinks to **wiring**: make the
environment authoring controls actually drive the (already-existing) IBL bake, the procedural
sky, the DDGI sky color, and the non-IBL fallback ambient.

> ## Post-lighting reality — what is ALREADY DONE
>
> The 8-phase `plans/lighting/` roadmap implemented essentially all of this phase's original
> "future" work. Do **not** rebuild any of it:
> - **Full split-sum IBL** — `irradianceCube` + `prefilteredCube` + `brdfLut` baked from
>   `envCube`, bound at mesh **set 3**, driving physically-based diffuse + specular ambient
>   (`mesh.slang:454-466`). This *replaced* scalar ambient as the default; "Step 4: Diffuse
>   IBL" and "Step 5: Specular IBL" below are **DONE**.
> - **PBR Cook-Torrance BRDF** with metallic/roughness materials — done (lighting phase 1).
> - **Colored ambient via the environment** already happens through IBL; the SH-projection
>   path proposed below is unnecessary (a baked irradiance cube already does it).
> - The scalar `directionAmbient.w` term is now only the **fallback** used when IBL is off
>   (`counts.z == 0`), computed as `albedo * (1-metallic) * directionAmbient.w`
>   (`mesh.slang:468-471`). The big LightUbo restructure proposed in "Step 1" is obsolete —
>   the real `LightUbo` has ~14 fields (`renderer_detail.cppm:1026-1041`).
>
> ## Revised remaining work (what this phase SHOULD do now)
>
> 1. **Drive the procedural sky from `SceneEnvironment`.** `ibl_skygen.slang` hardcodes sun
>    direction + zenith/horizon/ground colors (`:21-45`). Promote those to skygen push-constant
>    / UBO params sourced from `SceneEnvironment` (sun from the scene's `DirectionalLight`;
>    tint/intensity from environment). Because the bake runs **once at init**
>    (`renderer.cppm:275-278`), add a **`rebakeEnvironment` on demand** when those inputs change
>    so the visible sky (phase 2, sampling `envCube`) *and* the IBL lighting update together —
>    one slider re-tints background + lighting coherently. Guard re-bake behind a dirty flag
>    (it's a `waitIdle` + a handful of dispatches, fine for editor-time edits, not per-frame).
> 2. **Feed environment sky color into DDGI.** `Ddgi.skyColor`/`sunDir`/`sunColor`/`sunIntensity`
>    (`renderer_types.cppm:814-855`) are hardcoded today and consumed by the DDGI trace; route
>    them through `setSceneEnvironment`/`setDdgiScene` from `SceneEnvironment`.
> 3. **RGB fallback ambient (small, optional).** For the IBL-off path, add a `glm::vec4`
>    ambient-color slot to `LightUbo` and change the fallback line to
>    `albedo*(1-metallic) * ambientColor.rgb * ambientColor.a`, sourced from
>    `environment.ambientColor * ambientIntensity`. Low priority — only matters with IBL off.
> 4. **Wire `useSkyForAmbient`.** When false, skip environment-driven ambient and keep the
>    legacy directional-light ambient fallback.
>
> The detailed steps below are the **original (pre-lighting) plan**, kept for context. Steps
> 4–5 are DONE; steps 1–3 are superseded by "Revised remaining work" above.

## Current Lighting Limitation (original — now historical)

The mesh shader currently uses:

- one directional light,
- scalar ambient,
- punctual light list,
- clustered or non-clustered punctual loop.

The current ambient path is scalar:

```cpp
LightUbo.directionAmbient = glm::vec4(glm::normalize(direction), ambient);
```

Shader side:

```hlsl
float3 lit = directional + globals.directionAmbient.w;
```

This makes colored skylight impossible.

## Step 1: RGB Ambient

Change light globals to support colored ambient.

Suggested GPU layout:

```cpp
struct LightUbo
{
    glm::vec4 direction;        // xyz travel direction
    glm::vec4 colorIntensity;   // rgb color, a intensity
    glm::vec4 ambientIntensity; // rgb ambient color, a ambient intensity
    glm::uvec4 counts;          // x punctual count
};
```

Shader:

```hlsl
float3 ambient = globals.ambientIntensity.rgb * globals.ambientIntensity.a;
float3 lit = directional + ambient;
```

Public API:

```cpp
void setSceneLighting(
    Renderer& renderer,
    glm::vec3 direction,
    glm::vec3 color,
    f32 intensity,
    glm::vec3 ambientColor,
    f32 ambientIntensity,
    const std::vector<GpuLight>& lights);
```

Keep old overloads temporarily if useful.

## Step 2: Environment-Derived Ambient

In `renderScene`:

- Read first `DirectionalLightComponent` as today.
- Resolve `Scene.environment`.
- If `useSkyForAmbient`, use `environment.ambientColor * environment.ambientIntensity`.
- If not, use current light ambient fallback.

Migration:

- Existing `DirectionalLightComponent::ambient` can stay for now as legacy/direct ambient intensity.
- Longer term, move ambient off directional light and into `SceneEnvironment`.

## Step 3: Sky Texture Approximation

Before true irradiance convolution exists:

- Let user set `ambientColor` manually.
- Optionally sample average color CPU-side during import/cache later.

Do not sample the sky texture per fragment as ambient. It is noisy, expensive, and not physically correct.

## Step 4: Diffuse IBL

Later, generate low-order spherical harmonics or a small irradiance cubemap from the sky texture.

Options:

- CPU SH projection from decoded equirectangular pixels.
- GPU convolution into a cubemap.

First practical target:

```cpp
struct SkyIrradiance
{
    glm::vec4 sh[9];
};
```

Shader:

```hlsl
float3 irradiance = sampleSH(globals.skySh, normal);
```

This becomes the diffuse ambient term for lit materials.

## Step 5: Specular IBL

Only after material model has roughness/metalness:

- Convert equirectangular source to cubemap.
- Generate prefiltered mip chain by roughness.
- Generate BRDF LUT.
- Sample reflection vector by roughness in material shader.

## Implementation Steps

1. Expand light UBO to RGB ambient.
2. Update C++ buffer writes.
3. Update `mesh.slang`.
4. Update `DirectionalLightComponent` handling in `renderScene`.
5. Wire `SceneEnvironment` ambient controls.
6. Verify clustered lighting still works.
7. Add tests/screenshots for colored ambient.

## Verification

- Red/blue ambient visibly tints shadowed areas.
- Directional light behavior is otherwise unchanged.
- Point and spot lights still render in clustered and non-clustered modes.
- Existing projects preserve approximately the same look after migration.

