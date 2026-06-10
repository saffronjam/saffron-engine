module;

// Same global-module-fragment shape as the interface unit: glm + entt via classic
// includes, no `import std`.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
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

        // A bone's authored rest local TRS, read from its TransformComponent (Euler ->
        // quat, matching transformMatrix). Identity if the handle is stale.
        auto restPoseOf(Scene& scene, const SkinnedMeshComponent& skin, std::size_t i) -> JointPose
        {
            JointPose rest;
            if (i >= skin.boneHandles.size())
            {
                return rest;
            }
            const Entity bone{ skin.boneHandles[i] };
            if (!valid(scene, bone) || !hasComponent<TransformComponent>(scene, bone))
            {
                return rest;
            }
            const TransformComponent& transform = getComponent<TransformComponent>(scene, bone);
            rest.translation = transform.translation;
            rest.rotation = glm::quat(transform.rotation);
            rest.scale = transform.scale;
            return rest;
        }

        // Drop every pose override on a rig's bones so they revert to the rest pose.
        void clearOverrides(Scene& scene, const SkinnedMeshComponent& skin)
        {
            for (const entt::entity handle : skin.boneHandles)
            {
                if (handle != entt::null && scene.registry.valid(handle))
                {
                    scene.registry.remove<PoseOverrideComponent>(handle);
                }
            }
        }

        // Resolve (and cache) a clip Uuid to its loaded AnimClip. A broken asset is
        // negative-cached as an empty clip so it is not re-read every frame.
        auto loadClip(AnimationRuntime& runtime, const AssetCatalog& catalog, std::string_view assetRoot, Uuid clip)
            -> const AnimClip*
        {
            if (clip.value == 0)
            {
                return nullptr;
            }
            if (auto it = runtime.clipCache.find(clip.value); it != runtime.clipCache.end())
            {
                return &it->second;
            }
            const AssetEntry* entry = findAsset(catalog, clip);
            if (entry == nullptr || entry->type != AssetType::Animation)
            {
                return nullptr;
            }
            const std::string path = std::string(assetRoot) + "/" + entry->path;
            auto loaded = loadAnimation(path);
            if (!loaded)
            {
                logWarn(std::format("animation: clip {} failed to load ('{}'): {}", clip.value, path, loaded.error()));
                return &runtime.clipCache.emplace(clip.value, AnimClip{}).first->second;
            }
            return &runtime.clipCache.emplace(clip.value, std::move(*loaded)).first->second;
        }

        // Advance the playhead by dt*speed under the wrap mode. Once clamps + stops at an
        // end; Loop wraps; PingPong bounces and flips direction.
        void advanceTime(AnimationPlayerComponent& player, f32 duration, f32 dt)
        {
            const f32 delta = dt * player.speed;
            if (player.wrap == AnimationPlayerComponent::Wrap::PingPong)
            {
                player.time = player.time + (player.pingForward ? delta : -delta);
                if (player.time >= duration)
                {
                    player.time = 2.0f * duration - player.time;
                    player.pingForward = false;
                }
                if (player.time <= 0.0f)
                {
                    player.time = -player.time;
                    player.pingForward = true;
                }
                player.time = glm::clamp(player.time, 0.0f, duration);
                return;
            }
            player.time = player.time + delta;
            if (player.wrap == AnimationPlayerComponent::Wrap::Loop)
            {
                player.time = std::fmod(player.time, duration);
                if (player.time < 0.0f)
                {
                    player.time = player.time + duration;
                }
                return;
            }
            if (player.time >= duration)
            {
                player.time = duration;
                player.playing = false;
            }
            else if (player.time < 0.0f)
            {
                player.time = 0.0f;
                player.playing = false;
            }
        }

        // Per-joint blend of the animated local pose with an external producer's pose
        // (IK/physics). Inert in v1 (all weights 0); the seam phase 13 fills.
        auto blendJoint(const JointPose& base, const JointPose& over, f32 weight) -> JointPose
        {
            JointPose out;
            out.translation = glm::mix(base.translation, over.translation, weight);
            out.rotation = glm::normalize(glm::slerp(base.rotation, over.rotation, weight));
            out.scale = glm::mix(base.scale, over.scale, weight);
            return out;
        }

        // sampleClip, but each track is bound to its joint by index when sound and re-resolved
        // by the durable node name when the index is stale (out of range or names disagree).
        void sampleClipResolved(const AnimClip& clip, f32 t, const std::vector<std::string>& boneNames,
                                const std::unordered_map<std::string, i32>& nameToIndex, PoseBuffer& out)
        {
            const auto jointCount = static_cast<i32>(out.local.size());
            for (const AnimTrack& track : clip.tracks)
            {
                i32 joint = track.joint;
                const bool stale = joint < 0 || joint >= jointCount ||
                                   (!track.jointName.empty() && static_cast<std::size_t>(joint) < boneNames.size() &&
                                    boneNames[static_cast<std::size_t>(joint)] != track.jointName);
                if (stale)
                {
                    auto it = nameToIndex.find(track.jointName);
                    joint = it != nameToIndex.end() ? it->second : -1;
                }
                if (joint < 0 || joint >= jointCount)
                {
                    continue;
                }
                const glm::vec4 v = sampleTrack(track, t);
                const auto j = static_cast<std::size_t>(joint);
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

    void tickAnimation(AnimationRuntime& runtime, Scene& scene, const AssetCatalog& catalog, std::string_view assetRoot,
                       f32 dt, AnimMode mode)
    {
        forEach<AnimationPlayerComponent, SkinnedMeshComponent>(
            scene,
            [&](Entity, AnimationPlayerComponent& player, SkinnedMeshComponent& skin)
            {
                // Play animates every rig; Edit previews only the timeline-selected one.
                const bool active = mode == AnimMode::Play || player.previewInEdit;
                const AnimClip* clip = active ? loadClip(runtime, catalog, assetRoot, player.clip) : nullptr;
                if (clip == nullptr)
                {
                    clearOverrides(scene, skin);
                    return;
                }
                if (player.playing && clip->duration > 0.0f)
                {
                    advanceTime(player, clip->duration, dt);
                }

                // Seed the pose with each bone's rest local TRS so untracked joints (and
                // untracked channels of a tracked joint) keep their authored value, and
                // collect the name<->index maps for durable track resolution.
                const std::size_t jointCount = skin.bones.size();
                PoseBuffer pose;
                pose.local.resize(jointCount);
                std::vector<std::string> boneNames(jointCount);
                std::unordered_map<std::string, i32> nameToIndex;
                for (std::size_t i = 0; i < jointCount; i = i + 1)
                {
                    pose.local[i] = restPoseOf(scene, skin, i);
                    if (i < skin.boneHandles.size())
                    {
                        const Entity bone{ skin.boneHandles[i] };
                        if (valid(scene, bone) && hasComponent<NameComponent>(scene, bone))
                        {
                            boneNames[i] = getComponent<NameComponent>(scene, bone).name;
                            nameToIndex.emplace(boneNames[i], static_cast<i32>(i));
                        }
                    }
                }
                sampleClipResolved(*clip, player.time, boneNames, nameToIndex, pose);

                // Write the blended local pose onto each bone as a runtime override. The
                // blend layer is inert in v1 (all weights 0 -> final == sampled local).
                for (std::size_t i = 0; i < jointCount; i = i + 1)
                {
                    JointPose finalPose = pose.local[i];
                    if (i < pose.weight.size() && i < pose.override_.size() && pose.weight[i] > 0.0f)
                    {
                        finalPose = blendJoint(pose.local[i], pose.override_[i], pose.weight[i]);
                    }
                    if (i >= skin.boneHandles.size())
                    {
                        continue;
                    }
                    const entt::entity handle = skin.boneHandles[i];
                    if (handle == entt::null || !scene.registry.valid(handle))
                    {
                        continue;
                    }
                    PoseOverrideComponent& override_ = scene.registry.emplace_or_replace<PoseOverrideComponent>(handle);
                    override_.translation = finalPose.translation;
                    override_.rotation = finalPose.rotation;
                    override_.scale = finalPose.scale;
                }
            });
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

        // .sanim IO round-trip (Phase 2): a two-track clip survives save -> load exactly.
        {
            AnimClip clip;
            clip.name = "io_roundtrip";
            clip.duration = 1.0f;
            AnimTrack rot;
            rot.joint = 1;
            rot.jointName = "Tip";
            rot.path = AnimTrack::Path::Rotation;
            rot.interp = AnimTrack::Interp::Linear;
            rot.times = { 0.0f, 1.0f };
            rot.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
            clip.tracks.push_back(rot);
            AnimTrack trans;
            trans.joint = 0;
            trans.jointName = "Root";
            trans.path = AnimTrack::Path::Translation;
            trans.interp = AnimTrack::Interp::Step;
            trans.times = { 0.0f, 0.5f };
            trans.values = { 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f };
            clip.tracks.push_back(trans);

            const std::string path = "/tmp/saffron_selftest.sanim";
            auto saved = saveAnimation(clip, path);
            expect(saved.has_value(), "sanim save");
            if (saved)
            {
                auto loaded = loadAnimation(path);
                expect(loaded.has_value(), "sanim load");
                if (loaded)
                {
                    expect(loaded->name == clip.name, "sanim name");
                    expect(glm::abs(loaded->duration - clip.duration) < eps, "sanim duration");
                    expect(loaded->tracks.size() == clip.tracks.size(), "sanim track count");
                    bool tracksMatch = loaded->tracks.size() == clip.tracks.size();
                    for (std::size_t i = 0; i < clip.tracks.size() && i < loaded->tracks.size(); i = i + 1)
                    {
                        const AnimTrack& a = clip.tracks[i];
                        const AnimTrack& b = loaded->tracks[i];
                        tracksMatch = tracksMatch && a.joint == b.joint && a.jointName == b.jointName &&
                                      a.path == b.path && a.interp == b.interp && a.times == b.times &&
                                      a.values == b.values;
                    }
                    expect(tracksMatch, "sanim track fields");
                }
            }
        }

        // Evaluator (Phase 3): a previewInEdit rig writes a PoseOverrideComponent that
        // animates the bone and shows through world composition, while the authored
        // rest-pose TransformComponent stays put; clearing the preview reverts it.
        {
            const f32 s = std::sqrt(0.5f);
            Scene scene;
            Entity rootBone = createEntity(scene, "Root");
            Entity tipBone = createEntity(scene, "Tip");
            Entity meshEntity = createEntity(scene, "Rig");
            SkinnedMeshComponent& skin = addComponent<SkinnedMeshComponent>(scene, meshEntity);
            skin.bones = { getComponent<IdComponent>(scene, rootBone).id,
                           getComponent<IdComponent>(scene, tipBone).id };
            skin.boneHandles = { rootBone.handle, tipBone.handle };
            AnimationPlayerComponent& player = addComponent<AnimationPlayerComponent>(scene, meshEntity);
            player.clip = Uuid{ 1234 };
            player.wrap = AnimationPlayerComponent::Wrap::Loop;

            // Seed the clip cache directly (bypass the catalog/disk): joint 0 rotates 90 deg
            // about Y over 1s, LINEAR.
            AnimationRuntime runtime;
            AnimClip clip;
            clip.name = "spin";
            clip.duration = 1.0f;
            AnimTrack rot;
            rot.joint = 0;
            rot.jointName = "Root";
            rot.path = AnimTrack::Path::Rotation;
            rot.interp = AnimTrack::Interp::Linear;
            rot.times = { 0.0f, 1.0f };
            rot.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s, 0.0f, s };
            clip.tracks.push_back(rot);
            runtime.clipCache.emplace(player.clip.value, clip);
            const AssetCatalog catalog;

            const glm::quat q45 = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));

            // Edit, no preview: nothing animates, no override appears.
            tickAnimation(runtime, scene, catalog, "", 0.5f, AnimMode::Edit);
            expect(!scene.registry.all_of<PoseOverrideComponent>(rootBone.handle), "edit without preview is inert");

            // Edit + preview + playing: the bone is driven to the 45 deg midpoint.
            player.previewInEdit = true;
            player.playing = true;
            player.time = 0.0f;
            tickAnimation(runtime, scene, catalog, "", 0.5f, AnimMode::Edit);
            expect(glm::abs(player.time - 0.5f) < eps, "edit preview advances the playhead");
            const auto* over = scene.registry.try_get<PoseOverrideComponent>(rootBone.handle);
            expect(over != nullptr, "preview writes a pose override");
            if (over != nullptr)
            {
                expect(quatClose(over->rotation, q45), "override holds the sampled rotation");
            }
            // Rest pose untouched (non-destructive), so preview never dirties the project.
            expect(glm::length(getComponent<TransformComponent>(scene, rootBone).rotation) < eps,
                   "rest-pose TransformComponent stays at identity");
            // World composition prefers the override.
            updateWorldTransforms(scene);
            expect(quatClose(worldRotation(scene, rootBone), q45), "world transform reflects the override");

            // Clearing the preview reverts the bone to rest next tick.
            player.previewInEdit = false;
            tickAnimation(runtime, scene, catalog, "", 0.0f, AnimMode::Edit);
            expect(!scene.registry.all_of<PoseOverrideComponent>(rootBone.handle),
                   "clearing preview removes the override");
            updateWorldTransforms(scene);
            expect(quatClose(worldRotation(scene, rootBone), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)),
                   "bone reverts to rest after preview clears");

            // Play animates every rig regardless of previewInEdit.
            player.previewInEdit = false;
            player.playing = true;
            player.time = 0.0f;
            tickAnimation(runtime, scene, catalog, "", 0.5f, AnimMode::Play);
            expect(scene.registry.all_of<PoseOverrideComponent>(rootBone.handle), "play animates without preview");
        }

        if (failures != 0)
        {
            return Err(std::format("animation self-test: {} check(s) failed", failures));
        }
        logInfo("animation self-test: all checks passed");
        return {};
    }
}
