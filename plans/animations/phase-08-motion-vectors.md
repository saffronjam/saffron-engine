# Phase 8 — skinned motion vectors (palette double-buffer + `prevModel`)

**Status:** NOT STARTED

## Goal

Fix TAA ghosting on animated and moving meshes. Two motions are missing today: **deformation motion**
(a bone moved between frames) and **per-instance object motion** (the whole entity translated/rotated).
Solve deformation by skinning a **second deformed buffer with last frame's palette** so `motion.slang`
reads the previous skinned position as a plain buffer fetch (no double-skinning in a shader); solve object
motion by adding **`prevModel`** to `InstanceData`. Then drop the motion-pass skinned guard so skinned
geometry emits correct velocity.

## What exists to build on

- Motion-vector prepass skips skinned batches: `if (batch.skinned) continue;` at `renderer_drawlist.cpp:657`.
- `motion.slang` is **camera-only** today — its header explicitly says "geometry assumed static —
  per-instance previous-model tracking is a later add" (`motion.slang:1-4`); it uses only `inst.model` and
  `renderer.prevViewProj` (`renderer_types.cppm:1526`) for the camera half.
- `InstanceData` (`renderer_types.cppm:1597`) has `model`, `normalMatrix`, `baseColor`, `texture` (uvec4),
  `pbr`, `emissive` — **no `prevModel`** (verification gotcha).
- The palette: grow-only `Instancing.jointBuffers[frame]` (`renderer_types.cppm:1065`) + `ensureJointCapacity`
  (`renderer_drawlist.cpp:251`); assembled per frame in `submitDrawList` (`:287`) / `renderScene`.
- Phase 6's deformed-vertex buffer (current frame); Phase 7's all-pass consumption.

## Work

### 1. Previous-frame palette + previous deformed buffer

- Keep **last frame's** joint palette: either a second `jointBuffers` slot per frame-in-flight, or retain
  frame N−1's palette keyed per skinned instance by entity (so an instance that existed last frame has a
  valid previous palette; new instances use the current palette → zero deformation motion, correct).
- Run the Phase-6 `skin.slang` compute **twice** per skinned instance: once with the current palette →
  current deformed buffer (Phases 6/7), once with the previous palette → a **previous** deformed buffer.
  Both are grow-only per-frame buffers in the `Skinning` struct.

### 2. `prevModel` on `InstanceData`

Add `glm::mat4 prevModel` to `InstanceData` (`renderer_types.cppm:1597`) — or a parallel previous-instance
SSBO if you'd rather not grow the per-instance struct. Populate it each frame from the entity's
**previous** world matrix (cache last frame's `model` per instance keyed by entity; new instances set
`prevModel = model`). This is independent of skinning and fixes object-motion ghosting for *any* moving
entity.

### 3. Skinned motion in `motion.slang`

- Drop the `batch.skinned` guard at `:657`; record skinned items in the motion pass binding the **current**
  deformed buffer on binding 0.
- `motion.slang` computes current clip-space position from the current deformed vertex + `inst.model` +
  `viewProj`, and previous clip-space position from the **previous** deformed vertex + `inst.prevModel` +
  `renderer.prevViewProj`; the motion vector is their screen-space difference. For static (non-skinned)
  meshes, current == previous deformed buffer is just the static `Vertex` buffer twice (or skip the prev
  buffer and use the static buffer for both) — so the same shader handles both with no skinning math
  inside it (the prev position is a buffer read, per the README rationale).

## Validation (done criteria)

- `make engine` green, validation-clean.
- TAA: a fast-moving bone (e.g. a waving limb) and a translating entity **no longer ghost/smear** under
  TAA — compare a motion-heavy frame before/after (PNG screenshot, or inspect the motion-vector target if
  a debug view exists).
- `make prepare-for-commit` clean.
- `docs/`: extend `compute-skinning.md` (or the TAA/motion-vector page) with the double-buffer + `prevModel`
  explanation.

## Notes / gotchas

- **New-this-frame instances** (just spawned, or first frame of play) have no valid previous palette/model
  — set previous = current so they emit zero motion rather than a garbage velocity (which would flash).
- Doubling the compute dispatch + a second deformed buffer adds bandwidth/VRAM; it's the standard cost of
  correct skinned motion vectors and is why the deform-once architecture (Phase 6) pays off — the prev
  buffer is just one more dispatch, not a re-architecture.
- Bumping `InstanceData` size touches the instance SSBO stride — verify the shader's `InstanceData` layout
  matches the C++ struct exactly (the existing `texture.y` packing is layout-sensitive).
