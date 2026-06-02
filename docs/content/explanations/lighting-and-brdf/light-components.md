+++
title = 'Light components'
weight = 1
math = true
+++

# Light components

A light component is an ECS component that carries the radiometric and shape parameters of one
light source. Saffron has three: a directional sun, point lights, and spot lights. Nothing is
baked. Each frame the lights are gathered from the scene, packed into GPU structs, and uploaded
into the lighting set the fragment shader reads.

## The three components

All three are plain value structs in `Saffron.Scene`. Position comes from the entity's
`TransformComponent`; the light itself only carries radiometric and shape parameters.

```cpp
struct DirectionalLightComponent
{
    glm::vec3 direction{ -0.5f, -1.0f, -0.3f };  // the way the light travels
    glm::vec3 color{ 1.0f };
    f32 intensity = 1.0f;
    f32 ambient = 0.15f;
};

struct PointLightComponent  { glm::vec3 color; f32 intensity; f32 range; };

struct SpotLightComponent
{
    glm::vec3 direction{ 0.0f, -1.0f, 0.0f };
    glm::vec3 color{ 1.0f };
    f32 intensity = 5.0f;
    f32 range = 10.0f;
    f32 innerAngle = 20.0f;  // full intensity inside this half-angle (degrees)
    f32 outerAngle = 30.0f;  // zero past this half-angle
};
```

The scene shades through the first directional light it finds and ignores the rest. It is the
only light carrying an `ambient` scalar, which feeds the flat-ambient fallback when
[IBL](../ibl-ambient-term/) is off. Point and spot lights are the punctual lights: a position
and an inverse-square falloff with a hard `range`. A spot adds a cone aimed by `direction`, with
a soft edge between `innerAngle` and `outerAngle`.

## Two GPU shapes

The directional light and the punctual lights take different paths to the GPU because they are
evaluated differently. The directional light is a handful of scalars folded into the lighting
UBO (set 1, binding 0). The punctual lights become a variable-length array, one `GpuLight` per
point or spot light:

```cpp
struct GpuLight
{
    glm::vec4 positionRange;   // xyz = world position, w = range
    glm::vec4 colorIntensity;  // rgb = color, a = intensity
    glm::vec4 directionType;   // xyz = world direction (spot), w = type (0 = point, 1 = spot)
    glm::vec4 spotCos;         // x = cos(innerAngle), y = cos(outerAngle)
};
```

Four `vec4`s keep the struct naturally aligned for std430. A point light leaves `directionType`
zeroed; a spot writes its normalized direction with type `1` in `.w` and pre-computes the
cosines of its two half-angles into `spotCos`. The shader compares against cosines, so converting
degrees to cosine once on the CPU costs less than a `cos` per fragment. `renderScene` builds the
array with two `forEach` loops over the point and spot components.

## The upload

`setSceneLighting` writes the current frame's copies. Writing directly is safe because
`beginFrame` already waited on this frame's fence, so no in-flight frame is reading them.

The punctual array goes into a mapped storage buffer (set 1, binding 1) whose capacity grows to
the next power of two on demand and never shrinks. The same buffer is bound twice, into the
fragment lighting set and into the compute cull set, so growing it rewrites both descriptors.
The directional light and the punctual count land in the `LightUbo`, where `counts.x` is the
count the brute-force loop reads and the other lanes are feature flags
([directional shadow](../directional-light/), IBL, SSAO).

## In the code

| What | File | Symbols |
|---|---|---|
| The components | `scene.cppm` | `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent` |
| Gather + pack | `assets.cppm` | `renderScene` |
| The GPU struct | `renderer_types.cppm` | `GpuLight` |
| The upload | `renderer_lighting.cpp` | `setSceneLighting`, `ensureLightCapacity`, `LightUbo` |

> [!NOTE]
> Only the first directional light shades the scene; extra ones are silently ignored. The light
> buffer grows by powers of two and never shrinks within a session, so a scene that briefly
> spikes to many lights keeps the larger allocation.

## Related

- [Cook-Torrance BRDF](../cook-torrance-brdf/) — the model every one of these runs through
- [Punctual lights and attenuation](../punctual-lights-and-attenuation/) — how `range`, cone, and falloff are evaluated
- [Directional light](../directional-light/) — the shadowed sun term
- [Clustered forward](../clustered-forward/) — what culls the punctual array into froxels
