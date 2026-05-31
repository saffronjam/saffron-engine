module;

// Vulkan-Hpp in no-exceptions mode: every call returns a result we convert to
// Result. We never use vk::raii (it throws). Classic includes (no import std).
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
#include <nanosvg.h>
#include <nanosvgrast.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module Saffron.Rendering;

export import :RenderGraph;
export import :Types;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Detail;

namespace se
{

    auto newRenderer(Window& window) -> Result<Renderer>
    {
        Renderer renderer;
        renderer.window = &window;

        u32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

        vkb::InstanceBuilder instanceBuilder;
        instanceBuilder
            .set_app_name("Saffron Editor")
            .set_engine_name("Saffron Engine")
            .require_api_version(1, 3, 0)
            .request_validation_layers(true)
            .use_default_debug_messenger();
        for (u32 i = 0; i < sdlExtensionCount; i = i + 1)
        {
            instanceBuilder.enable_extension(sdlExtensions[i]);
        }
        auto instanceResult = instanceBuilder.build();
        if (!instanceResult)
        {
            return Err(std::format("instance creation failed: {}", instanceResult.error().message()));
        }
        renderer.context.vkbInstance = instanceResult.value();
        renderer.context.instance = vk::Instance{ renderer.context.vkbInstance.instance };

        VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window.handle, renderer.context.vkbInstance.instance, nullptr, &rawSurface))
        {
            return Err(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
        }
        renderer.context.surface = vk::SurfaceKHR{ rawSurface };

        VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        features11.shaderDrawParameters = VK_TRUE;  // Slang SV_VertexID emits the DrawParameters capability

        VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        // Bindless: one global texture array indexed per-instance. Core Vulkan 1.2
        // descriptor indexing — required (selection fails with a clear error if absent).
        VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{ renderer.context.vkbInstance };
        auto physicalResult = selector
                                  .set_minimum_version(1, 3)
                                  .set_required_features_11(features11)
                                  .set_required_features_12(features12)
                                  .set_required_features_13(features13)
                                  .set_surface(rawSurface)
                                  .select();
        if (!physicalResult)
        {
            return Err(std::format("no suitable GPU: {}", physicalResult.error().message()));
        }

        vkb::DeviceBuilder deviceBuilder{ physicalResult.value() };
        auto deviceResult = deviceBuilder.build();
        if (!deviceResult)
        {
            return Err(std::format("device creation failed: {}", deviceResult.error().message()));
        }
        renderer.context.vkbDevice = deviceResult.value();
        renderer.context.physicalDevice = vk::PhysicalDevice{ physicalResult.value().physical_device };
        renderer.context.device = vk::Device{ renderer.context.vkbDevice.device };

        auto queueResult = renderer.context.vkbDevice.get_queue(vkb::QueueType::graphics);
        if (!queueResult)
        {
            return Err(std::format("no graphics queue: {}", queueResult.error().message()));
        }
        renderer.context.graphicsQueue = vk::Queue{ queueResult.value() };
        renderer.context.graphicsQueueFamily = renderer.context.vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        // Highest MSAA level the device supports for both color + depth framebuffers (capped at 8x).
        vk::SampleCountFlags sampleCounts = renderer.context.physicalDevice.getProperties().limits.framebufferColorSampleCounts &
                                            renderer.context.physicalDevice.getProperties().limits.framebufferDepthSampleCounts;
        if (sampleCounts & vk::SampleCountFlagBits::e8) { renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e8; }
        else if (sampleCounts & vk::SampleCountFlagBits::e4) { renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e4; }
        else if (sampleCounts & vk::SampleCountFlagBits::e2) { renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e2; }

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.instance = renderer.context.vkbInstance.instance;
        allocatorInfo.physicalDevice = physicalResult.value().physical_device;
        allocatorInfo.device = renderer.context.vkbDevice.device;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&allocatorInfo, &renderer.context.allocator) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateAllocator failed" });
        }

        auto swapchainBuilt = buildSwapchain(renderer, window.width, window.height);
        if (!swapchainBuilt)
        {
            return Err(swapchainBuilt.error());
        }

        // Offscreen scene target shown in the editor Viewport panel. Same format
        // as the swapchain so the scene pipelines need no special format.
        auto offscreen = newColorImage(renderer, window.width, window.height, OffscreenColorFormat, true);
        if (!offscreen)
        {
            return Err(offscreen.error());
        }
        renderer.targets.offscreen = std::move(*offscreen);
        renderer.targets.desiredWidth = window.width;
        renderer.targets.desiredHeight = window.height;
        renderer.targets.generation = 1;

        auto depth = newDepthImage(renderer, window.width, window.height);
        if (!depth)
        {
            return Err(depth.error());
        }
        renderer.targets.depth = std::move(*depth);

        for (FrameData& frame : renderer.frame.frames)
        {
            vk::CommandPoolCreateInfo poolInfo{};
            poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
            poolInfo.queueFamilyIndex = renderer.context.graphicsQueueFamily;
            auto pool = checked(renderer.context.device.createCommandPool(poolInfo), "createCommandPool");
            if (!pool)
            {
                return Err(pool.error());
            }
            frame.commandPool = *pool;

            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto buffers = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "allocateCommandBuffers");
            if (!buffers)
            {
                return Err(buffers.error());
            }
            frame.commandBuffer = (*buffers)[0];

            auto imageAvailable = checked(renderer.context.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
            if (!imageAvailable)
            {
                return Err(imageAvailable.error());
            }
            frame.imageAvailable = *imageAvailable;

            vk::FenceCreateInfo fenceInfo{};
            fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
            auto fence = checked(renderer.context.device.createFence(fenceInfo), "createFence");
            if (!fence)
            {
                return Err(fence.error());
            }
            frame.inFlight = *fence;
        }

        if (Result<void> descriptors = initDescriptorResources(renderer); !descriptors)
        {
            return Err(descriptors.error());
        }
        setDirectionalLight(renderer, glm::vec3(-0.5f, -1.0f, -0.3f), glm::vec3(1.0f), 1.0f, 0.15f);

        const std::array<u8, 4> white{ 255, 255, 255, 255 };
        auto whiteTexture = uploadTexture(renderer, white.data(), 1, 1, false);
        if (!whiteTexture)
        {
            return Err(whiteTexture.error());
        }
        renderer.defaultWhiteTexture = *whiteTexture;

        // Seed every bindless slot with the default texture so no slot is ever unbound —
        // some drivers (llvmpipe) fault sampling a partially-bound array even on slots a
        // shader never reads. Real uploads overwrite their slot afterwards.
        {
            std::vector<vk::DescriptorImageInfo> infos(MaxBindlessTextures,
                vk::DescriptorImageInfo{ renderer.descriptors.linearSampler, (*whiteTexture)->view,
                                         vk::ImageLayout::eShaderReadOnlyOptimal });
            vk::WriteDescriptorSet fill{};
            fill.dstSet = renderer.descriptors.bindlessSet;
            fill.dstBinding = 0;
            fill.dstArrayElement = 0;
            fill.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            fill.setImageInfo(infos);
            renderer.context.device.updateDescriptorSets(fill, {});
        }

        logInfo(std::format("vulkan ready — gpu '{}', {} swapchain images",
                            renderer.context.vkbDevice.physical_device.name,
                            renderer.swapchain.images.size()));
        return renderer;
    }

    void destroyRenderer(Renderer& renderer)
    {
        if (renderer.context.device)
        {
            static_cast<void>(renderer.context.device.waitIdle());
        }

        // Drop any Refs the renderer itself still holds, plus the closure vectors
        // (which may capture Refs), before the descriptor pool / allocator / device
        // are torn down — a GpuTexture frees its material set from the pool.
        renderer.frame.sceneDrawList = SceneDrawList{};  // drops mesh/texture/pipeline Refs
        renderer.pipelines.cache.clear();            // drops the cached mesh PSOs
        renderer.frame.sceneSubmissions.clear();
        renderer.frame.uiSubmissions.clear();
        renderer.defaultWhiteTexture.reset();
        renderer.pipelines.cull.reset();        // RAII frees the compute pipeline + layout
        renderer.pipelines.thumbnail.reset();
        renderer.pipelines.tonemap.reset();
        renderer.pipelines.fxaa.reset();
        renderer.pipelines.depthPrepass.reset();

        renderer.targets.offscreen.reset();  // free before the allocator/device
        renderer.targets.depth.reset();
        renderer.targets.msaaColor.reset();
        renderer.targets.msaaDepth.reset();
        renderer.targets.scratch.reset();

        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            renderer.instancing.buffers[i].reset();  // RAII frees the SSBO before the allocator
            renderer.lighting.lightListBuffers[i].reset();
            renderer.lighting.clusterBuffers[i].reset();
            if (renderer.lighting.lightBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, renderer.lighting.lightBuffers[i], renderer.lighting.lightAllocs[i]);
                renderer.lighting.lightBuffers[i] = VK_NULL_HANDLE;
            }
            if (renderer.lighting.clusterParamBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, renderer.lighting.clusterParamBuffers[i], renderer.lighting.clusterParamAllocs[i]);
                renderer.lighting.clusterParamBuffers[i] = VK_NULL_HANDLE;
            }
        }
        if (renderer.descriptors.descriptorPool)
        {
            renderer.context.device.destroyDescriptorPool(renderer.descriptors.descriptorPool);
        }
        if (renderer.descriptors.bindlessPool)
        {
            renderer.context.device.destroyDescriptorPool(renderer.descriptors.bindlessPool);
        }
        if (renderer.descriptors.bindlessSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.bindlessSetLayout);
        }
        if (renderer.descriptors.lightSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.lightSetLayout);
        }
        if (renderer.descriptors.instanceSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.instanceSetLayout);
        }
        if (renderer.descriptors.clusterSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.clusterSetLayout);
        }
        if (renderer.descriptors.tonemapSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.tonemapSetLayout);
        }
        if (renderer.descriptors.fxaaSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.fxaaSetLayout);
        }
        if (renderer.descriptors.linearSampler)
        {
            renderer.context.device.destroySampler(renderer.descriptors.linearSampler);
        }

        for (FrameData& frame : renderer.frame.frames)
        {
            renderer.context.device.destroyFence(frame.inFlight);
            renderer.context.device.destroySemaphore(frame.imageAvailable);
            renderer.context.device.destroyCommandPool(frame.commandPool);
        }

        destroySwapchainResources(renderer);

        if (renderer.context.allocator != nullptr)
        {
            vmaDestroyAllocator(renderer.context.allocator);
            renderer.context.allocator = nullptr;
        }
        if (renderer.context.surface)
        {
            vkb::destroy_surface(renderer.context.vkbInstance, static_cast<VkSurfaceKHR>(renderer.context.surface));
        }
        vkb::destroy_device(renderer.context.vkbDevice);
        vkb::destroy_instance(renderer.context.vkbInstance);
    }

    auto beginFrame(Renderer& renderer) -> bool
    {
        const u32 winW = renderer.window->width;
        const u32 winH = renderer.window->height;
        if (winW > 0 && winH > 0 &&
            (renderer.swapchain.extent.width != winW || renderer.swapchain.extent.height != winH))
        {
            static_cast<void>(renderer.context.device.waitIdle());
            recreateSwapchain(renderer);
            return false;
        }

        FrameData& frame = renderer.frame.frames[renderer.frame.index];

        static_cast<void>(renderer.context.device.waitForFences(frame.inFlight, VK_TRUE, UINT64_MAX));

        vk::ResultValue<u32> acquire = renderer.context.device.acquireNextImageKHR(
            renderer.swapchain.handle, UINT64_MAX, frame.imageAvailable, nullptr);
        if (acquire.result == vk::Result::eErrorOutOfDateKHR)
        {
            recreateSwapchain(renderer);
            return false;
        }
        if (acquire.result != vk::Result::eSuccess && acquire.result != vk::Result::eSuboptimalKHR)
        {
            logError(std::format("vkAcquireNextImageKHR failed: {}", vk::to_string(acquire.result)));
            return false;
        }
        renderer.frame.imageIndex = acquire.value;

        // Ensure the previous frame that used THIS image has finished before we
        // reuse the image's renderFinished semaphore.
        if (renderer.swapchain.imagesInFlight[renderer.frame.imageIndex])
        {
            static_cast<void>(renderer.context.device.waitForFences(renderer.swapchain.imagesInFlight[renderer.frame.imageIndex], VK_TRUE, UINT64_MAX));
        }
        renderer.swapchain.imagesInFlight[renderer.frame.imageIndex] = frame.inFlight;

        // Apply a pending Viewport resize (requested last frame). Single shared
        // target, so a full device idle is required before recreating it.
        if (renderer.targets.desiredWidth > 0 && renderer.targets.desiredHeight > 0 &&
            (renderer.targets.desiredWidth != renderer.targets.offscreen.extent.width ||
             renderer.targets.desiredHeight != renderer.targets.offscreen.extent.height))
        {
            static_cast<void>(renderer.context.device.waitIdle());
            auto resized = newColorImage(renderer, renderer.targets.desiredWidth,
                                         renderer.targets.desiredHeight, OffscreenColorFormat, true);
            if (resized)
            {
                renderer.targets.offscreen = std::move(*resized);
                renderer.targets.generation = renderer.targets.generation + 1;
                updateTonemapSet(renderer);  // the storage-image binding follows the new view
                auto resizedDepth = newDepthImage(renderer, renderer.targets.desiredWidth, renderer.targets.desiredHeight);
                if (resizedDepth)
                {
                    renderer.targets.depth = std::move(*resizedDepth);
                }
                else
                {
                    logError(resizedDepth.error());
                }
                recreateMsaaTargets(renderer);  // MSAA targets follow the offscreen extent
                recreateFxaaTarget(renderer);   // and the FXAA scratch target
            }
            else
            {
                logError(resized.error());
            }
        }

        static_cast<void>(renderer.context.device.resetFences(frame.inFlight));
        static_cast<void>(frame.commandBuffer.reset());
        renderer.frame.sceneDrawList = SceneDrawList{};  // last frame's geometry has presented
        renderer.frame.sceneSubmissions.clear();
        renderer.frame.uiSubmissions.clear();

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(frame.commandBuffer.begin(beginInfo));

        // Rendering scopes are opened in endFrame: pass 1 (scene → offscreen),
        // pass 2 (ui → swapchain).
        return true;
    }

    void submit(Renderer& renderer, RenderFn fn)
    {
        renderer.frame.sceneSubmissions.push_back(std::move(fn));
    }

    void submitUi(Renderer& renderer, RenderFn fn)
    {
        renderer.frame.uiSubmissions.push_back(std::move(fn));
    }

    void setViewportDesiredSize(Renderer& renderer, u32 width, u32 height)
    {
        renderer.targets.desiredWidth = width;
        renderer.targets.desiredHeight = height;
    }

    auto viewportImageView(const Renderer& renderer) -> vk::ImageView
    {
        return renderer.targets.offscreen.view;
    }

    auto viewportGeneration(const Renderer& renderer) -> u32
    {
        return renderer.targets.generation;
    }

    auto viewportWidth(const Renderer& renderer) -> u32
    {
        return renderer.targets.offscreen.extent.width;
    }

    auto viewportHeight(const Renderer& renderer) -> u32
    {
        return renderer.targets.offscreen.extent.height;
    }

    void beginFrameGraph(Renderer& renderer)
    {
        Image& offscreen = renderer.targets.offscreen;
        Image& depth = renderer.targets.depth;
        const u32 f = renderer.frame.index;
        const bool doCull = renderer.lighting.clusterDispatchPending && renderer.pipelines.cull;
        renderer.lighting.clusterDispatchPending = false;

        // The frame as a render graph: declare each pass's resource usage and let the
        // graph derive the barriers + layout transitions. The offscreen color carries
        // its layout across frames (sampled by ImGui last frame → WAR into this scene).
        renderer.graph.current = newRenderGraph();
        RenderGraph& graph = renderer.graph.current;
        // frameSceneColor is always the offscreen (what ImGui samples + tonemap reads). The
        // scene's 1x result lands in `sceneOutput`: the offscreen normally, or the FXAA
        // scratch when FXAA is on (FXAA then edge-blurs scratch → offscreen). With MSAA the
        // scene renders to msaaColor and resolves into sceneOutput. mutually exclusive via set-aa.
        const bool msaa = renderer.targets.sampleCount != vk::SampleCountFlagBits::e1 && renderer.targets.msaaColor.image;
        const bool fxaa = renderer.targets.fxaaEnabled && renderer.targets.scratch.image && renderer.pipelines.fxaa;
        renderer.graph.sceneColor = importImage(graph, offscreen.image, offscreen.view,
            vk::ImageAspectFlagBits::eColor, offscreen.layout, &offscreen.layout);
        RgResource sceneOutput = renderer.graph.sceneColor;
        if (fxaa)
        {
            sceneOutput = importImage(graph, renderer.targets.scratch.image, renderer.targets.scratch.view,
                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
        }
        RgResource sceneColorAttachment = sceneOutput;
        if (msaa)
        {
            sceneColorAttachment = importImage(graph, renderer.targets.msaaColor.image, renderer.targets.msaaColor.view,
                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
        }
        Image* depthTarget = &depth;
        if (msaa)
        {
            depthTarget = &renderer.targets.msaaDepth;
        }
        RgResource sceneDepth = importImage(graph, depthTarget->image, depthTarget->view,
            vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined, nullptr);
        renderer.graph.swapImage = importImage(graph, renderer.swapchain.images[renderer.frame.imageIndex],
            renderer.swapchain.imageViews[renderer.frame.imageIndex], vk::ImageAspectFlagBits::eColor,
            vk::ImageLayout::eUndefined, nullptr);

        // Clustered forward: a compute pass culls the punctual lights into the froxel
        // grid; the scene fragment reads the result (the graph emits the compute→
        // fragment barrier from these declared usages).
        RgResource clusterBuffer{};
        if (doCull)
        {
            clusterBuffer = importBuffer(graph, renderer.lighting.clusterBuffers[f]->buffer);

            RgPass cull;
            cull.name = "light-cull";
            cull.kind = RgPassKind::Compute;
            cull.accesses = { RgAccess{ clusterBuffer, RgUsage::StorageWriteCompute } };
            cull.execute = [&renderer, f](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.cull->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    renderer.pipelines.cull->layout, 0, renderer.lighting.clusterSets[f], {});
                const u32 groups = (ClusterCount + 63) / 64;
                cmd.dispatch(groups, 1, 1);
            };
            addPass(graph, std::move(cull));
        }

        // Optional depth pre-pass: lay down scene depth first, so the scene pass loads it
        // and shades only the front-most fragments. The graph derives the depth WAW
        // barrier (pre-pass write → scene write) from the two declared depth usages.
        const bool doDepthPrepass = renderer.useDepthPrepass && renderer.pipelines.depthPrepass;
        if (doDepthPrepass)
        {
            RgPass depthPass;
            depthPass.name = "depth-prepass";
            depthPass.kind = RgPassKind::Graphics;
            depthPass.depth = RgAttachment{ sceneDepth, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            depthPass.renderArea = offscreen.extent;
            depthPass.execute = [&renderer](vk::CommandBuffer cmd)
        {
                recordDepthPrepass(renderer, cmd);
            };
            addPass(graph, std::move(depthPass));
        }

        RgPass scene;
        scene.name = "scene";
        scene.kind = RgPassKind::Graphics;
        if (doCull)
        {
            scene.accesses = { RgAccess{ clusterBuffer, RgUsage::StorageReadFragment } };
        }
        // MSAA: render to the multisampled color, resolve into the offscreen (don't store
        // the multisampled samples). Otherwise render straight into the offscreen.
        scene.color = RgAttachment{ sceneColorAttachment, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } };
        if (msaa)
        {
            scene.color->storeOp = vk::AttachmentStoreOp::eDontCare;
            scene.color->resolve = sceneOutput;
        }
        // Load the pre-pass depth when present; otherwise clear it here as before.
        vk::AttachmentLoadOp depthLoad = vk::AttachmentLoadOp::eClear;
        if (doDepthPrepass)
        {
            depthLoad = vk::AttachmentLoadOp::eLoad;
        }
        scene.depth = RgAttachment{ sceneDepth, depthLoad, vk::AttachmentStoreOp::eDontCare,
            vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
        scene.renderArea = offscreen.extent;
        scene.execute = [&renderer](vk::CommandBuffer cmd)
        {
            recordSceneDrawList(renderer, cmd);
            for (RenderFn& fn : renderer.frame.sceneSubmissions)
            {
                fn(cmd);
            }
        };
        addPass(graph, std::move(scene));

        // FXAA: edge-blur the scene scratch into the offscreen (a compute pass). Added here
        // so it runs before any app-authored post-process (e.g. tonemap) + the ui pass.
        if (fxaa)
        {
            RgPass fxaaPass;
            fxaaPass.name = "fxaa";
            fxaaPass.kind = RgPassKind::Compute;
            fxaaPass.accesses = { RgAccess{ sceneOutput, RgUsage::SampledReadCompute },
                                  RgAccess{ renderer.graph.sceneColor, RgUsage::StorageImageRWCompute } };
            const vk::Extent2D extent = offscreen.extent;
            fxaaPass.execute = [&renderer, extent](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.fxaa->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    renderer.pipelines.fxaa->layout, 0, renderer.descriptors.fxaaSet, {});
                cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            };
            addPass(graph, std::move(fxaaPass));
        }
    }

    auto frameGraph(Renderer& renderer) -> RenderGraph&
    {
        return renderer.graph.current;
    }

    auto viewportColorResource(const Renderer& renderer) -> RgResource
    {
        return renderer.graph.sceneColor;
    }

    void addTonemapPass(Renderer& renderer, RenderGraph& graph)
    {
        RgPass pass;
        pass.name = "tonemap";
        pass.kind = RgPassKind::Compute;
        pass.accesses = { RgAccess{ renderer.graph.sceneColor, RgUsage::StorageImageRWCompute } };
        pass.execute = [&renderer](vk::CommandBuffer cmd)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.tonemap->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                renderer.pipelines.tonemap->layout, 0, renderer.descriptors.tonemapSet, {});
            const vk::Extent2D extent = renderer.targets.offscreen.extent;
            cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
        addPass(graph, std::move(pass));
    }

    void endFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frame.frames[renderer.frame.index];
        RenderGraph& graph = renderer.graph.current;

        // The ui pass samples the (now post-processed) offscreen color and composites
        // ImGui into the swapchain. Added last so app-authored passes land before it.
        RgPass ui;
        ui.name = "ui";
        ui.kind = RgPassKind::Graphics;
        ui.accesses = { RgAccess{ renderer.graph.sceneColor, RgUsage::SampledRead } };
        ui.color = RgAttachment{ renderer.graph.swapImage, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } };
        ui.renderArea = renderer.swapchain.extent;
        ui.execute = [&renderer](vk::CommandBuffer cmd)
        {
            for (RenderFn& fn : renderer.frame.uiSubmissions)
            {
                fn(cmd);
            }
        };
        addPass(graph, std::move(ui));

        executeRenderGraph(graph, frame.commandBuffer);

        // The swapchain image is only safely owned in-frame, so a pending capture
        // is copied here, between the ImGui pass and present; its COLOR->PRESENT
        // transition is folded into captureImageToBuffer's toLayout.
        VkBuffer captureBuffer = VK_NULL_HANDLE;
        VmaAllocation captureAlloc = nullptr;
        VmaAllocationInfo captureInfo{};
        vk::Extent2D captureExtent{};
        bool doCapture = renderer.captureNextSwapchainPath.has_value();
        if (doCapture)
        {
            captureExtent = renderer.swapchain.extent;
            const vk::DeviceSize bytes =
                static_cast<vk::DeviceSize>(captureExtent.width) * captureExtent.height * 4;
            Result<void> created =
                newHostCaptureBuffer(renderer, bytes, captureBuffer, captureAlloc, captureInfo);
            if (!created)
            {
                logError(created.error());
                doCapture = false;
                renderer.captureNextSwapchainPath.reset();
            }
        }
        if (doCapture)
        {
            captureImageToBuffer(
                frame.commandBuffer, renderer.swapchain.images[renderer.frame.imageIndex], captureExtent,
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone,
                vk::Buffer{ captureBuffer });
        }
        else
        {
            transitionImage(
                frame.commandBuffer, renderer.swapchain.images[renderer.frame.imageIndex],
                vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone);
        }

        static_cast<void>(frame.commandBuffer.end());

        vk::Semaphore signalSemaphore = renderer.swapchain.renderFinished[renderer.frame.imageIndex];

        vk::SemaphoreSubmitInfo waitInfo{};
        waitInfo.semaphore = frame.imageAvailable;
        waitInfo.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

        vk::SemaphoreSubmitInfo signalInfo{};
        signalInfo.semaphore = signalSemaphore;
        signalInfo.stageMask = vk::PipelineStageFlagBits2::eAllCommands;

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = frame.commandBuffer;

        vk::SubmitInfo2 submitInfo{};
        submitInfo.setWaitSemaphoreInfos(waitInfo);
        submitInfo.setCommandBufferInfos(cmdInfo);
        submitInfo.setSignalSemaphoreInfos(signalInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, frame.inFlight));

        vk::PresentInfoKHR presentInfo{};
        presentInfo.setWaitSemaphores(signalSemaphore);
        presentInfo.setSwapchains(renderer.swapchain.handle);
        presentInfo.setImageIndices(renderer.frame.imageIndex);
        vk::Result present = renderer.context.graphicsQueue.presentKHR(presentInfo);
        if (present == vk::Result::eErrorOutOfDateKHR || present == vk::Result::eSuboptimalKHR)
        {
            recreateSwapchain(renderer);
        }

        // The recorded copy is now submitted; idle so it completed, then write the PNG.
        if (doCapture && captureBuffer != VK_NULL_HANDLE)
        {
            static_cast<void>(renderer.context.device.waitIdle());
            vmaInvalidateAllocation(renderer.context.allocator, captureAlloc, 0, VK_WHOLE_SIZE);
            auto wrote = writeBufferToPng(
                static_cast<const unsigned char*>(captureInfo.pMappedData),
                captureExtent.width, captureExtent.height,
                renderer.swapchain.format, *renderer.captureNextSwapchainPath);
            if (!wrote)
            {
                logError(wrote.error());
            }
            else
            {
                logInfo(std::format("captured window ({}x{}) to {}",
                                    captureExtent.width, captureExtent.height,
                                    *renderer.captureNextSwapchainPath));
            }
            vmaDestroyBuffer(renderer.context.allocator, captureBuffer, captureAlloc);
            renderer.captureNextSwapchainPath.reset();
        }

        renderer.frame.index = (renderer.frame.index + 1) % MaxFramesInFlight;
    }

    auto assetPath(std::string_view relative) -> std::string
    {
        const char* base = SDL_GetBasePath();  // SDL3: owned by SDL, do not free
        std::string result;
        if (base != nullptr)
        {
            result = base;
        }
        result.append(relative);
        return result;
    }

    auto newMeshPipeline(Renderer& renderer, std::string_view shaderName, bool unlit) -> Result<Ref<Pipeline>>
    {
        std::string path = assetPath(shaderName);
        auto moduleResult = loadShaderModule(renderer.context.device, path);
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        // The übershader's unlit branch is a specialization constant (id 0) baked into the
        // fragment stage, so this PSO is the lit or the unlit variant.
        const vk::Bool32 unlitValue = static_cast<vk::Bool32>(unlit);
        vk::SpecializationMapEntry specEntry{};
        specEntry.constantID = 0;
        specEntry.offset = 0;
        specEntry.size = sizeof(vk::Bool32);
        vk::SpecializationInfo specInfo{};
        specInfo.setMapEntries(specEntry);
        specInfo.dataSize = sizeof(vk::Bool32);
        specInfo.pData = &unlitValue;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";
        stages[1].pSpecializationInfo = &specInfo;

        vk::VertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;

        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };

        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.setVertexBindingDescriptions(binding);
        vertexInput.setVertexAttributeDescriptions(attributes);

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;

        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;  // enable Back once winding is verified
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;

        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = renderer.targets.sampleCount;  // match the MSAA target

        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLessOrEqual;  // passes fragments at a depth pre-pass's value

        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_FALSE;
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);

        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(OffscreenColorFormat);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4);  // viewProj

        std::array<vk::DescriptorSetLayout, 3> setLayouts{
            renderer.descriptors.bindlessSetLayout, renderer.descriptors.lightSetLayout, renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (mesh)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stages);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (mesh): {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    auto requestMeshPipeline(Renderer& renderer, const Material& material) -> Ref<Pipeline>
    {
        std::string key = material.shader;
        if (material.unlit)
        {
            key = key + "|unlit";
        }
        auto found = renderer.pipelines.cache.find(key);
        if (found != renderer.pipelines.cache.end())
        {
            return found->second;
        }
        auto built = newMeshPipeline(renderer, material.shader, material.unlit);
        if (!built)
        {
            logError(built.error());
            return nullptr;
        }
        renderer.pipelines.cache.emplace(key, *built);
        return *built;
    }

    auto pipelineCount(const Renderer& renderer) -> u32
    {
        return static_cast<u32>(renderer.pipelines.cache.size());
    }

    auto uploadMesh(Renderer& renderer, const Mesh& mesh) -> Result<Ref<GpuMesh>>
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            return Err(std::string{ "uploadMesh: empty mesh" });
        }
        const vk::DeviceSize vertexBytes = mesh.vertices.size() * sizeof(Vertex);
        const vk::DeviceSize indexBytes = mesh.indices.size() * sizeof(u32);

        // One staging buffer holds [vertices | indices]; two copies fan it out to
        // device-local vertex + index buffers.
        VkBufferCreateInfo stagingInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = vertexBytes + indexBytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(renderer.context.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation, &stagingMapped) != VK_SUCCESS)
        {
            return Err(std::string{ "uploadMesh: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, mesh.vertices.data(), vertexBytes);
        std::memcpy(static_cast<char*>(stagingMapped.pMappedData) + vertexBytes, mesh.indices.data(), indexBytes);
        vmaFlushAllocation(renderer.context.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        auto makeDeviceBuffer = [&](vk::DeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VmaAllocation& outAlloc) -> bool
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = size;
            info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            return vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &outBuffer, &outAlloc, nullptr) == VK_SUCCESS;
        };

        GpuMesh gpu;
        gpu.allocator = renderer.context.allocator;
        gpu.indexCount = static_cast<u32>(mesh.indices.size());
        gpu.submeshes = mesh.submeshes;
        gpu.boundsMin = glm::vec3(std::numeric_limits<f32>::max());
        gpu.boundsMax = glm::vec3(std::numeric_limits<f32>::lowest());
        for (const Vertex& vertex : mesh.vertices)
        {
            gpu.boundsMin = glm::min(gpu.boundsMin, vertex.position);
            gpu.boundsMax = glm::max(gpu.boundsMax, vertex.position);
        }

        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = nullptr;
        VmaAllocation indexAlloc = nullptr;
        if (!makeDeviceBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer, vertexAlloc) ||
            !makeDeviceBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer, indexAlloc))
        {
            if (vertexBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, vertexBuffer, vertexAlloc);
            }
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(std::string{ "uploadMesh: device vmaCreateBuffer failed" });
        }
        gpu.vertexBuffer = vk::Buffer{ vertexBuffer };
        gpu.vertexAlloc = vertexAlloc;
        gpu.indexBuffer = vk::Buffer{ indexBuffer };
        gpu.indexAlloc = indexAlloc;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "uploadMesh: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];

        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        cmd.copyBuffer(vk::Buffer{ staging }, gpu.vertexBuffer, vk::BufferCopy{ 0, 0, vertexBytes });
        cmd.copyBuffer(vk::Buffer{ staging }, gpu.indexBuffer, vk::BufferCopy{ vertexBytes, 0, indexBytes });
        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);

        logInfo(std::format("uploaded mesh: {} vertices, {} indices, {} submeshes",
                            mesh.vertices.size(), mesh.indices.size(), mesh.submeshes.size()));
        return std::make_shared<GpuMesh>(std::move(gpu));
    }

    // Ensures the current frame's instance buffer holds at least `count` elements,
    // growing to the next power of two (never shrinking) and rewriting its set.
    auto ensureInstanceCapacity(Renderer& renderer, u32 frame, u32 count) -> Result<void>
    {
        if (renderer.instancing.buffers[frame] && renderer.instancing.capacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.instancing.capacity[frame];
        if (capacity == 0)
        {
            capacity = 256;
        }
        while (capacity < count)
        {
            capacity = capacity * 2;
        }

        // beginFrame already waited on this frame's fence, so the old buffer (used by
        // the same frame) is no longer in flight — safe to drop and recreate.
        Result<Ref<Buffer>> buffer =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(InstanceData));
        if (!buffer)
        {
            return Err(buffer.error());
        }
        renderer.instancing.buffers[frame] = *buffer;
        renderer.instancing.capacity[frame] = capacity;

        vk::DescriptorBufferInfo bufferInfo{ (*buffer)->buffer, 0, (*buffer)->size };
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.instancing.sets[frame];
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.setBufferInfo(bufferInfo);
        renderer.context.device.updateDescriptorSets(write, {});
        return {};
    }

    // Ensures the current frame's punctual-light buffer holds at least `count` lights,
    // growing to the next power of two (never shrinking) and rewriting its set.
    auto ensureLightCapacity(Renderer& renderer, u32 frame, u32 count) -> Result<void>
    {
        if (renderer.lighting.lightListBuffers[frame] && renderer.lighting.lightListCapacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.lighting.lightListCapacity[frame];
        if (capacity == 0)
        {
            capacity = LightListInitial;
        }
        while (capacity < count)
        {
            capacity = capacity * 2;
        }
        Result<Ref<Buffer>> buffer =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(GpuLight));
        if (!buffer)
        {
            return Err(buffer.error());
        }
        renderer.lighting.lightListBuffers[frame] = *buffer;
        renderer.lighting.lightListCapacity[frame] = capacity;

        // Both the fragment lighting set (binding 1) and the compute cluster set
        // (binding 1) read this buffer — rewrite both to the grown allocation.
        vk::DescriptorBufferInfo bufferInfo{ (*buffer)->buffer, 0, (*buffer)->size };
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = renderer.lighting.lightSets[frame];
        writes[0].dstBinding = 1;
        writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[0].setBufferInfo(bufferInfo);
        writes[1].dstSet = renderer.lighting.clusterSets[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[1].setBufferInfo(bufferInfo);
        renderer.context.device.updateDescriptorSets(writes, {});
        return {};
    }

    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items)
    {
        renderer.stats = RenderStats{};
        renderer.frame.sceneDrawList = SceneDrawList{};
        if (items.empty())
        {
            return;
        }

        // Bucket items by (pipeline, mesh); each bucket becomes one instanced draw. The
        // albedo is bindless — a per-instance index into the global texture array — so
        // items differing only by texture batch together. First-seen order preserved.
        struct Bucket
        {
            Ref<Pipeline> pipeline;
            Ref<GpuMesh> mesh;
            std::vector<InstanceData> instances;
        };
        std::vector<Bucket> buckets;
        std::vector<Ref<GpuTexture>> liveTextures;
        for (const DrawItem& item : items)
        {
            if (!item.mesh)
            {
                continue;
            }
            auto pipeline = requestMeshPipeline(renderer, item.material);
            if (!pipeline)
            {
                continue;
            }
            // Resolve the albedo to a bindless slot (default white when the item has none).
            u32 textureIndex = 0;
            if (renderer.defaultWhiteTexture)
            {
                textureIndex = renderer.defaultWhiteTexture->bindlessIndex;
            }
            if (item.texture && item.texture->image)
            {
                textureIndex = item.texture->bindlessIndex;
                liveTextures.push_back(item.texture);
            }
            Bucket* bucket = nullptr;
            for (Bucket& candidate : buckets)
            {
                if (candidate.pipeline.get() == pipeline.get() && candidate.mesh.get() == item.mesh.get())
                {
                    bucket = &candidate;
                    break;
                }
            }
            if (bucket == nullptr)
            {
                buckets.push_back(Bucket{ pipeline, item.mesh, {} });
                bucket = &buckets.back();
            }
            bucket->instances.push_back(InstanceData{ item.model, item.normalMatrix, item.baseColor,
                                                      glm::uvec4{ textureIndex, 0, 0, 0 } });
        }

        // Flatten buckets into one contiguous instance array + per-batch ranges.
        std::vector<InstanceData> instances;
        instances.reserve(items.size());
        std::vector<DrawBatch> batches;
        for (Bucket& bucket : buckets)
        {
            DrawBatch batch;
            batch.pipeline = bucket.pipeline;
            batch.mesh = bucket.mesh;
            batch.baseInstance = static_cast<u32>(instances.size());
            batch.instanceCount = static_cast<u32>(bucket.instances.size());
            instances.insert(instances.end(), bucket.instances.begin(), bucket.instances.end());
            batches.push_back(std::move(batch));
        }

        if (instances.empty())
        {
            return;
        }
        const u32 frame = renderer.frame.index;
        if (Result<void> ok = ensureInstanceCapacity(renderer, frame, static_cast<u32>(instances.size())); !ok)
        {
            logError(ok.error());
            return;
        }
        const vk::DeviceSize bytes = instances.size() * sizeof(InstanceData);
        std::memcpy(renderer.instancing.buffers[frame]->mapped, instances.data(), bytes);
        vmaFlushAllocation(renderer.context.allocator, renderer.instancing.buffers[frame]->alloc, 0, bytes);

        u32 drawCalls = 0;
        u32 drawnInstances = 0;
        for (const DrawBatch& batch : batches)
        {
            drawCalls = drawCalls + static_cast<u32>(batch.mesh->submeshes.size());
            drawnInstances = drawnInstances + batch.instanceCount;
        }
        renderer.stats.drawCalls = drawCalls;
        renderer.stats.batches = static_cast<u32>(batches.size());
        renderer.stats.instances = drawnInstances;

        SceneDrawList list;
        list.viewProj = viewProj;
        list.batches = std::move(batches);
        list.liveTextures = std::move(liveTextures);
        list.lightSet = renderer.lighting.lightSets[frame];
        list.instanceSet = renderer.instancing.sets[frame];
        list.valid = true;
        renderer.frame.sceneDrawList = std::move(list);
    }

    // Record the scene's shaded geometry. All mesh PSOs share the layout, so the light +
    // instance sets and the viewProj push bind once; per batch then binds its pipeline +
    // material set and issues one instanced drawIndexed.
    void recordSceneDrawList(Renderer& renderer, vk::CommandBuffer cmd)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || list.batches.empty())
        {
            return;
        }
        vk::PipelineLayout layout = list.batches[0].pipeline->layout;
        // All sets bind once: the bindless albedo array (0) + light (1) + instance (2).
        // Per-instance texture indices live in the instance buffer, so no per-batch set.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, renderer.descriptors.bindlessSet, {});
        std::array<vk::DescriptorSet, 2> frameSets{ list.lightSet, list.instanceSet };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, frameSets, {});
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &list.viewProj);
        for (const DrawBatch& batch : list.batches)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, batch.pipeline->pipeline);
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            for (const Submesh& submesh : batch.mesh->submeshes)
            {
                cmd.drawIndexed(submesh.indexCount, batch.instanceCount, submesh.firstIndex,
                                submesh.vertexOffset, batch.baseInstance);
            }
        }
    }

    void recordDepthPrepass(Renderer& renderer, vk::CommandBuffer cmd)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || !renderer.pipelines.depthPrepass)
        {
            return;
        }
        // The vertex-only pipeline needs only the instance set (set 2) + viewProj push.
        vk::PipelineLayout layout = renderer.pipelines.depthPrepass->layout;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.depthPrepass->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, list.instanceSet, {});
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &list.viewProj);
        for (const DrawBatch& batch : list.batches)
        {
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            for (const Submesh& submesh : batch.mesh->submeshes)
            {
                cmd.drawIndexed(submesh.indexCount, batch.instanceCount, submesh.firstIndex,
                                submesh.vertexOffset, batch.baseInstance);
            }
        }
    }

    auto defaultTexture(const Renderer& renderer) -> const Ref<GpuTexture>&
    {
        return renderer.defaultWhiteTexture;
    }

    auto renderStats(const Renderer& renderer) -> RenderStats
    {
        return renderer.stats;
    }

    void waitGpuIdle(Renderer& renderer)
    {
        if (renderer.context.device)
        {
            static_cast<void>(renderer.context.device.waitIdle());
        }
    }

    void setDirectionalLight(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity, f32 ambient)
    {
        setSceneLighting(renderer, direction, color, intensity, ambient, {});
    }

    void setSceneLighting(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity,
                          f32 ambient, const std::vector<GpuLight>& lights)
    {
        // Write the current frame's copies; beginFrame already waited on its fence, so
        // no in-flight frame is reading them.
        const u32 frame = renderer.frame.index;
        if (renderer.lighting.lightMapped[frame] == nullptr)
        {
            return;
        }
        const u32 count = static_cast<u32>(lights.size());
        if (count > 0)
        {
            if (Result<void> ok = ensureLightCapacity(renderer, frame, count); !ok)
            {
                logError(ok.error());
                return;
            }
            const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(count) * sizeof(GpuLight);
            std::memcpy(renderer.lighting.lightListBuffers[frame]->mapped, lights.data(), bytes);
            vmaFlushAllocation(renderer.context.allocator, renderer.lighting.lightListBuffers[frame]->alloc, 0, bytes);
        }

        LightUbo ubo;
        ubo.directionAmbient = glm::vec4(glm::normalize(direction), ambient);
        ubo.colorIntensity = glm::vec4(color, intensity);
        ubo.counts = glm::uvec4(count, 0, 0, 0);
        std::memcpy(renderer.lighting.lightMapped[frame], &ubo, sizeof(ubo));
        vmaFlushAllocation(renderer.context.allocator, renderer.lighting.lightAllocs[frame], 0, sizeof(ubo));
        renderer.lighting.frameLightCount = count;
    }

    void setClusterCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj,
                          f32 nearPlane, f32 farPlane)
    {
        const u32 frame = renderer.frame.index;
        if (renderer.lighting.clusterParamMapped[frame] == nullptr)
        {
            return;
        }
        ClusterParams params;
        params.view = view;
        params.inverseProjection = glm::inverse(proj);
        params.gridSize = glm::uvec4(ClusterGridX, ClusterGridY, ClusterGridZ, renderer.lighting.frameLightCount);
        params.screenSize = glm::uvec4(viewportWidth(renderer), viewportHeight(renderer),
                                       renderer.lighting.useClustered ? 1u : 0u, 0u);
        params.zPlanes = glm::vec4(nearPlane, farPlane, 0.0f, 0.0f);
        std::memcpy(renderer.lighting.clusterParamMapped[frame], &params, sizeof(params));
        vmaFlushAllocation(renderer.context.allocator, renderer.lighting.clusterParamAllocs[frame], 0, sizeof(params));
        renderer.lighting.clusterDispatchPending = renderer.lighting.useClustered && renderer.lighting.frameLightCount > 0;
    }

    void setClustered(Renderer& renderer, bool enabled)
    {
        renderer.lighting.useClustered = enabled;
    }

    auto clusteredEnabled(const Renderer& renderer) -> bool
    {
        return renderer.lighting.useClustered;
    }

    void setPostProcess(Renderer& renderer, bool enabled)
    {
        renderer.usePostProcess = enabled;
    }

    auto postProcessEnabled(const Renderer& renderer) -> bool
    {
        return renderer.usePostProcess;
    }

    void setDepthPrepass(Renderer& renderer, bool enabled)
    {
        renderer.useDepthPrepass = enabled;
    }

    auto depthPrepassEnabled(const Renderer& renderer) -> bool
    {
        return renderer.useDepthPrepass;
    }

    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa)
    {
        vk::SampleCountFlagBits count = vk::SampleCountFlagBits::e1;
        if (msaaSamples >= 8) { count = vk::SampleCountFlagBits::e8; }
        else if (msaaSamples >= 4) { count = vk::SampleCountFlagBits::e4; }
        else if (msaaSamples >= 2) { count = vk::SampleCountFlagBits::e2; }
        if (static_cast<u32>(count) > static_cast<u32>(renderer.targets.maxSampleCount))
        {
            count = renderer.targets.maxSampleCount;
        }

        waitGpuIdle(renderer);
        renderer.targets.sampleCount = count;
        renderer.targets.fxaaEnabled = fxaa;
        recreateMsaaTargets(renderer);
        recreateFxaaTarget(renderer);

        // The mesh + depth-prepass PSOs bake the sample count — rebuild them.
        renderer.pipelines.cache.clear();
        Result<Ref<Pipeline>> depthPrepass =
            makeDepthPrepassPipeline(renderer, "shaders/mesh.spv");
        if (depthPrepass)
        {
            renderer.pipelines.depthPrepass = *depthPrepass;
        }
        else
        {
            logError(depthPrepass.error());
        }
    }

    auto aaMode(const Renderer& renderer) -> std::string
    {
        if (renderer.targets.fxaaEnabled)
        {
            return "fxaa";
        }
        const u32 n = static_cast<u32>(renderer.targets.sampleCount);
        if (n <= 1)
        {
            return "off";
        }
        return std::format("msaa{}", n);
    }

    auto uploadSvgIcon(Renderer& renderer, const std::string& svgPath,
                                                              u32 pixelSize, glm::vec4 tint) -> Result<Ref<GpuTexture>>
    {
        std::ifstream in(svgPath);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", svgPath));
        }
        std::string svg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // nanosvg does not resolve the "currentColor" keyword; map it to white so
        // stroke-only icons (e.g. Lucide) rasterize, then tint below.
        for (std::size_t pos = svg.find("currentColor"); pos != std::string::npos; pos = svg.find("currentColor", pos))
        {
            svg.replace(pos, 12, "#ffffff");
        }

        NSVGimage* image = nsvgParse(svg.data(), "px", 96.0f);  // nsvgParse mutates the buffer
        if (image == nullptr || image->width <= 0.0f || image->height <= 0.0f)
        {
            if (image != nullptr)
            {
                nsvgDelete(image);
            }
            return Err(std::format("nanosvg failed to parse '{}'", svgPath));
        }
        NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
        if (rasterizer == nullptr)
        {
            nsvgDelete(image);
            return Err(std::string{ "nsvgCreateRasterizer failed" });
        }
        const f32 scale = static_cast<f32>(pixelSize) / glm::max(image->width, image->height);
        std::vector<u8> rgba(static_cast<std::size_t>(pixelSize) * pixelSize * 4, 0);
        nsvgRasterize(rasterizer, image, 0.0f, 0.0f, scale, rgba.data(),
                      static_cast<int>(pixelSize), static_cast<int>(pixelSize), static_cast<int>(pixelSize) * 4);
        nsvgDeleteRasterizer(rasterizer);
        nsvgDelete(image);

        for (std::size_t i = 0; i < rgba.size(); i = i + 4)
        {
            rgba[i + 0] = static_cast<u8>(rgba[i + 0] * tint.r);
            rgba[i + 1] = static_cast<u8>(rgba[i + 1] * tint.g);
            rgba[i + 2] = static_cast<u8>(rgba[i + 2] * tint.b);
            rgba[i + 3] = static_cast<u8>(rgba[i + 3] * tint.a);
        }
        return uploadTexture(renderer, rgba.data(), pixelSize, pixelSize, false);
    }

    // The minimal mesh-thumbnail pipeline (vertex input + a 2x mat4 push constant, no
    // descriptor sets). Color format matches the offscreen thumbnail image.
    auto newThumbnailPipeline(Renderer& renderer) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath("shaders/thumbnail.spv"));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";

        vk::VertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };
        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.setVertexBindingDescriptions(binding);
        vertexInput.setVertexAttributeDescriptions(attributes);

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(renderer.swapchain.format);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = 2 * sizeof(glm::mat4);  // mvp + normalMatrix

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (thumbnail)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stages);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (thumbnail): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    auto renderMeshThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size) -> Result<Ref<GpuTexture>>
    {
        if (!mesh)
        {
            return Err(std::string{ "renderMeshThumbnail: null mesh" });
        }
        if (!renderer.pipelines.thumbnail)
        {
            auto pipeline = newThumbnailPipeline(renderer);
            if (!pipeline)
            {
                return Err(pipeline.error());
            }
            renderer.pipelines.thumbnail = *pipeline;
        }

        auto colorImage = newColorImage(renderer, size, size, renderer.swapchain.format);
        if (!colorImage)
        {
            return Err(colorImage.error());
        }
        Image color = std::move(*colorImage);
        auto depthImage = newDepthImage(renderer, size, size);
        if (!depthImage)
        {
            return Err(depthImage.error());
        }
        Image depth = std::move(*depthImage);

        // Frame the mesh: a 3/4 view at a distance that fits its bounding sphere.
        const glm::vec3 center = (mesh->boundsMin + mesh->boundsMax) * 0.5f;
        f32 radius = glm::length(mesh->boundsMax - mesh->boundsMin) * 0.5f;
        if (radius <= 0.0001f)
        {
            radius = 1.0f;
        }
        const f32 fovy = glm::radians(45.0f);
        const f32 distance = radius / glm::tan(fovy * 0.5f) * 1.3f;
        const glm::vec3 eye = center + glm::normalize(glm::vec3(1.0f, 0.7f, 1.0f)) * distance;
        const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj = glm::perspective(fovy, 1.0f, glm::max(0.01f, distance - radius * 2.0f), distance + radius * 2.0f);
        proj[1][1] *= -1.0f;  // Vulkan clip; matches the viewport so the thumbnail is upright
        struct ThumbnailPush
        {
            glm::mat4 mvp;
            glm::mat4 normalMatrix;
        } push{ proj * view, glm::mat4(1.0f) };

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "renderMeshThumbnail: allocateCommandBuffers");
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        transitionImage(cmd, color.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
        transitionImage(cmd, depth.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageAspectFlagBits::eDepth);

        vk::RenderingAttachmentInfo colorAttach{};
        colorAttach.imageView = color.view;
        colorAttach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttach.loadOp = vk::AttachmentLoadOp::eClear;
        colorAttach.storeOp = vk::AttachmentStoreOp::eStore;
        colorAttach.clearValue = vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.12f, 0.12f, 0.14f, 1.0f } } };
        vk::RenderingAttachmentInfo depthAttach{};
        depthAttach.imageView = depth.view;
        depthAttach.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depthAttach.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
        depthAttach.clearValue = vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } };
        vk::RenderingInfo rendering{};
        rendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } };
        rendering.layerCount = 1;
        rendering.setColorAttachments(colorAttach);
        rendering.setPDepthAttachment(&depthAttach);
        cmd.beginRendering(rendering);

        vk::Viewport viewport{ 0.0f, 0.0f, static_cast<f32>(size), static_cast<f32>(size), 0.0f, 1.0f };
        cmd.setViewport(0, viewport);
        cmd.setScissor(0, vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } });
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.thumbnail->pipeline);
        cmd.pushConstants(renderer.pipelines.thumbnail->layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push), &push);
        vk::DeviceSize offset = 0;
        cmd.bindVertexBuffers(0, mesh->vertexBuffer, offset);
        cmd.bindIndexBuffer(mesh->indexBuffer, 0, vk::IndexType::eUint32);
        for (const Submesh& submesh : mesh->submeshes)
        {
            cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
        }
        cmd.endRendering();

        transitionImage(cmd, color.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);

        // Take ownership of the color image as a sampled GpuTexture (no material set;
        // ImGui samples it via uiRegisterTexture). Null the Image's handles so it does
        // not free them on scope exit.
        GpuTexture texture;
        texture.device = renderer.context.device;
        texture.allocator = renderer.context.allocator;
        texture.image = color.image;
        texture.view = color.view;
        texture.alloc = color.alloc;
        texture.extent = color.extent;
        texture.format = color.format;
        color.image = nullptr;
        color.view = nullptr;
        color.alloc = nullptr;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

    auto uploadTexture(Renderer& renderer, const u8* rgba, u32 width, u32 height, bool srgb) -> Result<Ref<GpuTexture>>
    {
        if (width == 0 || height == 0)
        {
            return Err(std::string{ "uploadTexture: zero-sized image" });
        }
        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(width) * height * 4;

        VkBufferCreateInfo stagingInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = bytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(renderer.context.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation, &stagingMapped) != VK_SUCCESS)
        {
            return Err(std::string{ "uploadTexture: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, rgba, bytes);
        vmaFlushAllocation(renderer.context.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        const vk::Format format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo imageAlloc{};
        imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation imageAllocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &imageAlloc, &rawImage, &imageAllocation, nullptr) != VK_SUCCESS)
        {
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(std::string{ "uploadTexture: vmaCreateImage failed" });
        }

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "uploadTexture: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        transitionImage(cmd, vk::Image{ rawImage },
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ width, height, 1 };
        cmd.copyBufferToImage(vk::Buffer{ staging }, vk::Image{ rawImage }, vk::ImageLayout::eTransferDstOptimal, region);
        transitionImage(cmd, vk::Image{ rawImage },
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        auto view = checked(renderer.context.device.createImageView(viewInfo), "uploadTexture: createImageView");
        if (!view)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            return Err(view.error());
        }

        // Claim a bindless slot and write the texture into the global array.
        const u32 index = renderer.descriptors.nextBindlessIndex;
        renderer.descriptors.nextBindlessIndex = renderer.descriptors.nextBindlessIndex + 1;
        writeBindlessTexture(renderer, *view, index);

        GpuTexture texture;
        texture.device = renderer.context.device;
        texture.allocator = renderer.context.allocator;
        texture.image = vk::Image{ rawImage };
        texture.view = *view;
        texture.alloc = imageAllocation;
        texture.extent = vk::Extent2D{ width, height };
        texture.format = format;
        texture.bindlessIndex = index;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

}
