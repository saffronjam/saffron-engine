# Phase 1 — `Saffron.Animation` module + pose/clip types + samplers

**Status:** COMPLETED

## Goal

Stand up a new **`Saffron.Animation`** module and the pure data + math layer it owns: the pose
representation (`Pose`/`PoseBuffer`), the runtime clip types (`Clip`/`Track`), and clip **sampling**
(all three glTF interpolation modes, with slerp on rotations). Nothing in the engine imports this module
yet — it builds in isolation and the sampling math is unit-testable in a self-test, exactly like the
`jointMatrices` self-test pattern. Until this is green, do not start Phase 3 (Phase 2 only needs the
`Clip`/`Track` types, which live in Geometry — see below).

## What exists to build on

- Engine modules are one flat static lib: interface units in the `CXX_MODULES` file set
  (`engine/CMakeLists.txt:4-26`), impl `.cpp` units as `PRIVATE` sources (`:30-48`); `cxx_std_26` +
  `CXX_MODULE_STD ON` already set on the target (`:50-51`).
- The DAG (`AGENTS.md`): `Geometry → Core`, `Scene → {Core, Json}`. Modules that wrap heavy C++ headers
  (`scene`, `geometry`) use classic `#include` in the global module fragment and **do NOT `import std`**
  (`scene.cppm:1-23` is the template). `Saffron.Animation` manipulates `glm` (quat slerp, mat4 compose) —
  a heavy C++ header — so it **follows that pattern**.
- `glm` is already vendored and used throughout `geometry`/`scene` (`glm::vec3`, `glm::quat`, `glm::mat4`).
- The fallible-call idiom: `Result<T> = std::expected<T, std::string>` returned from anything that can
  fail (`core.cppm`). Math helpers that cannot fail return plain values.
- **Where the clip file type lives:** `Saffron.Geometry` owns mesh/file formats (`Vertex`, `VertexSkin`,
  `Mesh`, `saveMesh*`/`loadMesh*`). The **runtime clip data type + `.sanim` IO belong in Geometry**
  (Phase 2 adds the IO); this phase defines the types there so both Geometry (import/save) and Animation
  (sampling) share one definition without a circular edge.

## Work

### 1. Define the clip data types in `Saffron.Geometry`

In `geometry.cppm`, alongside `VertexSkin` (`:58`), add the keyframe-track model — a faithful, lossless
mirror of a glTF animation channel:

```cpp
/// One animated joint channel: a sampled curve targeting a joint's translation, rotation, or scale.
struct AnimTrack {
    i32 joint = -1;                 // stable index into SkinnedMeshComponent.bones (resolved at import by name)
    std::string jointName;          // the glTF node name — the durable binding key (survives reorder/reimport)
    enum class Path : u8 { Translation, Rotation, Scale } path = Path::Translation;
    enum class Interp : u8 { Step, Linear, CubicSpline } interp = Interp::Linear;
    std::vector<f32> times;         // sampler.input — strictly increasing, seconds
    std::vector<f32> values;        // sampler.output — flat: vec3 per key (T/S) or quat xyzw per key (R);
                                    //   CubicSpline stores 3x (in-tangent, value, out-tangent) per key
};

/// A named animation clip: a bundle of joint tracks with a total duration.
struct AnimClip {
    std::string name;
    f32 duration = 0.0f;            // max track end time, seconds
    std::vector<AnimTrack> tracks;
};
```

These are POD-ish and serializable; Phase 2 adds `saveAnimation`/`loadAnimation` (`.sanim`) next to
`saveMeshSkinned`.

### 2. Create the `Saffron.Animation` module

New files `engine/source/saffron/animation/animation.cppm` (interface) and `animation.cpp` (impl), wired
into `engine/CMakeLists.txt` (interface in the `CXX_MODULES` set `:4-26`, impl in `PRIVATE` `:30-48`).
Module declaration follows `scene.cppm:1-23` exactly — classic `#include <glm/...>` in the global module
fragment, **no `import std`**, `export module Saffron.Animation;`, `import Saffron.Core; import
Saffron.Geometry; import Saffron.Scene;`.

> Scene is imported for `SkinnedMeshComponent`/`Entity`/`forEach` used by the Phase-3 evaluator; this
> phase only needs Core + Geometry, but declaring the full edge now avoids churn.

### 3. Pose representation

```cpp
/// A single joint's local transform, decomposed (the form clips sample into and blends operate on).
struct JointPose { glm::vec3 translation{0.0f}; glm::quat rotation{1,0,0,0}; glm::vec3 scale{1.0f}; };

/// A skeleton-sized pose, indexed 1:1 with SkinnedMeshComponent.bones.
/// `weight` is the inert per-bone blend layer (Phase 3 writes it; v1 leaves it 0 = pure animation).
struct PoseBuffer {
    std::vector<JointPose> local;   // the sampled/animated local TRS per joint
    std::vector<JointPose> override_;// external producers (IK/physics) write here (Phase 13+); empty in v1
    std::vector<f32> weight;        // 0 = use `local`, 1 = use `override_`; per joint
};
```

### 4. Sampling

Exported, pure, allocation-light:

```cpp
/// Sample one track at time t into a JointPose field (T, R, or S). Binary-search the cursor;
/// STEP = hold previous key; LINEAR = lerp (slerp for rotation); CUBICSPLINE = Hermite with dt-scaled tangents.
auto sampleTrack(const AnimTrack& track, f32 t) -> glm::vec4;   // vec3 in .xyz (T/S) or quat xyzw (R)

/// Sample a whole clip at time t into `out` (sized to joint count). Joints with no track keep bind/rest.
void sampleClip(const AnimClip& clip, f32 t, PoseBuffer& out);
```

Implementation notes (the glTF-spec correctness points, all from the research):
- **Rotation uses `glm::slerp`** between adjacent quaternion keys (LINEAR) — never component lerp; and
  **normalize** the result. CubicSpline rotation interpolates the quat components then normalizes.
- **CubicSpline** output is `[inTangent, value, outTangent]` per key (3× stride); the Hermite basis uses
  `t_local = (t - t0)/(t1 - t0)` and scales tangents by `(t1 - t0)`.
- **Clamp** before the first key / after the last (no extrapolation).
- Cursor cache: a track is sampled monotonically during playback, so keep a hint index to avoid a full
  binary search each frame (optional in v1; correctness first).

### 5. A self-test gate (mirrors the `jointMatrices` self-test)

Add an internal `animationSelfTest()` invoked once at module init (or behind a debug flag like the
existing scene self-test at `scene.cppm:1392`): build a tiny 1-joint `AnimClip` with known LINEAR/STEP/
CUBICSPLINE keys and assert `sampleClip` reproduces the endpoints exactly and the midpoint within epsilon
(slerp midpoint for rotation). This is the unit test the headless build runs.

### 6. Update the module DAG in `AGENTS.md`

Add `Animation → {Core, Geometry, Scene}` to the DAG block and to the `Host → {…}` edge list (Host gains
it in Phase 3). Note in the module list that `animation` uses classic `#include` + does NOT `import std`.

## Validation (done criteria)

- `make engine` green with the new module compiling in isolation (nothing imports it yet).
- `make prepare-for-commit` clean (clang-format + clang-tidy) for the new files.
- `animationSelfTest()` passes (endpoints exact, slerp midpoint within epsilon) under a headless
  `SAFFRON_EXIT_AFTER_FRAMES=1` run with the self-test enabled.
- `docs/`: add `docs/content/explanations/animation/_index.md` hub + an "animation data model" page
  (concept: clip = tracks of keyframes, pose = local TRS, the blend layer); add the hub row.

## Notes / gotchas

- `glm::quat` constructor order is `(w, x, y, z)` but glTF stores `[x, y, z, w]`; the existing skin import
  already swaps for the bind matrices (`geometry.cppm:566`) — `sampleTrack` for rotation must read the
  flat `values` as `xyzw` and construct accordingly.
- Keep `Saffron.Animation` free of any entt/Scene mutation in this phase — it is pure types + math here;
  the evaluator that touches the scene is Phase 3. This keeps the unit test trivial and the module leaf-like.
