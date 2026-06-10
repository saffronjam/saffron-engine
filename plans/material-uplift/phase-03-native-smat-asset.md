# Phase 03 — Native `.smat` material asset

**Status:** NOT STARTED
**Depends on:** 02

## Goal

Make a material a first-class catalog asset: add `AssetType::Material`, define the `.smat` JSON
format, add load/save through the `AssetServer`, register `.smat` files in the catalog, and provide
a built-in **default material** (reserved `Uuid`) used as the universal fallback. No entity wiring
yet (that is phase 09) — this phase is the format + the asset-server plumbing.

## Why

Everything downstream (assignment, importer, editor, instances, graph) references a material *asset*.
Today materials only exist inline in components. The `.smat` is reference-only (decision 3): it bakes
nothing, references textures by `Uuid`, and trusts the colorspace/normal metadata recorded on each
texture's `AssetEntry`.

## The `.smat` format (JSON, under `assets.root`, e.g. `materials/<uuid>.smat`)

```jsonc
{
  "version": 1,
  "shader": "mesh",            // übershader family (only "mesh" today)
  "blend": "opaque",           // opaque | masked | translucent  (PSO axis; masked/opaque only until phase 06)
  "unlit": false,
  "doubleSided": false,
  "features": [],              // resolved feature tokens (populated from phase 05): normalMap, occlusion, height, ...
  "factors": {
    "baseColor": [1,1,1,1], "metallic": 0.0, "roughness": 1.0,
    "emissive": [0,0,0], "emissiveStrength": 1.0,
    "normalStrength": 1.0, "alphaCutoff": 0.5, "heightScale": 0.05,
    "uvTiling": [1,1], "uvOffset": [0,0]
  },
  "textures": {                // each value: { "asset": "<decimal-uuid>", optional "channels": {...}, "convention": "gl" }
    "albedo": { "asset": "0" },
    "ormOrMr": { "asset": "0" },
    "normal": { "asset": "0", "convention": "gl" },
    "emissive": { "asset": "0" },
    "height": { "asset": "0" }
  }
}
```

Texture refs are decimal-string `Uuid`s (matches the scene serde convention). `"0"` = none.
The in-memory representation is a `MaterialAsset` struct that lowers to `MaterialParamsData` +
`Material{shader,unlit}` at resolve time (phase 09 uses it; this phase just round-trips the file).

## Files to touch

- `engine/source/saffron/scene/scene.cppm` — add `Material` to `enum class AssetType`.
- `engine/source/saffron/assets/assets.cppm` — update `assetTypeName`/`assetTypeFromName` ("material");
  add a `MaterialAsset` struct + `materialAssetToJson`/`materialAssetFromJson`; add
  `loadMaterialAsset(assets, renderer, id) -> Ref<...>`/`saveMaterialAsset(assets, id, MaterialAsset)`
  following the `loadMeshAsset`/`registerTextureBytes` patterns; add a `materialRefByUuid` cache to
  `AssetServer`; clear it in `clearAssetCaches`.
- `engine/source/saffron/assets/` — a `defaultMaterial()` (reserved `Uuid`, e.g. value `1`): white
  albedo, roughness 1, metallic 0, no textures. Resolve returns it when a referenced material is missing.

## Steps

1. Add the enum value + name conversions (forward-compat: unknown → `Other` already).
2. Define `MaterialAsset` (mirror of the `.smat` JSON) + the to/from-JSON pair.
3. Add `loadMaterialAsset`/`saveMaterialAsset` + the cache map; `.smat` lives at `materials/<uuid>.smat`.
4. Register the built-in default material (reserved id, synthesized in-memory; never written to disk).
5. Add an `se` command stub later in phase 10; for now a unit/e2e that writes a `.smat`, reloads it,
   and asserts equality is enough verification.

## Gate / done

- `make engine` clean; a round-trip test (`materialAssetToJson` → `FromJson`) is equal.
- `make prepare-for-commit` clean.
- Docs: add a "Materials" concept stub under `docs/content/explanations/` + a hub row (the format).

## Risks

- **Reserved-Uuid collision** — `newUuid()` must never mint the default-material id; reserve a small
  low range (the engine already treats `0` as "none"; use `1` for default and exclude it from minting).
- Keep the format **reference-only** — resist baking texture bytes or resolved indices into the `.smat`;
  indices are assigned at load by the bindless allocator and are not stable across runs.
