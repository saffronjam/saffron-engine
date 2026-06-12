# Phase 05 — Container writer (bake `.smodel`)

**Status:** NOT STARTED
**Depends on:** 04

## Goal

Implement `bakeModel`: take the import graph (`ImportedModel`) + `ImportOptions` and produce one
`assets/models/<uuid>.smodel` — per-sub-asset byte images (mesh via `saveMesh`-to-buffer, material via
`materialAssetToJson`, texture as raw encoded bytes, animation via `saveAnimation`-to-buffer) assembled
into chunks + a `ChunkTOC` + the `MetadataChunk` (nodes/skin/materials/import recipe). **Imported materials
become first-class `.smat`-JSON chunks** with stable sub-ids. `bakeModel` writes the catalog rows and does
**not** upload to GPU and does **not** spawn. Defers: loading chunks back (06), instantiating (07).

## Why

This is the **build** stage and the heart of the plan: it turns the scatter-50-files import into one
self-contained file, and it makes imported materials durable for the first time (closing the
`applyImportedMaterials` gap, `assets.cppm:2329`). Not uploading/spawning here is the decoupling that
phase 07 builds on. Reusing the existing serializers as chunk codecs means `.smesh`/`.smat`/`.sanim`/image
bytes are wrapped, never reinvented.

## `bakeModel`

```cpp
struct BakeResult { Uuid modelId; std::filesystem::path path; std::vector<AssetEntry> rows; };

// Pure-ish: graph + options → one .smodel on disk + the catalog rows it contributes.
// No GPU upload, no entity spawn. modelId is reused on reimport (read from an existing container).
std::expected<BakeResult, std::string>
bakeModel(AssetServer& assets, const ImportedModel& graph, const ImportOptions& options,
          Uuid modelId /* 0 = mint new */);
```

Chunk production (reusing existing machinery as **buffer** encoders — add `*ToBuffer` overloads where the
current functions only write files):

| Sub-asset | Chunk | Encoder | Source |
|---|---|---|---|
| mesh geometry | `MESH` | `saveMeshToBuffer(mesh)` | the `SMeshHeader` image (`geometry.cppm:1136`) verbatim |
| material | `SMAT` | `materialAssetToJson(mat)` → bytes | the material-uplift `.smat` JSON |
| texture | `STEX` | raw encoded bytes (PNG/JPG/HDR), `flags`=colorspace | the bytes the importer already copies |
| animation | `SANM` | `saveAnimationToBuffer(clip)` | the `SANimHeader` image (`geometry.cppm:263`) |
| metadata | `META` | `encodeContainerMetadata(meta)` (phase 02) | nodes/skin/materials/import recipe |

The `MetadataChunk` is filled from the graph: `subAssets` (one per chunk, with the stable sub-ids from
phase 04), `materials` (subId + factor summary), `nodes`/`skin` (the hierarchy that supersedes `.srig`),
and `import` (`sourcePath`, `sourceHash` = hash of the source bytes, `importerVersion`, `options.toJson()`).
`remap` is empty at bake.

Then `writeContainer` (phase 01) frames it all. `bakeModel` adds catalog rows: one parent
`AssetType::Model` row (`container=0`, `path=models/<uuid>.smodel`) plus one row per sub-asset
(`container=modelId`, `chunk=idx`, `path` = the container path).

## Repointing `importModel`

`importModel` (`assets.cppm:2128`) becomes: `translateModel` (04) → `bakeModel` → return the `BakeResult`.
It **stops** writing loose `.smesh`/textures and **stops** calling `spawnModel`. (The `import-model`
command's spawn moves to phase 07/08; for this phase, keep a temporary caller or guard so the engine still
builds and the old e2e can be updated — see Steps.)

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `bakeModel`, `BakeResult`; repoint `importModel`; add the
  catalog-row construction; reuse `materialAssetToJson` and `registerTextureBytes`' byte-copy path.
- `engine/source/saffron/geometry/geometry.cppm` — add `saveMeshToBuffer` / `saveAnimationToBuffer`
  (buffer-returning siblings of `saveMesh`/`saveAnimation`) so the file-writing path and the chunk path
  share one encoder.

## Steps

1. Add `saveMeshToBuffer`/`saveAnimationToBuffer` (refactor `saveMesh`/`saveAnimation` to write into a
   `std::vector<std::byte>` that the file path then dumps — one code path, two sinks).
2. Implement `bakeModel`: walk the graph → encode each sub-asset to a chunk → fill `ContainerMetadata`
   (incl. `sourceHash`) → `writeContainer` → build `AssetEntry` rows.
3. Repoint `importModel` to `translateModel`+`bakeModel`; remove loose-file writes and the inline spawn.
4. Update the import e2e to assert: exactly one `.smodel` on disk, N catalog rows (1 model + sub-assets),
   no loose `textures/*.png` for an embedded import.
5. Self-test: `bakeModel` a synthetic 2-mesh/3-texture/2-material graph → `readContainerMetadata` →
   assert sub-asset count, material chunks present, nodes/skin round-trip; `readChunk` a `MESH` chunk and
   `loadMesh` it from the slice (proves the `.smesh` image is intact — full path lands in 06).

## Gate / done

- `make engine` clean; bake self-test + updated import e2e pass (one container, no loose files, materials
  embedded); `make prepare-for-commit` clean.
- No GPU upload or spawn happens inside `bakeModel`.

## Risks

- **`saveMesh` assumes whole-file offsets:** the `MESH` chunk is a `.smesh` image starting at the chunk's
  (16-B aligned) offset; phase 06's loader must pass a base offset. Bake it correctly here (the image is
  self-relative; absolute placement is the TOC's job) so 06 only adds a base.
- **Determinism:** `sourceHash` must be the hash of the *source* bytes (stable), and the bake output should
  be reproducible for the cache (11) / reimport-skip (13). Avoid `newUuid`/time inside the chunk bytes.
- **Texture dedup deferred:** ten meshes sharing one texture embed ten copies for now (accepted tradeoff;
  README decision 3). Note it; a content-hash dedup is a later optimization, not a format change.
- **Build-graph ordering:** the engine must still compile/run between phases — guard the now-spawnless
  `importModel` so phase 06/07 can wire the rest without a broken intermediate tree.
