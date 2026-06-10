# Phase 04 — Tangents + `.smesh` v3

**Status:** NOT STARTED
**Depends on:** — (independent of 01–03; can run in parallel)

## Goal

Add a per-vertex **tangent** so tangent-space normal mapping is possible. Bump the `.smesh` format
to v3, generate/import tangents, and plumb the tangent through the vertex shader to `VertexOutput`.
Without this, normal maps (phase 05) cannot be applied correctly on arbitrary geometry.

## Why

Normal maps store directions in tangent space; converting them to world space needs a TBN basis
(tangent, bitangent, normal) per fragment. The bitangent is reconstructed from N×T·sign, so we only
store tangent `xyz` + handedness `w` (vec4). glTF often provides tangents; when absent (OBJ, some
glTF), generate them from positions + UVs (MikkTSpace-style, or a simple per-triangle accumulate).

## Format change

`SMeshHeader` is versioned (`magic 'SMSH'`, `version` 1=unskinned, 2=skinned). Add **v3**: the
`Vertex` stream gains a `glm::vec4 tangent` (stride 32 → 48). Keep readers back-compatible: v1/v2
meshes load with a zero tangent (or a generated one). `vertexStride` in the header already records
the stride, so a v3 reader can branch on it.

```cpp
// geometry.cppm — Vertex grows:
struct Vertex { glm::vec3 position; glm::vec3 normal; glm::vec2 uv0; glm::vec4 tangent; }; // stride 48
```

## Files to touch

- `engine/source/saffron/geometry/geometry.cppm` — extend `Vertex`; bump `SMeshHeader.version`/stride
  handling in `saveMesh`/`loadMesh` (v3 read + write, v1/v2 still load); add a `generateTangents(Mesh&)`
  helper; in `extractGltf*`/the OBJ path, read glTF `TANGENT` accessor when present else generate.
- `engine/source/saffron/assets/assets.cppm` — `importModel` bakes v3; `uploadMesh` passes tangents.
- `engine/source/saffron/rendering/` — the mesh `VertexInputState`/attribute descriptions gain the
  tangent attribute (location after uv0); `GpuMesh` upload keeps the new stride.
- `engine/assets/shaders/mesh.slang` — `VertexInput`/`SkinnedVertexInput` gain `float4 tangent`;
  `VertexOutput` gains `float3 worldTangent` + handedness; both `vertexMain`/`vertexMainSkinned`
  transform the tangent by the model/normal matrix (and skin matrix for skinned).

## Steps

1. Extend `Vertex` + the vertex-input attribute descriptions; bump `.smesh` to v3 (write v3, read v1/v2/v3).
2. Add `generateTangents` (accumulate per-triangle `(dPos, dUV)` → tangent, orthonormalize vs normal,
   store handedness in `.w`). Use it when the source lacks tangents.
3. Plumb tangent through `vertexMain`/`vertexMainSkinned` → `VertexOutput.worldTangent`.
4. Re-import a known model; verify lit shading is unchanged (tangent unused until phase 05) and the
   new stream round-trips (`saveMesh`→`loadMesh`).

## Gate / done

- `make engine` clean; an existing scene with v1/v2 meshes still loads (back-compat path).
- Re-importing a glTF produces a v3 `.smesh`; present-only smoke unchanged.
- `make prepare-for-commit` clean.

## Risks

- **Back-compat:** existing project `.smesh` files are v1/v2. The loader must handle all three by the
  `version`/`vertexStride` in the header, not assume v3. Test with a pre-existing mesh.
- **Tangent generation quality:** a naive accumulate is fine for normal maps; mirrored UVs need the
  handedness sign or seams flip. Store and use `.w`.
- Stride change touches every place that assumes `sizeof(Vertex)==32`; grep for it.
