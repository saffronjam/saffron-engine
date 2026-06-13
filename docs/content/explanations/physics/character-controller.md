+++
title = 'Character controller'
weight = 6
+++

# Character controller

A walking character is not a rigid body. A box-on-a-floor tumbles, bounces, and resolves penetration
the way the solver sees fit — none of which is how a player expects to move. The engine uses Jolt's
**`CharacterVirtual`**: a kinematic *sweep* object that pushes a capsule through the world and
resolves penetration and wall-sliding without being a simulated body. It walks across floors, steps
over small ledges, and slides along walls natively.

## The component split

A character entity is a `TransformComponent` + a `ColliderComponent` (a capsule) + a
`CharacterControllerComponent` — and **no `RigidbodyComponent`**. The controller reuses the
collider's auto-fit capsule (radius + half-height) rather than introducing a second capsule, so
there is one source of truth for the shape. The component carries only movement parameters: a
horizontal `maxSpeed`, a `maxSlopeAngle` (steeper ground is treated as a wall), a `maxStepHeight`
(ledges up to this are stepped over), and a `gravityFactor`. The desired velocity, the integrated
vertical velocity, and the last ground state are runtime fields, reset on each play.

A `CharacterVirtual` is **not** a body, so the world build skips making a static body for a collider
that also has a `CharacterControllerComponent` — otherwise the static capsule would block the
character's own sweep.

## Stepping and write-back

Each fixed substep, after the rigid-body `Update` settles the world, every character is advanced:
gravity accumulates into the vertical velocity (zeroed when grounded), the `move-character` desired
velocity is folded in and clamped to `maxSpeed`, and `CharacterVirtual::ExtendedUpdate` runs the
stick-to-floor + WalkStairs sweep against the world, using the layer filters from the collision
matrix (the character lives in the `Character` layer). The resolved position is then written back
into the entity-root `TransformComponent`, the same frame, before `renderScene` — so the visible mesh
follows, exactly as the rigid-body and animation write-backs do.

This is **binding mode a**: the controller positions the root and nothing more. Any
`AnimationPlayerComponent` on the entity plays *independently on top* — the controller never reads or
drives the pose. Root-motion extraction and locomotion blending are a different coupling (the
animation plan's blend-layer producers), deliberately kept out of here.

## move-character is the input seam

`move-character {entity, velocity, jump?}` writes the desired horizontal velocity (and an optional
jump impulse) onto the component; the actual sweep happens on the next physics step. The command only
flips component fields — identical to how `set-foot-ik` only sets fields the evaluator consumes next
frame. For now this command *is* the input seam; mapping real input to it is gameplay's job.

> **`CharacterVirtual`, not `Character` or a kinematic rigidbody.** The body-backed `Character` and
> the kinematic-rigidbody path are deliberately not reused — a character is its own thing. The
> controller assumes a root entity (local == world); a parented character is a later refinement.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| The component | `engine/source/saffron/scene/scene.cppm` | `CharacterControllerComponent` |
| Create + step + write-back | `engine/source/saffron/physics/physics.cpp` | `addCharacter`, `stepPhysics` (the `ExtendedUpdate` loop) |
| The move seam | `engine/source/saffron/control/control_commands_physics.cpp` | `move-character` |
