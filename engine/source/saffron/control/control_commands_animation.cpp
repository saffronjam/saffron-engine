module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;
import Saffron.Scene;
import Saffron.SceneEdit;
import Saffron.Assets;

namespace se
{
    namespace
    {
        using json = nlohmann::json;

        // Resolve an AssetSelector (id-or-name) to an animation catalog entry.
        auto resolveClip(EngineContext& ctx, const AssetSelector& clip) -> Result<const AssetEntry*>
        {
            const json& sel = clip.value;
            const std::string name = sel.is_string() ? sel.get<std::string>() : std::string{};
            u64 byId = 0;
            if (sel.is_number_unsigned())
            {
                byId = sel.get<u64>();
            }
            else if (sel.is_number_integer())
            {
                const i64 v = sel.get<i64>();
                byId = v >= 0 ? static_cast<u64>(v) : 0;
            }
            else
            {
                byId = std::strtoull(name.c_str(), nullptr, 10);
            }
            for (const AssetEntry& entry : ctx.assets.catalog.entries)
            {
                if (entry.type == AssetType::Animation && (entry.id.value == byId || entry.name == name))
                {
                    return &entry;
                }
            }
            return Err(std::format("no animation clip '{}'", name));
        }

        auto wrapName(AnimationPlayerComponent::Wrap wrap) -> std::string
        {
            if (wrap == AnimationPlayerComponent::Wrap::Once)
            {
                return "once";
            }
            if (wrap == AnimationPlayerComponent::Wrap::PingPong)
            {
                return "pingpong";
            }
            return "loop";
        }

        auto wrapFromName(const std::string& name) -> AnimationPlayerComponent::Wrap
        {
            if (name == "once")
            {
                return AnimationPlayerComponent::Wrap::Once;
            }
            if (name == "pingpong")
            {
                return AnimationPlayerComponent::Wrap::PingPong;
            }
            return AnimationPlayerComponent::Wrap::Loop;
        }

        // Resolve the entity and fetch its player, attaching a default one if absent.
        auto playerOf(EngineContext& ctx, const EntitySelector& selector) -> Result<AnimationPlayerComponent*>
        {
            auto entity = resolveEntity(ctx, json{ { "entity", selector.value } });
            if (!entity)
            {
                return Err(entity.error());
            }
            Scene& scene = activeScene(ctx.sceneEdit);
            if (!hasComponent<AnimationPlayerComponent>(scene, *entity))
            {
                addComponent<AnimationPlayerComponent>(scene, *entity);
            }
            return &getComponent<AnimationPlayerComponent>(scene, *entity);
        }

        auto skeletonOverlayState(const SkeletonOverlayOptions& opts) -> SkeletonOverlayResult
        {
            SkeletonOverlayResult out;
            out.show = opts.show;
            out.axes = opts.axes;
            out.jointSize = opts.jointSize;
            return out;
        }

        auto footIkState(const FootIkComponent& ik) -> FootIkResult
        {
            FootIkResult out;
            out.enabled = ik.enabled;
            out.groundHeight = ik.groundHeight;
            out.chains = static_cast<i32>(ik.chains.size());
            return out;
        }

        // Resolve the entity and fetch its foot-IK config, attaching a default one if absent.
        auto footIkOf(EngineContext& ctx, const EntitySelector& selector) -> Result<FootIkComponent*>
        {
            auto entity = resolveEntity(ctx, json{ { "entity", selector.value } });
            if (!entity)
            {
                return Err(entity.error());
            }
            Scene& scene = activeScene(ctx.sceneEdit);
            if (!hasComponent<FootIkComponent>(scene, *entity))
            {
                addComponent<FootIkComponent>(scene, *entity);
            }
            return &getComponent<FootIkComponent>(scene, *entity);
        }

        auto stateOf(EngineContext& ctx, const AnimationPlayerComponent& player) -> AnimationStateResult
        {
            AnimationStateResult out;
            out.clip = WireUuid{ player.clip.value };
            out.clipName = std::string{};
            out.duration = 0.0f;
            if (const AssetEntry* entry = findAsset(ctx.assets.catalog, player.clip); entry != nullptr)
            {
                out.clipName = entry->name;
                out.duration = entry->duration;
            }
            out.time = player.time;
            out.playing = player.playing;
            out.wrap = wrapName(player.wrap);
            out.speed = player.speed;
            out.animationVersion = static_cast<i32>(ctx.sceneEdit.animationVersion);
            return out;
        }
    }

    void registerAnimationCommands(CommandRegistry& reg)
    {
        registerCommand<AnimationStateParams, AnimationStateResult>(
            reg, "get-animation-state", "get-animation-state {entity} — the rig's playhead, clip, wrap, and speed",
            [](EngineContext& ctx, const AnimationStateParams& params) -> Result<AnimationStateResult>
            {
                auto entity = resolveEntity(ctx, json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                if (!hasComponent<AnimationPlayerComponent>(scene, *entity))
                {
                    return Err(std::string{ "entity has no animation player" });
                }
                return stateOf(ctx, getComponent<AnimationPlayerComponent>(scene, *entity));
            });

        registerCommand<ListClipsParams, ListClipsResult>(
            reg, "list-clips", "list-clips {entity} — the animation clips in the project catalog",
            [](EngineContext& ctx, const ListClipsParams&) -> Result<ListClipsResult>
            {
                ListClipsResult out;
                for (const AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    if (entry.type == AssetType::Animation)
                    {
                        out.clips.push_back(AnimationClipDto{ WireUuid{ entry.id.value }, entry.name, entry.duration });
                    }
                }
                return out;
            });

        registerCommand<PlayAnimationParams, AnimationStateResult>(
            reg, "play-animation",
            "play-animation {entity, clip, speed=1, loop=true, blend=0} — play a clip (previews in Edit too)",
            [](EngineContext& ctx, const PlayAnimationParams& params) -> Result<AnimationStateResult>
            {
                auto clip = resolveClip(ctx, params.clip);
                if (!clip)
                {
                    return Err(clip.error());
                }
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                AnimationPlayerComponent& p = **player;
                const f32 blend = params.blend.value_or(0.0f);
                if (blend > 0.0f && p.clip.value != 0 && p.clip.value != (*clip)->id.value)
                {
                    p.prevClip = p.clip;  // cross-fade / inertialize from the current clip
                    p.transition = 0.0f;
                    p.transitionDuration = blend;
                }
                p.clip = (*clip)->id;
                p.time = 0.0f;
                p.speed = params.speed.value_or(1.0f);
                p.wrap = params.loop.value_or(true) ? AnimationPlayerComponent::Wrap::Loop
                                                    : AnimationPlayerComponent::Wrap::Once;
                p.playing = true;
                p.previewInEdit = true;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, p);
            });

        registerCommand<AnimationStateParams, AnimationStateResult>(
            reg, "pause-animation", "pause-animation {entity} — stop advancing time (keeps the pose shown)",
            [](EngineContext& ctx, const AnimationStateParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->playing = false;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<SeekAnimationParams, AnimationStateResult>(
            reg, "seek-animation", "seek-animation {entity, time} — set the playhead (previews in Edit)",
            [](EngineContext& ctx, const SeekAnimationParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->time = params.time;
                (*player)->previewInEdit = true;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<SetAnimationLoopParams, AnimationStateResult>(
            reg, "set-animation-loop", "set-animation-loop {entity, wrap} — once | loop | pingpong",
            [](EngineContext& ctx, const SetAnimationLoopParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->wrap = wrapFromName(params.wrap);
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<AnimationStateParams, AnimationStateResult>(
            reg, "stop-preview", "stop-preview {entity} — clear the Edit preview and stop (revert to rest)",
            [](EngineContext& ctx, const AnimationStateParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->previewInEdit = false;
                (*player)->playing = false;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<EmptyParams, SkeletonOverlayResult>(
            reg, "get-skeleton-overlay",
            "get-skeleton-overlay — the line-skeleton overlay toggle, axes, and joint size",
            [](EngineContext& ctx, const EmptyParams&) -> Result<SkeletonOverlayResult>
            { return skeletonOverlayState(ctx.sceneEdit.skeletonOverlay); });

        registerCommand<SetSkeletonOverlayParams, SkeletonOverlayResult>(
            reg, "set-skeleton-overlay",
            "set-skeleton-overlay {show?, axes?, jointSize?} — the selected rig's line-skeleton viewport overlay",
            [](EngineContext& ctx, const SetSkeletonOverlayParams& params) -> Result<SkeletonOverlayResult>
            {
                SkeletonOverlayOptions& opts = ctx.sceneEdit.skeletonOverlay;
                if (params.show)
                {
                    opts.show = *params.show;
                }
                if (params.axes)
                {
                    opts.axes = *params.axes;
                }
                if (params.jointSize)
                {
                    opts.jointSize = std::max(0.5f, *params.jointSize);
                }
                return skeletonOverlayState(opts);
            });

        registerCommand<GetFootIkParams, FootIkResult>(
            reg, "get-foot-ik", "get-foot-ik {entity} — the rig's foot-IK enable, ground height, and chain count",
            [](EngineContext& ctx, const GetFootIkParams& params) -> Result<FootIkResult>
            {
                auto ik = footIkOf(ctx, params.entity);
                if (!ik)
                {
                    return Err(ik.error());
                }
                return footIkState(**ik);
            });

        registerCommand<SetFootIkParams, FootIkResult>(
            reg, "set-foot-ik", "set-foot-ik {entity, enabled?, groundHeight?} — toggle kinematic foot IK on a rig",
            [](EngineContext& ctx, const SetFootIkParams& params) -> Result<FootIkResult>
            {
                auto ik = footIkOf(ctx, params.entity);
                if (!ik)
                {
                    return Err(ik.error());
                }
                FootIkComponent& c = **ik;
                if (params.enabled)
                {
                    c.enabled = *params.enabled;
                }
                if (params.groundHeight)
                {
                    c.groundHeight = *params.groundHeight;
                }
                ctx.sceneEdit.animationVersion += 1;
                return footIkState(c);
            });
    }
}
