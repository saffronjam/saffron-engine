+++
title = 'Shadows & culling'
weight = 10
+++

# Shadows & culling

How occlusion and per-light visibility are computed. Shadow-casting lights render depth (or distance) that the mesh fragment compares against, and clustered culling narrows the per-fragment light loop to nearby lights.

## Pages

| Page | Covers | Code |
|---|---|---|
| `directional-shadows` | orthographic light view, 2D depth map, 3×3 PCF | `renderer_detail.cppm`; `mesh.slang` · `pcfShadow` |
| `spot-light-shadows` | perspective light view, one shadowed spot, same PCF path | `renderer.cppm`; `mesh.slang` · `pcfShadow` |
| `point-light-cube-shadows` | 6-face cube of distance-to-light, distance comparison | `point_shadow.slang`; `mesh.slang` · `pointShadow` |
| `pcf-filtering` | comparison sampler, 3×3 kernel, off-map and beyond-far handling | `mesh.slang` · `pcfShadow` |
| `shadow-bias` | constant + slope bias, acne vs. peter-panning | `renderer_drawlist.cpp`; `mesh.slang` · `pointShadow` bias |
| `clustered-light-culling` | the froxel grid, exponential Z, sphere-vs-AABB cull dispatch | `light_cull.slang` · `computeMain` |
| `froxel-bounds` | screen-tile bounds → view-space AABB per froxel | `light_cull.slang` |
