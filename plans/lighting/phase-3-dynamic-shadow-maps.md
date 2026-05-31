# Phase 3: Dynamic Shadow Maps

**Status:** IN PROGRESS
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

<!--
DONE 2026-05-31 (directional shadow, the load-bearing slice — validation-clean under a
headless weston Wayland display in the toolbox):
- A single persistent D32 sampled shadow map (2048², Targets::shadowMap), depth-compare
  (PCF) sampler (Descriptors::shadowSampler, eLessOrEqual + ClampToBorder white border).
- A depth-only, depth-biased shadow PSO (makeShadowPipeline, dynamic eDepthBias; reuses
  mesh.slang vertexMain), recorded by recordShadowDepth with the light's viewProj.
- A graph "shadow" pass added in beginFrameGraph before the scene pass; the scene pass
  declares SampledRead on the map so the graph derives DepthWrite -> ShaderReadOnly +
  the cross-frame WAR. The map is pre-transitioned to ShaderReadOnly at init so its
  descriptor is valid on frames the pass is skipped (shader gates the sample on counts.y).
- renderScene fits an ortho light frustum to the scene world AABB (bounding-sphere,
  rotation-stable; GLM_FORCE_DEPTH_ZERO_TO_ONE so glm::ortho is Vulkan [0,1]).
- LightUbo gained counts.y (shadow-enabled flag) + shadowViewProj (set 1, binding 4 is
  the shadow map). mesh.slang: Sampler2DShadow + 3x3 PCF directionalShadow(), multiplied
  into the directional BRDF term only.
- se set-shadows {0|1}; render-stats reports shadows. Verified: caster cube casts a clear
  shadow onto a ground slab (shadows on vs off differ; changed region darker by mean ~47
  luminance, 84% of changed pixels darker).

REMAINING (later increments, then flip to COMPLETED):
- Spot-light shadows (atlas tile + per-light shadowViewProj/atlasRect in GpuLight).
- Directional CSM cascades (3-4 frustum-fit slices, texel snap, seam blend).
- Point cube / 6-tile omnidirectional shadows (reuse phase-2 cubemap Image).
- Depth-bias tuning per light; enabling back-face cull for the main pass.
-->


## Goal

Add real dynamic occlusion for the already-dynamic, entity-based lights via classic
shadow maps — the only shadow approach that runs on llvmpipe today (a depth render +
a compare-sampler, no ray tracing). The engine has **zero** shadows (`grep shadow`
across all engine source + shaders = 0 hits); every light, including the sun, is
unoccluded. Shadows are the single biggest perceptual jump for a scene that has none.
The atlas path scales to dozens of dynamic shadowed lights and leaves a clean seam to
swap in ray-traced visibility (phase 7) later.

**Depends on:** phase 1 (so the shadowed-light contribution is PBR-meaningful). Reuses
the depth-prepass PSO pattern, `SceneDrawList`, the `GpuLight` SSBO, and the
render-graph auto-barriers. Point cube shadows reuse phase 2's cubemap `Image`.

## Current state (verified)

- The clustered punctual lights (`GpuLight`, `renderer_types.cppm:681`) carry no
  shadow data; the fragment loop (`mesh.slang:166-183`) multiplies in no visibility.
- There is a vertex-only depth pre-pass already: `recordDepthPrepass`
  (`renderer_types.cppm:676`), `makeDepthPrepassPipeline` (referenced
  `renderer.cppm:1398`), driven as a graph pass in `beginFrameGraph`
  (`renderer.cppm:538-551`). **This is the template to clone for shadow passes.**
- New graph passes are authored engine-internal in `beginFrameGraph`
  (`renderer.cppm:467`) where the private `SceneDrawList` is visible — shadow passes
  must live here, not in a layer.
- `recordSceneDrawList` (`renderer.cppm:1220`) replays batches with a push-constant
  `viewProj` — a shadow pass replays the same geometry with the **light's** viewProj.

## New resources

- A `D32_SFLOAT` shadow-atlas `Image` (start 4096²), imported into the graph each
  frame so the `DepthAttachment → ShaderReadOnly` transition auto-derives via
  `applyAccess` (`render_graph.cppm:195`).
- A **comparison sampler** (`compareEnable`, `compareOp = eLessOrEqual`) for PCF — add
  alongside `descriptors.linearSampler` (`renderer_types.cppm:500`).
- A depth-only **shadow PSO** cloned from `makeDepthPrepassPipeline`: add
  `depthBiasEnable` + `vkCmdSetDepthBias` dynamic state (tune per light to kill acne)
  and front-face culling. One PSO serves all casters.

## Shader plumbing

Extend `GpuLight` (`renderer_types.cppm:681`) with:

```cpp
glm::mat4 shadowViewProj;       // light-space transform (0 = no shadow)
glm::vec4 atlasRectUvScaleBias; // xy = uv scale, zw = uv bias into the atlas tile
// (pack a "casts shadow" flag into an existing .w lane to avoid growing the struct further)
```

In `mesh.slang`'s per-cluster light loop (`:172-175`) and the directional block,
project `worldPos` by `shadowViewProj`, remap into the atlas tile via
`atlasRectUvScaleBias`, PCF-compare against the atlas, and multiply that light's
radiance by the visibility factor.

## Passes (engine-internal, before the scene pass)

Mirror the cull/depth-prepass blocks (`renderer.cppm:519-551`). Per shadowed light, a
depth-only graph pass:

- `kind = Graphics`, depth attachment = the atlas (or a tile of it), `loadOp = Clear`
  for the first tile.
- viewport/scissor set to that light's atlas tile (the graph sets a full-target
  viewport in `executeRenderGraph:353`; shadow passes need a tile sub-rect, so either
  set viewport/scissor inside the pass body after `beginRendering`, or render each
  tile as its own pass).
- body replays `recordDepthPrepass`-style draws with the light's `viewProj` as the
  push constant.

A per-frame CPU step packs tiles into the atlas by screen-space importance and writes
each light's `shadowViewProj` + `atlasRect` into the `GpuLight` SSBO. Optionally a
dirty flag skips re-rendering an unmoved light's tile (DOOM-style static caching).

## Per-type sequence (ship incrementally)

1. **Spot** — one tile, `perspective(2 × outerAngle)` look-at down the spot dir.
   Implement first: it validates the whole atlas + graph + shader pipeline in one
   pass.
2. **Atlas of N punctual** — size tiles by screen importance; pack point + spot.
3. **Directional CSM** — 3–4 cascades fit to view-frustum slices, texel-snap to kill
   swimming, blend bands at cascade seams. Note: the graph subresource range is fixed
   single-layer (`render_graph.cppm:218`), so a cascade **array** needs the
   `applyAccess` mip/layer extension — or use unrolled **atlas tiles** to avoid the
   graph change (lower friction first cut).
4. **Point cube / 6-tile** — reuse phase 2's cubemap `Image`; render 6 faces or 6
   atlas tiles; sample by direction with a linearized-distance compare.

## Control command

- `se set-shadows {off|spot|atlas|csm}` + `se shadow-stats` (shadowed light count,
  atlas occupancy). Setter in `renderer.cppm`, `registerCommand` in `control.cppm`
  (template `set-clustered`, `control.cppm:231`).

## Done when

- [ ] Spot light casts a correct PCF shadow into an atlas tile, sampled in the lit
      fragment, toggleable via `se set-shadows`.
- [ ] N punctual lights share the atlas; directional CSM shadows the scene without
      obvious swimming/peter-panning; point lights cast omnidirectional shadows.
- [ ] depth bias tuned (no acne, minimal peter-panning); validation-clean; PNG
      verified; runs on llvmpipe.

## Notes / risks

- CSM pitfalls: cascade-seam discontinuities (blend), shadow swimming (snap light
  texels to world grid), peter-panning (bias trade-off, front-face cull helps).
- Enabling back-face culling for the main pass (`renderer.cppm:825`) reduces acne but
  verify winding first.
- This map-based path is the llvmpipe-friendly default; phase 7 can later replace the
  per-light visibility lookup with a ray-query trace behind a feature check, keeping
  the same `GpuLight` shadow flag as the toggle.
- Raising the per-cluster cap (`MAX_LIGHTS_PER_CLUSTER = 64`, kept in sync across
  `mesh.slang:11`, `light_cull.slang:9`, and the C++ constant) may be needed before
  many shadowed lights consume cluster slots.
