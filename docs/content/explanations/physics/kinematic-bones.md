+++
title = 'Kinematic bodies and bone following'
weight = 5
+++

# Kinematic bodies and bone following

A **Kinematic** body is one the simulation does not push but *pushes back from*. It ignores gravity
and contacts — its motion is set explicitly each step — yet a dynamic body that hits it bounces off
correctly, because the kinematic body's swept motion imparts the right contact velocity. This is how
a moving platform carries a crate, and how a walking character's limbs shove the world around.

## The three binding modes

A skeleton and a physics body can be wired together three ways. Naming them keeps the phases
straight:

- **(a) static** — the body *is* the character's collision proxy; animation plays independently on
  top (the character controller).
- **(b) animation → physics** — per-bone kinematic bodies *follow* the animated pose, so the world
  reacts to a moving character. **This is what this page covers.**
- **(c) physics → animation** — the body drives the bone (the ragdoll), writing back into the pose.

Mode (b) is strictly one-way: animation is authoritative for the skeleton, and physics only *reads*
it. The `PoseBuffer.override_`/`weight` blend layer is left untouched here — that seam is mode (c)'s.

## MoveKinematic, never a teleport

The simulation moves a kinematic body with `BodyInterface::MoveKinematic(id, target, dt)`, which
derives the linear + angular velocity that carries the body to the target over `dt` and integrates
it as a swept motion. A teleport (`SetPosition`) would leave the body's velocity at zero, so a
dynamic body it overlaps is resolved as a static penetration push with no momentum — the crate would
*ooze* off the platform instead of getting *hit*. Deriving velocity from `(target − current)/dt` is
the whole point, so the same `PhysicsFixedStep` feeds both `MoveKinematic` and `Update` and the
swept velocity matches the integration step.

Every kinematic body — a free `RigidbodyComponent` whose motion type is `Kinematic`, and every
per-bone body — is driven this way, each fixed substep, toward its entity's current world transform.

## Reading the pose: compose, don't trust the cache

The bone target is each joint's animated world transform. The subtle trap: the cached
`WorldTransformComponent` is **one frame stale** during the simulation tick, because the pass that
refreshes it (`updateWorldTransforms`, inside `renderScene`) runs *after* the update. So the follow
step composes the world matrix itself from the fresh `PoseOverrideComponent` the animation evaluator
just wrote, rather than reading the cache. Getting this wrong is the single most likely source of a
one-frame lag.

The ordering that makes this work is already fixed: `tickAnimation` writes the pose overrides
*before* `tickPlay` runs the simulation seam, so by the time the bone bodies read the skeleton, this
frame's pose is in hand.

## Per-bone bodies, auto-fit on add

A rig opts in with a `KinematicBonesComponent` (`enabled` + an optional `driven` index list; empty
means every joint). Adding it auto-fits a capsule per bone into the reserved
`BonePhysicsComponent.shapeHalfExtents` — half-height from the joint-to-child rest distance, radius a
fraction of it, with a small default for leaf joints so Jolt never sees a degenerate capsule. On
play, one **kinematic** body is created per driven joint, keyed by the joint entity so it tears down
with the world on stop. The bodies are **independent colliders** — no constraints link them; that
joint graph is the ragdoll. A rig with `KinematicBonesComponent` is simply a moving collision proxy,
which is all "the world reacts to a walking character" needs.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Kinematic drive (free + bone bodies) | `engine/source/saffron/physics/physics.cpp` | `stepPhysics` (the `MoveKinematic` loop), `worldPose` |
| Per-bone body creation | `engine/source/saffron/physics/physics.cpp` | `buildBoneBodies` |
| The opt-in component | `engine/source/saffron/scene/scene.cppm` | `KinematicBonesComponent` |
| Auto-fit + toggle | `engine/source/saffron/control/control_commands_scene.cpp`, `control_commands_physics.cpp` | `fitBoneCapsules`, `set-kinematic-bones` |
