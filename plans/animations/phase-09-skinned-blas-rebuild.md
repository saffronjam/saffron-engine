# Phase 9 — skinned BLAS rebuild for ray tracing

**Status:** COMPLETED

## Goal

Make animated characters deform in ray-traced effects. Today the BLAS is built once from the **static
bind-pose** vertex buffer (`renderer_detail.cppm:490`), so RT shadows, DDGI, and ReSTIR all see a frozen
bind-pose silhouette for skinned meshes. Rebuild (refit) the BLAS each frame from the Phase-6 **deformed
vertex buffer** for skinned instances so the acceleration structure matches what's on screen.

## What exists to build on

- `buildBlas` (`renderer_detail.cppm:483-554`) sets
  `geom.geometry.triangles.vertexData.deviceAddress = bufferDeviceAddress(renderer, mesh.vertexBuffer)`
  (`:490`) — the static bind-pose buffer, and is called on mesh upload (one-shot per mesh).
- Phase 6/7's deformed `Vertex` buffer (current frame) in the `Skinning`/`Instancing` struct, with a
  per-skinned-instance offset; same 32-byte `Vertex` layout the BLAS already expects.
- The render graph + `RgUsage` for declaring the compute→AS-build dependency (the deformed buffer must be
  written by the skin compute pass before the BLAS refit reads it).
- The engine already runs RT effects (RT shadows / DDGI / ReSTIR per the Status list), so a TLAS exists
  and instances reference per-mesh BLASes.

## Work

### 1. Per-frame BLAS refit for skinned instances

For each skinned mesh-instance, build (or **refit** via `UPDATE` mode where the topology is unchanged — it
is, only vertex positions move) a BLAS each frame whose `vertexData.deviceAddress` points at the
**deformed** buffer at the instance's offset, rather than the static `mesh.vertexBuffer`. A skinned
instance gets its **own** BLAS (the deformed geometry is per-instance), unlike static meshes that share
one BLAS across instances.

- Allocate a per-skinned-instance BLAS (grow-only, keyed by instance/entity, like the other per-frame
  skinning resources). First frame = full `BUILD`; subsequent frames with the same topology = `UPDATE`
  (refit), which is much cheaper.
- Add the refit as a graph pass (or fold into the existing AS-build step) declaring a read of the deformed
  buffer so it's ordered after the skin compute pass; rebuild the **TLAS** afterward so it references the
  refit BLASes (the TLAS rebuild likely already happens per frame — point skinned instances at their
  per-frame BLAS).

### 2. Scope guard

Gate this to skinned instances only; static meshes keep their shared, build-once BLAS. Make the per-frame
refit conditional on the instance actually being skinned **and** RT being enabled, so non-RT or static
scenes pay nothing.

## Validation (done criteria)

- `make engine` green, validation-clean (AS-build synchronization correct — the deformed buffer is fully
  written before the refit reads it).
- Visual: with an RT effect on (RT shadows or DDGI), the animated character's **ray-traced shadow / GI
  contribution follows the pose** rather than the bind pose — compare a posed frame before/after.
- `make prepare-for-commit` clean.
- `docs/`: add the "skinned ray tracing" note to `compute-skinning.md` (per-instance refit BLAS, UPDATE
  mode, the cost).

## Notes / gotchas

- **Cost:** a per-frame per-character BLAS refit is real GPU time; `UPDATE`/refit (vs full rebuild) keeps
  it bounded since skinned topology never changes frame-to-frame. Measure with the profiler; this is the
  most expensive phase of the rendering block and the most optional for non-RT content.
- A refit BLAS can degrade in quality if vertices move far from the original build pose over time; a
  periodic full rebuild (every N frames, or on large pose deltas) keeps traversal efficient — a known RT
  skinning technique, worth a flag.
- This phase **completes** the compute-skinning correctness block (6→7→8→9). After it, skinned characters
  are correct in raster (scene/depth/shadow/AO), TAA, and RT — there is no remaining "skinned draws only
  in the scene pass" caveat.
