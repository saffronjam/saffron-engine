# Phase 06 — Chunk-slice loaders + GPU cache by sub-id

**Status:** COMPLETED
**Depends on:** 05

> Implementation note: the base-offset `loadMesh` is realized as `loadMeshFromBytes` /
> `loadMeshSkinFromBytes` / `loadAnimationFromBytes` (parse a `.smesh`/`.sanim` image from a memory
> span, validating against the span length) — a chunk slice is read into memory via `ByteSource` and
> parsed identically to a whole file. `loadMeshAsset` / `loadTextureAsset` now route an embedded
> sub-asset (`AssetEntry.container != 0`) through `resolveMesh` / `resolveTexture`, so the draw path
> stays unchanged. The GPU-free slice path (open container, slice, parse, `resolveMaterial`, remap
> fallback) is covered by `runChunkLoaderSelfTest`; the GPU upload half of `resolveMesh/Texture` is
> exercised by the phase 07/08 instantiate + render e2e.

## Goal

Implement `loadModelAsset`: open a `.smodel`, resolve a sub-id to its chunk slice, and load the
mesh/texture/animation/material from that slice — reusing the existing negative-cache loaders
(`loadMeshAsset` / `loadTextureAsset`, `assets.cppm`) by teaching them to source bytes from an
`(offset, length)` slice instead of a standalone file. GPU resource caches key on the **sub-id**, not the
container id. Honor the `remap` table (an external file wins over the embedded chunk). Defers: spawning
entities (07), the filesystem scan (09).

## Why

The renderer resolves mesh/texture by uuid every frame (the draw path), backed by `meshRefByUuid` /
`textureRefByUuid` on `AssetServer` (`assets.cppm:54`). For a container to render, those caches must be
fillable from a chunk slice and keyed by the sub-asset's id (so two models can't collide and one sub-asset
loads once). This is the read-back counterpart to phase 05's bake and the prerequisite for instantiation.

## Slice-sourced loading

Add an internal byte-source abstraction so the existing loaders don't care whether bytes come from a file
or a chunk:

```cpp
struct ByteSource {                      // a whole file, or a slice of one
    std::filesystem::path path; u64 offset = 0; u64 length = 0;  // length 0 ⇒ whole file
    std::expected<std::vector<std::byte>, std::string> read() const;
};

// Existing loaders gain a ByteSource overload; the file overload becomes ByteSource{path,0,0}.
Ref<MeshAsset>    loadMeshFromSource(AssetServer&, Renderer&, Uuid subId, ByteSource);
Ref<TextureAsset> loadTextureFromSource(AssetServer&, Renderer&, Uuid subId, ByteSource, Colorspace);
```

`loadMesh` (`geometry.cppm:1170`) must accept a **base offset**: today it hardcodes
`verticesOffset = sizeof(SMeshHeader)` and validates against file size (`geometry.cppm:1205`). The chunk
path passes the chunk's base; the loader reads the `SMeshHeader` at `base`, treats its internal offsets as
self-relative, and validates against the **chunk length**, not the file size.

## `loadModelAsset`

```cpp
struct ModelAsset {                      // cached per modelId
    ContainerMetadata meta;
    ContainerReader reader;              // holds the path + TOC for lazy chunk reads
};
Ref<ModelAsset> loadModelAsset(AssetServer&, Uuid modelId);   // negative-cached like loadMeshAsset

// Resolve a sub-id to a live GPU resource, honoring remap, using the sub-id as the cache key.
Ref<MeshAsset>    resolveMesh(AssetServer&, Renderer&, Uuid modelId, Uuid subId);
Ref<TextureAsset> resolveTexture(AssetServer&, Renderer&, Uuid modelId, Uuid subId);
Ref<MaterialAsset> resolveMaterial(AssetServer&, Renderer&, Uuid modelId, Uuid subId);
```

Resolution order per sub-id:
1. If `meta.remap[subId].external` is set → load from that standalone file (`ByteSource{external,0,0}`),
   warning (not failing) if the target is missing, then falling back to the embedded chunk.
2. Else find the TOC entry for `subId` → `ByteSource{containerPath, entry.offset, entry.length}` → load.
3. Cache the resulting `Ref` in `meshRefByUuid`/`textureRefByUuid`/`materialRefByUuid` under **`subId`**.

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `ByteSource`, `loadModelAsset`, `resolveMesh/Texture/Material`,
  `loadMeshFromSource`/`loadTextureFromSource`; rekey the GPU caches on sub-id; add `modelRefByUuid` to
  `AssetServer`; clear it in `clearAssetCaches`.
- `engine/source/saffron/geometry/geometry.cppm` — give `loadMesh` (and `loadAnimation`) a base-offset +
  bounded-length overload; the file path calls it with `base=0, length=fileSize`.

## Steps

1. Add the base-offset overload to `loadMesh`/`loadAnimation`; validate internal offsets against the
   bounded length (not file size). Keep the existing `loadMesh(path)` working via the overload.
2. Add `ByteSource` + the `*FromSource` loaders; route `loadMeshAsset`/`loadTextureAsset` through them.
3. Implement `loadModelAsset` (cache the `ContainerReader`+meta) and `resolveMesh/Texture/Material` with
   the remap-then-chunk order, caching by sub-id.
4. Self-test: `bakeModel` (05) a container, `loadModelAsset`, `resolveMesh`/`resolveTexture` a sub-id, and
   compare the loaded mesh's vertex/index counts + a texture's dimensions against the source graph. Add a
   "missing remap target falls back with a warning" case.

## Gate / done

- `make engine` clean; the resolve self-test round-trips mesh + texture from a baked container; the
  base-offset `loadMesh` still loads a standalone `.smesh` identically; `make prepare-for-commit` clean.

## Risks

- **Silent null on failure:** the negative-cache pattern caches a null `Ref` and renders nothing without
  error (the existing behavior). A wrong base offset or a TOC bug will *look* like an empty mesh, not a
  crash — add explicit logging on a chunk-load miss so phases 07+ can diagnose; phase 14/15 add proper
  broken-reference reporting.
- **`loadMesh` layout check (`geometry.cppm:1205`):** the inconsistent-layout validation will reject a
  correct chunk if the bounds are validated against file size instead of chunk length — the subtle part of
  step 1.
- **Cache-key migration:** any code currently caching by mesh-file uuid must move to sub-id; audit
  `loadMeshAsset` callers so a standalone mesh and an embedded mesh don't double-cache or alias.
- **Reader lifetime:** `ModelAsset` holds an open path/TOC; ensure it's dropped before teardown
  (`waitGpuIdle` ordering) like other `Ref`s.
