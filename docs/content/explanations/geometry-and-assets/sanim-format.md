+++
title = '.sanim format'
weight = 4
+++

# .sanim format

`.sanim` is a baked binary animation clip: a 32-byte header, the clip name, then one
self-describing record per joint track. Each glTF animation is decoded once on import and
written to its own `.sanim` sidecar; the player reads it back directly at runtime.

A clip is strictly a *sidecar*. It is never folded into the [`.smesh`](../smesh-format/) — the
mesh format and its version stay untouched, and a rig with no clips bakes exactly as before.
The two share a shape (a fixed header, raw little-endian arrays, a version field, a defensive
loader) but carry different magic so neither can be mistaken for the other.

## Layout

A fixed 32-byte header, then the clip name bytes, then per track a 20-byte record followed by
the track's joint name, times, and values:

```cpp
struct SANimHeader
{
    char magic[4];     // 'S','A','N','M'
    u32 version;
    u32 trackCount;
    f32 duration;      // clip length, seconds
    u32 nameLen;       // clip-name bytes that follow the header
    u32 reserved[3];
};
static_assert(sizeof(SANimHeader) == 32);

struct SANimTrackRecord       // joint name, times, then values follow it
{
    i32 joint;                // index into SkinnedMeshComponent.bones
    u8  path;                 // AnimTrack::Path  (Translation/Rotation/Scale)
    u8  interp;               // AnimTrack::Interp (Step/Linear/CubicSpline)
    u16 pad;
    u32 nameLen;              // the glTF node name (the durable binding key)
    u32 timeCount;            // keyframe count
    u32 valueCount;           // flat float count (see the track model)
};
static_assert(sizeof(SANimTrackRecord) == 20);
```

The `times` and `values` arrays are written as raw `f32` blobs, in the exact flat layout the
sampler reads — `vec3` per key for translation/scale, `xyzw` per key for rotation, and the
`3×(in-tangent, value, out-tangent)` stride for cubic-spline tracks. See the
[animation data model](../../animation/animation-data-model/) for what those arrays mean.

## Binding a track to a joint

A track carries **both** a joint index (its position in `SkinnedMeshComponent.bones`, fast)
and the source node name (durable). The index is the source-of-truth glTF joint order fixed at
import; the name survives a reorder or reimport, so a later evaluator can re-resolve a stale
index by name. Both are written so neither binding is lost.

## Loading defensively

`loadAnimation` reads the whole file once, then walks it with a bounds-checked cursor: every
field — the clip name, each track record, and each track's name/times/values — is taken only
if that many bytes remain. A malformed `timeCount` or `valueCount` can never drive a giant
`resize()`, the same defence [`loadMesh`](../smesh-format/) applies. A short or truncated file
returns an `Err` rather than reading past the buffer.

## Self-test

`runGeometrySelfTest` imports the rigged `animated-strip.gltf` fixture, confirms it yields a
skin plus at least one decoded clip, and round-trips that clip through `saveAnimation` /
`loadAnimation`. The animation module's `runAnimationSelfTest` separately round-trips a
synthetic two-track clip and asserts every field survives byte-for-byte.

## In the code

| What | File | Symbols |
|---|---|---|
| Header + track record | `geometry.cppm` | `SANimHeader`, `SANimTrackRecord` |
| Version constant | `geometry.cppm` | `AnimFormatVersion` |
| Write path | `geometry.cppm` | `saveAnimation` |
| Defensive load | `geometry.cppm` | `loadAnimation` |
| Clip decode on import | `geometry.cppm` | `importGltfModel` |
| Catalog registration | `assets.cppm` | `importModel` |

> [!NOTE]
> Clips are sidecars by design: importing a rig writes one `.sanim` per glTF animation beside
> the `.smesh` and registers an `AssetType::Animation` catalog entry, so the `.smesh` format
> never grows an animation section.

## Related

- [Animation data model](../../animation/animation-data-model/) — the clip/track types this serializes
- [.smesh format](../smesh-format/) — the mesh sidecar it sits beside
- [Model import](../gltf-and-obj-import/) — where glTF animations are decoded
- [Asset server & catalog](../asset-server-and-catalog/) — where the clip entry is registered
