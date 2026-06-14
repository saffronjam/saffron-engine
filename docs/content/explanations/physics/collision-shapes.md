+++
title = 'Collision shapes and materials'
weight = 3
+++

# Collision shapes and materials

A `ColliderComponent` can be one of five shapes. Two are **analytic** — a sphere and a capsule,
sized from a few numbers — and two are **cooked** from the entity's mesh: a convex hull and a full
triangle mesh. The box rounds out the set. Which one you pick depends on whether the body moves and
how closely the collision shape needs to track the visible geometry.

## The five shapes

- **Box** — half-extents per axis. The cheap default.
- **Sphere** — a radius (packed into `halfExtents.x`). The cheapest dynamic shape; rolls.
- **Capsule** — a radius (`.x`) and a cylinder half-height (`.y`), Y-up. The standard character /
  limb shape.
- **ConvexHull** — the convex wrapping of the source mesh's vertices, cooked from the `.smesh`.
  Valid on **dynamic** bodies, and the right choice when a box/sphere/capsule is too coarse.
- **Mesh** — the exact triangle mesh, cooked from the `.smesh`. **Static or kinematic only** — Jolt's
  `MeshShape` cannot back a dynamic body.

### Mesh on a dynamic body fails loudly

Putting a `Mesh` shape on a `Dynamic` rigidbody is a real authoring error, not something to paper
over. The world build **skips that body and logs** a message naming the entity and the fix (use a
ConvexHull for dynamic, or make the body static/kinematic). It is never silently downgraded to a box —
silent substitution would hide the mistake and is exactly the kind of compatibility shim this codebase
refuses. ConvexHull is the dynamic-capable cooked shape.

## Cooking re-reads the `.smesh`

The GPU mesh keeps only its vertex/index buffers and an AABB — it discards the CPU vertices after
upload. So convex-hull and mesh cooking re-read the baked `.smesh` through `loadMeshCpuAsset`
(a catalog lookup + a bytes read + decode, no GPU upload and no cache entry — cooking is a one-shot
at `Edit → Playing`, not the draw path). The vertices and indices are fed to Jolt in **mesh index
order**, never through a hash set, so the cooked shape — and therefore the simulation — is
byte-reproducible run-to-run, which is what the cross-platform-deterministic build needs.

The cook crosses the module boundary as a `std::function<Result<Mesh>(Uuid)>` the Host binds to
`loadMeshCpuAsset`. That keeps `<Jolt/…>` out of `Saffron.Assets` (the cook returns a plain `Mesh`)
and keeps the asset reader out of the one Jolt translation unit.

## Auto-fit is the default, not a button

Adding a `ColliderComponent` fits its shape to the entity's mesh AABB automatically — the locked
design decision. The fit is shape-aware:

| Shape | Fit from the AABB half-extents `h` and centre `c` |
|---|---|
| Box | `halfExtents = h`, `offset = c` |
| Sphere | radius `= max(h.x, h.y, h.z)` (the box's bounding sphere — never smaller than the mesh) |
| Capsule | radius `= max(h.x, h.z)`, half-height `= max(0, h.y − radius)`, Y-up |
| ConvexHull / Mesh | a fallback box in `halfExtents`; the cook uses the real geometry, `sourceMesh = the mesh` |

Auto-fit reads the GPU mesh AABB (already computed at upload, the same bounds the thumbnail framing
uses), in **mesh-local space** — the body transform carries the entity's scale, so the fit never
bakes scale in and a re-fit after scaling reproduces the same local dims. Three layers cover the
authoring: auto-fit on add (the default that just works), `fit-collider` to re-fit on demand (after a
shape or mesh change), and `set-component-field` for manual overrides.

> **Capsule axis (v1):** the capsule is fitted Y-up regardless of the mesh's dominant axis. A mesh
> long on X or Z is still fitted Y-up; per-axis capsule orientation is a later refinement.

## PhysicsMaterial: friction and restitution

`ColliderComponent.material` carries `friction` (0 = ice, 1 = rubber) and `restitution`
(bounciness, 0..1). These are written onto the body at creation and are what produces the visible
behaviour — a high-friction box stops sliding where a low-friction one keeps going, and a
high-restitution sphere rebounds where a `restitution = 0` one comes to rest on first contact.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Shape build + cook + Mesh-on-dynamic guard | `engine/source/saffron/physics/physics.cpp` | `buildColliderShape`, `MeshCookSource` |
| CPU mesh decode for cooking | `engine/source/saffron/assets/assets.cppm` | `loadMeshCpuAsset` |
| Shape-aware auto-fit | `engine/source/saffron/control/control_commands_scene.cpp` | `fitColliderToMesh` |
| Re-fit command | `engine/source/saffron/control/control_commands_physics.cpp` | `fit-collider` |
