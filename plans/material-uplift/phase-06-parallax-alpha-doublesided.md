# Phase 06 — Parallax + alpha-clip + double-sided

**Status:** NOT STARTED
**Depends on:** 05

## Goal

Add the remaining standard surface features: **height-driven parallax occlusion mapping** (the
silhouette-less depth illusion), **alpha-clip / masked** blend (cutout foliage, decals), and a
**`doubleSided`** raster axis. Translucent (alpha-blended) is explicitly **out of scope** — it needs
a sorted, depth-write-off transparency pass the engine does not have; tracked separately.

## Why

Parallax is what makes a flat sphere read as deep rock without tessellation (the height map from a
PBR set). Masked alpha is needed for any cutout material. `doubleSided` is needed for foliage/cloth.
These complete the "what a scanned PBR material needs" set short of true displacement.

## Design

- **Parallax occlusion mapping (POM)** in `evalSurface`, behind `HAS_HEIGHT`: march the view ray
  (`m.viewDir` transformed into tangent space) against the height map (`tex1.x`), offset the UV by the
  found intersection, then do all other samples at the parallaxed UV. `heightScale` (in `pbr`/`emissive.w`)
  controls depth. Keep step count modest (e.g. 8–32 adaptive by view angle) — it is per-fragment cost.
- **Alpha-clip (masked)**: `blend:"masked"` sets a feature/flag; `evalSurface` (or `fragmentMain`)
  does `if (s.opacity < alphaCutoff) discard;`. This needs **no new PSO** (discard is in-shader) — but
  it disables early-Z, so gate it to masked materials only. `alphaCutoff` is already a `pbr` lane.
- **`doubleSided`**: cull mode is currently hardcoded `eNone` (so everything is already two-sided!).
  Make cull a **PSO axis**: extend `Material` with `bool doubleSided`, add it to the
  `requestMeshPipeline` string key (`"|2s"`), and set `cullMode = doubleSided ? eNone : eBack` in
  `newMeshPipeline`. Verify winding (`frontFace` is CCW) so single-sided doesn't cull the wrong faces —
  this is a real correctness step since nothing is culled today.

## Files to touch

- `engine/assets/shaders/mesh.slang` — POM loop in `evalSurface` behind `HAS_HEIGHT`; tangent-space
  view dir (needs TBN from phase 04/05); `discard` for masked; sample height from `tex1.x`.
- `engine/source/saffron/rendering/renderer_types.cppm` — `Material` gains `bool doubleSided` (+ a
  `blend` enum or a `masked` bool); `SubmeshMaterial` gains the height texture + `heightScale`.
- `engine/source/saffron/rendering/renderer_pipelines.cpp` — `doubleSided` in the cache key +
  `cullMode` selection in `newMeshPipeline`; confirm CCW winding for the culled path.
- `engine/source/saffron/rendering/renderer_drawlist.cpp` — pack height index + `heightScale` + the
  masked flag; route `Material.doubleSided` from the resolved `.smat`.
- `.smat` (`blend`, `heightScale`, `doubleSided`) already specified in phase 03 — wire them through resolve.

## Steps

1. Add the height slot + `heightScale` to `SubmeshMaterial`/`MaterialParams`/drawlist (mirrors phase 05).
2. POM in `evalSurface`: tangent-space ray march, parallaxed UV reused by all samples.
3. Masked: `discard` below `alphaCutoff`; gate to masked materials (feature bit).
4. `doubleSided`: cache-key axis + `cullMode`; flip default to `eBack` for single-sided, verify winding.
5. Test: the Poly Haven coast-rocks height map shows depth at grazing angles; a masked foliage texture
   cuts out; a single-sided plane culls its back face.

## Gate / done

- `make engine` clean; parallax depth visible at grazing view; masked cutout works; single-sided culls.
- Non-height/opaque materials unchanged. `make prepare-for-commit` clean. Docs: parallax + blend modes.

## Risks

- **Winding correctness**: enabling back-face culling for the first time will expose any inverted-winding
  meshes. Keep `eNone` as the default-material behaviour and opt single-sided in per `.smat`.
- **POM cost/artifacts**: silhouettes still don't change (POM ≠ displacement); steep angles need more
  steps or show swimming. Cap steps; document it's not true displacement (that's a future tessellation task).
- **Masked + early-Z**: discard disables early-Z; don't apply the masked path to opaque materials.
