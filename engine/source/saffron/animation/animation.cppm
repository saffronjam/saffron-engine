module;

// glm is a header-heavy C++ library, so this module uses classic includes
// (no `import std`), like the geometry/scene modules.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <expected>
#include <string>
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

    /// Headless unit gate (mirrors the jointMatrices self-test): samples known STEP/
    /// LINEAR/CUBICSPLINE keys and asserts endpoints are exact and midpoints match
    /// (slerp midpoint for rotation). An Err means the sampling math is broken.
    auto runAnimationSelfTest() -> Result<void>;
}
