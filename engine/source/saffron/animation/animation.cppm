module;

// glm is a header-heavy C++ library, so this module uses classic includes
// (no `import std`), like the geometry/scene modules.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module Saffron.Animation;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Scene;

export namespace se
{
    /// A single joint's local transform, decomposed — the form clips sample into and
    /// the blend layer operates on. Rotation is a unit quaternion (w, x, y, z).
    struct JointPose
    {
        glm::vec3 translation{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 scale{ 1.0f };
    };

    /// A skeleton-sized pose, indexed 1:1 with SkinnedMeshComponent.bones. `local` is
    /// the sampled/animated TRS; `override_` is where external producers (IK/physics)
    /// write (Phase 13+); `weight` is the inert per-bone blend layer (v1 leaves it 0,
    /// meaning pure animation). `override_`/`weight` are empty/zero until a producer fills them.
    struct PoseBuffer
    {
        std::vector<JointPose> local;
        std::vector<JointPose> override_;
        std::vector<f32> weight;
    };

    /// Sample one track at time t. Returns a vec3 in .xyz for Translation/Scale, or a
    /// normalized quaternion as xyzw for Rotation. STEP holds the previous key, LINEAR
    /// lerps (slerp for rotation), CUBICSPLINE is a Hermite spline with dt-scaled
    /// tangents; t is clamped to [first key, last key] (no extrapolation).
    auto sampleTrack(const AnimTrack& track, f32 t) -> glm::vec4;

    /// Sample a whole clip at time t into `out.local` (sized to the joint count by the
    /// caller and pre-filled with the rest pose). Only joints with a track are written;
    /// joints with no track keep their bind/rest value.
    void sampleClip(const AnimClip& clip, f32 t, PoseBuffer& out);

    /// Edit previews a single rig's clip non-destructively; Play advances every rig.
    enum class AnimMode : u8
    {
        Edit,
        Play,
    };

    /// Per-session animation state: clip Uuid -> loaded AnimClip. The Host owns one and
    /// clears it on project (re)load so a reimported clip is picked up fresh.
    struct AnimationRuntime
    {
        std::unordered_map<u64, AnimClip> clipCache;
    };

    /// Sample and (when playing) advance every AnimationPlayerComponent on a SkinnedMesh
    /// rig, writing a PoseOverrideComponent onto each driven bone — and removing it from an
    /// inactive rig's bones so they fall back to the authored rest pose. In Play every rig
    /// animates; in Edit only a `previewInEdit` rig does. `catalog` + `assetRoot` resolve a
    /// clip Uuid to its `.sanim` (loaded once into `runtime`). Never writes a bone's
    /// TransformComponent, so the rest pose and the project's dirty state stay untouched.
    void tickAnimation(AnimationRuntime& runtime, Scene& scene, const AssetCatalog& catalog, std::string_view assetRoot,
                       f32 dt, AnimMode mode);

    /// Headless unit gate (mirrors the jointMatrices self-test): samples known STEP/
    /// LINEAR/CUBICSPLINE keys and asserts endpoints are exact and midpoints match
    /// (slerp midpoint for rotation). An Err means the sampling math is broken.
    auto runAnimationSelfTest() -> Result<void>;
}
