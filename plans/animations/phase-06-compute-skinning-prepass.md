# Phase 6 — compute skinning pre-pass + deformed-vertex buffer

**Status:** COMPLETED

## Goal

Stop skinning inside the graphics vertex shader and start **deforming once** in a compute pre-pass that
writes a skinned vertex buffer in the base 32-byte `Vertex` layout. The scene pass then reads that buffer
as an ordinary static mesh (binding 0) — no skinned shader variant. This is the foundation for Phases
7–9: every later pass, and the BLAS, will read this same deformed buffer, which is the only way to make
TAA, shadows, AO, and ray tracing correct for characters (collapsing 5+ skinned PSO permutations to zero).

## What exists to build on

- `vertexMainSkinned` (`mesh.slang:325`) does the LBS math today: `skinMatrix = Σ wᵢ·jointMatrices[base+jointᵢ]`
  where `base = inst.texture.y` (the per-instance joint offset, `renderer_types.cppm:1602`); the palette
  is the SSBO at **set 2, binding 1** (`mesh.slang:151`). Port this exact math into compute.
- The palette: `ensureJointCapacity(renderer, frame, count)` (`renderer_drawlist.cpp:251`, grow-only,
  starts at 128, doubles) + `submitDrawList(..., joints)` (`:287`) memcpy into
  `Instancing.jointBuffers[frame]` (`renderer_types.cppm:1065`).
- `GpuMesh.skinBuffer` (`renderer_types.cppm:202`) is the `VertexSkin` stream, created by `uploadMesh`
  (`renderer_drawlist.cpp:55`) when skin is present; `GpuMesh.vertexBuffer` is the static bind-pose
  `Vertex` stream.
- Compute passes in the graph: `RgUsage::{StorageReadCompute, StorageWriteCompute, StorageImageRWCompute,
  SampledReadCompute}` (`render_graph.cppm:25`); `addPass(graph, pass)` (`:123`) declares accesses and the
  graph **derives all barriers** (the `AGENTS.md` "never write a barrier by hand" rule).
- The `Instancing` grow-only buffer pattern (`renderer_types.cppm:1065`) is the model for allocating the
  transient deformed buffer until graph-created transient resources exist (Status: not built).
- Shaders compile from `engine/assets/*.slang` in CMake (`slangc -target spirv`).

## Work

### 1. `skin.slang` compute kernel

New `engine/assets/shaders/skin.slang`: a compute entry that, per vertex of a skinned mesh-instance,
reads the static `Vertex` (position/normal/uv0) + the `VertexSkin` (joints/weights) + the joint palette
(at the instance's `jointOffset`), computes `skinMatrix = Σ wᵢ·palette[base+jointᵢ]`, and writes a
deformed `Vertex` (skinned position; normal by the inverse-transpose of the upper 3×3, or the skinMatrix
3×3 for rigid-ish bones; uv0 copied). One thread per vertex; dispatch `ceil(vertexCount/64)` groups.

Bindings: `StorageReadCompute` on the static `Vertex` buffer, the `VertexSkin` buffer, and the palette;
`StorageWriteCompute` on the deformed `Vertex` buffer. A small push-constant or per-dispatch uniform
carries `vertexCount` + `jointOffset`.

### 2. The deformed-vertex buffer (transient, grow-only)

Add to `Instancing` (or a sibling `Skinning` struct in `renderer_types.cppm`) a per-frame-in-flight,
grow-only device buffer sized to the **sum of skinned-instance vertex counts** for the frame, plus a
per-skinned-instance offset into it (mirror `jointBuffers`/`jointCapacity` exactly: `std::array<Ref<Buffer>,
MaxFramesInFlight>` + capacity). Each skinned draw item gets a `deformedVertexOffset` computed when the
draw list is assembled (alongside the existing `jointOffset`).

> This is the engine's first real graph-created transient in spirit; until that subsystem lands, the
> grow-only-per-frame allocation is the sanctioned interim (README locked decision). **Log** the peak
> allocation so the never-shrinks VRAM growth is visible.

### 3. The compute pass + scene-pass rewire

- During graph setup (before the scene pass records), add a `skin` compute pass via `addPass` that, for
  each skinned mesh-instance, dispatches `skin.slang`. Declare the palette + static `Vertex` + `VertexSkin`
  as `StorageReadCompute` and the deformed buffer as `StorageWriteCompute` so the graph inserts the
  compute→vertex-input barrier.
- In `recordSceneDrawList` (`renderer_drawlist.cpp:471`): for skinned batches, bind the **deformed buffer**
  (at `deformedVertexOffset`) on **binding 0** and use the **non-skinned** `vertexMain` pipeline — i.e. a
  skinned batch now records exactly like a static batch. Remove the scene-pass dependence on
  `vertexMainSkinned` / the `skinBuffer` second binding.
- Keep `vertexMainSkinned` in the tree only if still referenced; otherwise retire it once Phases 7–9 also
  read the deformed buffer (it becomes dead). The palette SSBO stays (the compute kernel consumes it).

## Validation (done criteria)

- `make engine` green; the skinned fixture renders **identically** in the scene pass to before (same
  silhouette/deformation) — this phase is a refactor of *where* skinning happens, not *what* it produces.
  Diff a screenshot (the PNG screenshot path) of a posed frame before/after.
- Validation-clean log (the smoke/contract gate) — no missing-barrier validation errors from the new
  compute pass.
- `make prepare-for-commit` clean.
- `docs/`: add `docs/content/explanations/rendering/compute-skinning.md` (deform-once, the deformed
  buffer, why compute over per-pass VS permutations) + hub row.

## Notes / gotchas

- **Normal skinning:** transform normals by the skin matrix's inverse-transpose (or accept the 3×3 for
  uniformly-scaled rigs); getting this wrong shows as lighting that swims under animation.
- **Barrier discipline:** declare every access on the `RgPass` — a missed declaration is a data race the
  validation layer or a flicker will reveal. Do not hand-insert `vkCmdPipelineBarrier`.
- **Offsets share a word:** `InstanceData.texture` is `uvec4 {albedo, jointOffset, mr, 0}`
  (`renderer_types.cppm:1602`); the deformed-vertex offset is a *CPU-side* binding offset, not packed into
  `texture` — keep it on the draw item, not the instance SSBO, unless a downstream pass needs it.
- This phase only rewires the **scene pass**; the other passes still `continue` on `batch.skinned`
  (Phase 7 removes those). That's fine — the deformed buffer exists and is correct; Phase 7 just points
  more passes at it.
