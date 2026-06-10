module;

// Same global-module-fragment shape as the interface unit: glm via classic
// includes, no `import std`.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>

module Saffron.Animation;

import Saffron.Core;
import Saffron.Geometry;

namespace se
{
    namespace
    {
        auto asQuat(glm::vec4 v) -> glm::quat
        {
            return { v.w, v.x, v.y, v.z };
        }

        auto fromQuat(glm::quat q) -> glm::vec4
        {
            return { q.x, q.y, q.z, q.w };
        }
    }

    auto sampleTrack(const AnimTrack& track, f32 t) -> glm::vec4
    {
        const bool rotation = track.path == AnimTrack::Path::Rotation;
        const int cc = rotation ? 4 : 3;
        const auto stride = static_cast<std::size_t>(cc);
        const std::vector<f32>& times = track.times;
        const std::size_t n = times.size();

        if (n == 0 || track.values.empty())
        {
            if (rotation)
            {
                return { 0.0f, 0.0f, 0.0f, 1.0f };
            }
            if (track.path == AnimTrack::Path::Scale)
            {
                return { 1.0f, 1.0f, 1.0f, 0.0f };
            }
            return glm::vec4(0.0f);
        }

        // CubicSpline stores [in-tangent, value, out-tangent] per key (3x stride); the
        // sampled value sits one stride in. STEP/LINEAR store the value flat.
        auto valueOffset = [&](std::size_t key) -> std::size_t
        {
            if (track.interp == AnimTrack::Interp::CubicSpline)
            {
                return key * 3 * stride + stride;
            }
            return key * stride;
        };
        auto readVec4 = [&](std::size_t offset) -> glm::vec4
        {
            glm::vec4 r(0.0f);
            for (int c = 0; c < cc; c = c + 1)
            {
                r[c] = track.values[offset + static_cast<std::size_t>(c)];
            }
            return r;
        };
        auto finish = [&](glm::vec4 v) -> glm::vec4
        {
            if (rotation)
            {
                return fromQuat(glm::normalize(asQuat(v)));
            }
            return v;
        };

        if (t <= times.front())
        {
            return finish(readVec4(valueOffset(0)));
        }
        if (t >= times.back())
        {
            return finish(readVec4(valueOffset(n - 1)));
        }

        const auto upper = std::ranges::upper_bound(times, t);
        const std::size_t i1 = static_cast<std::size_t>(upper - times.begin());
        const std::size_t i0 = i1 - 1;
        const f32 dt = times[i1] - times[i0];
        f32 local = 0.0f;
        if (dt > 0.0f)
        {
            local = (t - times[i0]) / dt;
        }

        if (track.interp == AnimTrack::Interp::Step)
        {
            return finish(readVec4(valueOffset(i0)));
        }

        if (track.interp == AnimTrack::Interp::Linear)
        {
            if (rotation)
            {
                const glm::quat a = glm::normalize(asQuat(readVec4(valueOffset(i0))));
                const glm::quat b = glm::normalize(asQuat(readVec4(valueOffset(i1))));
                return fromQuat(glm::normalize(glm::slerp(a, b, local)));
            }
            return glm::mix(readVec4(valueOffset(i0)), readVec4(valueOffset(i1)), local);
        }

        const f32 t2 = local * local;
        const f32 t3 = t2 * local;
        const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const f32 h10 = t3 - 2.0f * t2 + local;
        const f32 h01 = -2.0f * t3 + 3.0f * t2;
        const f32 h11 = t3 - t2;
        const glm::vec4 p0 = readVec4(i0 * 3 * stride + stride);
        const glm::vec4 p1 = readVec4(i1 * 3 * stride + stride);
        const glm::vec4 m0 = readVec4(i0 * 3 * stride + 2 * stride) * dt;
        const glm::vec4 m1 = readVec4(i1 * 3 * stride) * dt;
        return finish(h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1);
    }

    void sampleClip(const AnimClip& clip, f32 t, PoseBuffer& out)
    {
        for (const AnimTrack& track : clip.tracks)
        {
            if (track.joint < 0)
            {
                continue;
            }
            const auto j = static_cast<std::size_t>(track.joint);
            if (j >= out.local.size())
            {
                continue;
            }
            const glm::vec4 v = sampleTrack(track, t);
            switch (track.path)
            {
            case AnimTrack::Path::Translation:
                out.local[j].translation = glm::vec3(v);
                break;
            case AnimTrack::Path::Rotation:
                out.local[j].rotation = asQuat(v);
                break;
            case AnimTrack::Path::Scale:
                out.local[j].scale = glm::vec3(v);
                break;
            }
        }
    }

    auto runAnimationSelfTest() -> Result<void>
    {
        u32 failures = 0;
        auto expect = [&failures](bool condition, std::string_view what)
        {
            if (!condition)
            {
                failures = failures + 1;
                logError(std::format("animation self-test failed: {}", what));
            }
        };
        auto quatClose = [](glm::quat a, glm::quat b) -> bool
        {
            // Quaternions double-cover rotations, so q and -q are the same orientation.
            return glm::abs(glm::dot(a, b)) > 1.0f - 1e-4f;
        };
        constexpr f32 eps = 1e-4f;

        // LINEAR translation: endpoints exact, midpoint lerps, t clamps past the ends.
        {
            AnimTrack track;
            track.path = AnimTrack::Path::Translation;
            track.interp = AnimTrack::Interp::Linear;
            track.times = { 0.0f, 2.0f };
            track.values = { 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f };
            expect(glm::distance(glm::vec3(sampleTrack(track, 0.0f)), glm::vec3(0.0f)) < eps, "linear T start");
            expect(glm::distance(glm::vec3(sampleTrack(track, 2.0f)), glm::vec3(10.0f, 0.0f, 0.0f)) < eps,
                   "linear T end");
            expect(glm::distance(glm::vec3(sampleTrack(track, 1.0f)), glm::vec3(5.0f, 0.0f, 0.0f)) < eps,
                   "linear T mid");
            expect(glm::distance(glm::vec3(sampleTrack(track, -1.0f)), glm::vec3(0.0f)) < eps, "linear T clamp low");
            expect(glm::distance(glm::vec3(sampleTrack(track, 9.0f)), glm::vec3(10.0f, 0.0f, 0.0f)) < eps,
                   "linear T clamp high");
        }

        // STEP scale: holds the previous key until the next key's time.
        {
            AnimTrack track;
            track.path = AnimTrack::Path::Scale;
            track.interp = AnimTrack::Interp::Step;
            track.times = { 0.0f, 1.0f };
            track.values = { 1.0f, 1.0f, 1.0f, 3.0f, 3.0f, 3.0f };
            expect(glm::distance(glm::vec3(sampleTrack(track, 0.9f)), glm::vec3(1.0f)) < eps, "step S holds key0");
            expect(glm::distance(glm::vec3(sampleTrack(track, 1.0f)), glm::vec3(3.0f)) < eps, "step S at next key");
        }

        // CUBICSPLINE translation: endpoints exact; asymmetric tangents bend the midpoint
        // to 0.75 (distinct from the linear 0.5), proving the Hermite path runs.
        {
            AnimTrack track;
            track.path = AnimTrack::Path::Translation;
            track.interp = AnimTrack::Interp::CubicSpline;
            track.times = { 0.0f, 1.0f };
            track.values = {
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,  // key0: in, value, out
                0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  // key1: in, value, out
            };
            expect(glm::distance(glm::vec3(sampleTrack(track, 0.0f)), glm::vec3(0.0f)) < eps, "cubic T start");
            expect(glm::distance(glm::vec3(sampleTrack(track, 1.0f)), glm::vec3(1.0f, 0.0f, 0.0f)) < eps,
                   "cubic T end");
            expect(glm::abs(sampleTrack(track, 0.5f).x - 0.75f) < eps, "cubic T mid");
        }

        // LINEAR rotation = slerp: 0 deg -> 90 deg about Y, midpoint is exactly 45 deg.
        {
            const f32 s = std::sqrt(0.5f);
            AnimTrack track;
            track.path = AnimTrack::Path::Rotation;
            track.interp = AnimTrack::Interp::Linear;
            track.times = { 0.0f, 1.0f };
            track.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s, 0.0f, s };  // xyzw: identity, 90 deg Y
            const glm::quat q0(1.0f, 0.0f, 0.0f, 0.0f);
            const glm::quat q90 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::quat q45 = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            expect(quatClose(asQuat(sampleTrack(track, 0.0f)), q0), "slerp R start");
            expect(quatClose(asQuat(sampleTrack(track, 1.0f)), q90), "slerp R end");
            expect(quatClose(asQuat(sampleTrack(track, 0.5f)), q45), "slerp R mid");
        }

        // sampleClip integration on joint 0 (T cubic, R slerp, S step); an untracked
        // joint keeps its pre-filled rest value.
        {
            AnimClip clip;
            clip.name = "selftest";
            clip.duration = 1.0f;

            AnimTrack t;
            t.joint = 0;
            t.path = AnimTrack::Path::Translation;
            t.interp = AnimTrack::Interp::CubicSpline;
            t.times = { 0.0f, 1.0f };
            t.values = {
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            };
            clip.tracks.push_back(t);

            const f32 s = std::sqrt(0.5f);
            AnimTrack r;
            r.joint = 0;
            r.path = AnimTrack::Path::Rotation;
            r.interp = AnimTrack::Interp::Linear;
            r.times = { 0.0f, 1.0f };
            r.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s, 0.0f, s };
            clip.tracks.push_back(r);

            AnimTrack sc;
            sc.joint = 0;
            sc.path = AnimTrack::Path::Scale;
            sc.interp = AnimTrack::Interp::Step;
            sc.times = { 0.0f, 1.0f };
            sc.values = { 1.0f, 1.0f, 1.0f, 3.0f, 3.0f, 3.0f };
            clip.tracks.push_back(sc);

            PoseBuffer pose;
            pose.local.resize(2);
            pose.local[0].translation = glm::vec3(99.0f);             // overwritten by the T track
            pose.local[1].translation = glm::vec3(7.0f, 8.0f, 9.0f);  // untracked rest sentinel
            sampleClip(clip, 0.5f, pose);

            expect(glm::abs(pose.local[0].translation.x - 0.75f) < eps, "clip T cubic mid");
            const glm::quat q45 = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            expect(quatClose(pose.local[0].rotation, q45), "clip R slerp mid");
            expect(glm::distance(pose.local[0].scale, glm::vec3(1.0f)) < eps, "clip S step holds");
            expect(glm::distance(pose.local[1].translation, glm::vec3(7.0f, 8.0f, 9.0f)) < eps,
                   "untracked joint keeps rest");
        }

        if (failures != 0)
        {
            return Err(std::format("animation self-test: {} check(s) failed", failures));
        }
        logInfo("animation self-test: all checks passed");
        return {};
    }
}
