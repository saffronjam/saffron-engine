module;

// Vulkan-Hpp in no-exceptions mode: every call returns a result we convert to
// std::expected. We never use vk::raii (it throws). Classic includes (no import std).
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module Saffron.Rendering;

import Saffron.Core;
import Saffron.Window;

export namespace se
{
    // A unit of GPU work recorded into the active command buffer — the deferred
    // submission seam. The backend supplies the command buffer.
    using RenderFn = std::function<void(vk::CommandBuffer)>;

    inline constexpr u32 MaxFramesInFlight = 2;

    struct FrameData
    {
        vk::CommandPool commandPool;
        vk::CommandBuffer commandBuffer;
        vk::Semaphore imageAvailable;
        vk::Fence inFlight;
    };

    // --- RAII meta-layer ------------------------------------------------------
    // A graphics pipeline that owns its vk handles and frees them on destruction.
    // Move-only: the destructor + move-assignment are resource management. Owned
    // by the Renderer (never crosses to client code), destroyed before the device.
    struct Pipeline
    {
        vk::Device device;  // borrowed
        vk::Pipeline pipeline;
        vk::PipelineLayout layout;

        Pipeline() = default;
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;

        Pipeline(Pipeline&& other) noexcept
            : device(other.device), pipeline(other.pipeline), layout(other.layout)
        {
            other.device = nullptr;
            other.pipeline = nullptr;
            other.layout = nullptr;
        }

        Pipeline& operator=(Pipeline&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                device = other.device;
                pipeline = other.pipeline;
                layout = other.layout;
                other.device = nullptr;
                other.pipeline = nullptr;
                other.layout = nullptr;
            }
            return *this;
        }

        ~Pipeline()
        {
            reset();
        }

        void reset()
        {
            if (device)
            {
                if (pipeline)
                {
                    device.destroyPipeline(pipeline);
                }
                if (layout)
                {
                    device.destroyPipelineLayout(layout);
                }
            }
            pipeline = nullptr;
            layout = nullptr;
        }
    };

    struct Renderer
    {
        // vk-bootstrap keeps the bits we need for clean teardown.
        vkb::Instance vkbInstance;
        vkb::Device vkbDevice;

        vk::Instance instance;
        vk::SurfaceKHR surface;
        vk::PhysicalDevice physicalDevice;
        vk::Device device;
        vk::Queue graphicsQueue;
        u32 graphicsQueueFamily = 0;
        VmaAllocator allocator = nullptr;

        vk::SwapchainKHR swapchain;
        vk::Format swapchainFormat = vk::Format::eUndefined;
        vk::Extent2D swapchainExtent;
        std::vector<vk::Image> swapchainImages;
        std::vector<vk::ImageView> swapchainImageViews;
        std::vector<vk::Semaphore> renderFinished;  // one per swapchain image
        std::vector<vk::Fence> imagesInFlight;       // borrowed per-frame fence per image

        std::array<FrameData, MaxFramesInFlight> frames{};
        u32 frameIndex = 0;
        u32 imageIndex = 0;

        std::array<f32, 4> clearColor{ 0.05f, 0.06f, 0.08f, 1.0f };
        std::vector<RenderFn> submissions;
        std::vector<Pipeline> pipelines;  // owned; destroyed before the device

        Window* window = nullptr;  // borrowed
    };

    std::expected<Renderer, std::string> newRenderer(Window& window);
    void destroyRenderer(Renderer& renderer);

    bool beginFrame(Renderer& renderer);
    void submit(Renderer& renderer, RenderFn fn);
    void endFrame(Renderer& renderer);

    std::string assetPath(std::string_view relative);

    // Creates a pipeline owned by the renderer; returns its handle (index).
    std::expected<u32, std::string> newTrianglePipeline(Renderer& renderer, std::string_view shaderName);
    void drawTriangle(Renderer& renderer, u32 pipelineHandle);
}

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
namespace se
{
    namespace
    {
        // Converts a Vulkan-Hpp ResultValue to std::expected, checked at the call site.
        template <typename T>
        std::expected<T, std::string> checked(vk::ResultValue<T> rv, std::string_view what)
        {
            if (rv.result != vk::Result::eSuccess)
            {
                return std::unexpected(std::format("{}: {}", what, vk::to_string(rv.result)));
            }
            return std::move(rv.value);
        }

        std::expected<void, std::string> checked(vk::Result result, std::string_view what)
        {
            if (result != vk::Result::eSuccess)
            {
                return std::unexpected(std::format("{}: {}", what, vk::to_string(result)));
            }
            return {};
        }

        void transitionImage(
            vk::CommandBuffer cmd,
            vk::Image image,
            vk::ImageLayout oldLayout,
            vk::ImageLayout newLayout,
            vk::PipelineStageFlags2 srcStage,
            vk::AccessFlags2 srcAccess,
            vk::PipelineStageFlags2 dstStage,
            vk::AccessFlags2 dstAccess)
        {
            vk::ImageMemoryBarrier2 barrier{};
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.image = image;
            barrier.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

            vk::DependencyInfo dependency{};
            dependency.setImageMemoryBarriers(barrier);
            cmd.pipelineBarrier2(dependency);
        }

        void destroySwapchainResources(Renderer& renderer)
        {
            for (vk::ImageView view : renderer.swapchainImageViews)
            {
                renderer.device.destroyImageView(view);
            }
            renderer.swapchainImageViews.clear();

            for (vk::Semaphore semaphore : renderer.renderFinished)
            {
                renderer.device.destroySemaphore(semaphore);
            }
            renderer.renderFinished.clear();

            if (renderer.swapchain)
            {
                renderer.device.destroySwapchainKHR(renderer.swapchain);
                renderer.swapchain = nullptr;
            }
        }

        std::expected<void, std::string> buildSwapchain(Renderer& renderer, u32 width, u32 height)
        {
            vkb::SwapchainBuilder builder{ renderer.vkbDevice };
            builder.set_desired_format(VkSurfaceFormatKHR{
                       .format = VK_FORMAT_B8G8R8A8_UNORM,
                       .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                .set_desired_extent(width, height)
                .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

            if (renderer.swapchain)
            {
                builder.set_old_swapchain(static_cast<VkSwapchainKHR>(renderer.swapchain));
            }

            auto result = builder.build();
            if (!result)
            {
                return std::unexpected(std::format("swapchain build failed: {}", result.error().message()));
            }

            destroySwapchainResources(renderer);

            vkb::Swapchain swapchain = result.value();
            renderer.swapchain = vk::SwapchainKHR{ swapchain.swapchain };
            renderer.swapchainFormat = vk::Format{ static_cast<vk::Format>(swapchain.image_format) };
            renderer.swapchainExtent = vk::Extent2D{ swapchain.extent.width, swapchain.extent.height };

            renderer.swapchainImages.clear();
            for (VkImage image : swapchain.get_images().value())
            {
                renderer.swapchainImages.push_back(vk::Image{ image });
            }

            renderer.swapchainImageViews.clear();
            for (vk::Image image : renderer.swapchainImages)
            {
                vk::ImageViewCreateInfo viewInfo{};
                viewInfo.image = image;
                viewInfo.viewType = vk::ImageViewType::e2D;
                viewInfo.format = renderer.swapchainFormat;
                viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
                auto view = checked(renderer.device.createImageView(viewInfo), "createImageView");
                if (!view)
                {
                    return std::unexpected(view.error());
                }
                renderer.swapchainImageViews.push_back(*view);
            }

            renderer.renderFinished.clear();
            for (std::size_t i = 0; i < renderer.swapchainImages.size(); i = i + 1)
            {
                auto semaphore = checked(renderer.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
                if (!semaphore)
                {
                    return std::unexpected(semaphore.error());
                }
                renderer.renderFinished.push_back(*semaphore);
            }

            renderer.imagesInFlight.assign(renderer.swapchainImages.size(), vk::Fence{});
            return {};
        }

        void recreateSwapchain(Renderer& renderer)
        {
            u32 width = renderer.window->width;
            u32 height = renderer.window->height;
            if (width == 0 || height == 0)
            {
                return;  // minimized — keep the old swapchain, retry once restored
            }
            static_cast<void>(renderer.device.waitIdle());
            auto built = buildSwapchain(renderer, width, height);
            if (!built)
            {
                logError(built.error());
            }
        }

        std::expected<vk::ShaderModule, std::string> loadShaderModule(vk::Device device, const std::string& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                return std::unexpected(std::format("cannot open shader '{}'", path));
            }
            std::streamsize size = file.tellg();
            if (size <= 0 || (size % 4) != 0)
            {
                return std::unexpected(std::format("invalid spir-v size for '{}'", path));
            }
            std::vector<u32> code(static_cast<std::size_t>(size) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), size);

            vk::ShaderModuleCreateInfo info{};
            info.codeSize = static_cast<std::size_t>(size);
            info.pCode = code.data();
            return checked(device.createShaderModule(info), std::format("createShaderModule '{}'", path));
        }
    }

    std::expected<Renderer, std::string> newRenderer(Window& window)
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
            return std::unexpected(std::format("instance creation failed: {}", instanceResult.error().message()));
        }
        renderer.vkbInstance = instanceResult.value();
        renderer.instance = vk::Instance{ renderer.vkbInstance.instance };

        VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window.handle, renderer.vkbInstance.instance, nullptr, &rawSurface))
        {
            return std::unexpected(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
        }
        renderer.surface = vk::SurfaceKHR{ rawSurface };

        VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        features11.shaderDrawParameters = VK_TRUE;  // Slang SV_VertexID emits the DrawParameters capability

        VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{ renderer.vkbInstance };
        auto physicalResult = selector
                                  .set_minimum_version(1, 3)
                                  .set_required_features_11(features11)
                                  .set_required_features_13(features13)
                                  .set_surface(rawSurface)
                                  .select();
        if (!physicalResult)
        {
            return std::unexpected(std::format("no suitable GPU: {}", physicalResult.error().message()));
        }

        vkb::DeviceBuilder deviceBuilder{ physicalResult.value() };
        auto deviceResult = deviceBuilder.build();
        if (!deviceResult)
        {
            return std::unexpected(std::format("device creation failed: {}", deviceResult.error().message()));
        }
        renderer.vkbDevice = deviceResult.value();
        renderer.physicalDevice = vk::PhysicalDevice{ physicalResult.value().physical_device };
        renderer.device = vk::Device{ renderer.vkbDevice.device };

        auto queueResult = renderer.vkbDevice.get_queue(vkb::QueueType::graphics);
        if (!queueResult)
        {
            return std::unexpected(std::format("no graphics queue: {}", queueResult.error().message()));
        }
        renderer.graphicsQueue = vk::Queue{ queueResult.value() };
        renderer.graphicsQueueFamily = renderer.vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.instance = renderer.vkbInstance.instance;
        allocatorInfo.physicalDevice = physicalResult.value().physical_device;
        allocatorInfo.device = renderer.vkbDevice.device;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&allocatorInfo, &renderer.allocator) != VK_SUCCESS)
        {
            return std::unexpected(std::string{ "vmaCreateAllocator failed" });
        }

        auto swapchainBuilt = buildSwapchain(renderer, window.width, window.height);
        if (!swapchainBuilt)
        {
            return std::unexpected(swapchainBuilt.error());
        }

        for (FrameData& frame : renderer.frames)
        {
            vk::CommandPoolCreateInfo poolInfo{};
            poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
            poolInfo.queueFamilyIndex = renderer.graphicsQueueFamily;
            auto pool = checked(renderer.device.createCommandPool(poolInfo), "createCommandPool");
            if (!pool)
            {
                return std::unexpected(pool.error());
            }
            frame.commandPool = *pool;

            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto buffers = checked(renderer.device.allocateCommandBuffers(allocInfo), "allocateCommandBuffers");
            if (!buffers)
            {
                return std::unexpected(buffers.error());
            }
            frame.commandBuffer = (*buffers)[0];

            auto imageAvailable = checked(renderer.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
            if (!imageAvailable)
            {
                return std::unexpected(imageAvailable.error());
            }
            frame.imageAvailable = *imageAvailable;

            vk::FenceCreateInfo fenceInfo{};
            fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
            auto fence = checked(renderer.device.createFence(fenceInfo), "createFence");
            if (!fence)
            {
                return std::unexpected(fence.error());
            }
            frame.inFlight = *fence;
        }

        logInfo(std::format("vulkan ready — gpu '{}', {} swapchain images",
                            renderer.vkbDevice.physical_device.name,
                            renderer.swapchainImages.size()));
        return renderer;
    }

    void destroyRenderer(Renderer& renderer)
    {
        if (renderer.device)
        {
            static_cast<void>(renderer.device.waitIdle());
        }

        renderer.pipelines.clear();  // RAII frees them while the device is still alive

        for (FrameData& frame : renderer.frames)
        {
            renderer.device.destroyFence(frame.inFlight);
            renderer.device.destroySemaphore(frame.imageAvailable);
            renderer.device.destroyCommandPool(frame.commandPool);
        }

        destroySwapchainResources(renderer);

        if (renderer.allocator != nullptr)
        {
            vmaDestroyAllocator(renderer.allocator);
            renderer.allocator = nullptr;
        }
        if (renderer.surface)
        {
            vkb::destroy_surface(renderer.vkbInstance, static_cast<VkSurfaceKHR>(renderer.surface));
        }
        vkb::destroy_device(renderer.vkbDevice);
        vkb::destroy_instance(renderer.vkbInstance);
    }

    bool beginFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];

        static_cast<void>(renderer.device.waitForFences(frame.inFlight, VK_TRUE, UINT64_MAX));

        vk::ResultValue<u32> acquire = renderer.device.acquireNextImageKHR(
            renderer.swapchain, UINT64_MAX, frame.imageAvailable, nullptr);
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
        renderer.imageIndex = acquire.value;

        // Ensure the previous frame that used THIS image has finished before we
        // reuse the image's renderFinished semaphore.
        if (renderer.imagesInFlight[renderer.imageIndex])
        {
            static_cast<void>(renderer.device.waitForFences(renderer.imagesInFlight[renderer.imageIndex], VK_TRUE, UINT64_MAX));
        }
        renderer.imagesInFlight[renderer.imageIndex] = frame.inFlight;

        static_cast<void>(renderer.device.resetFences(frame.inFlight));
        static_cast<void>(frame.commandBuffer.reset());
        renderer.submissions.clear();

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(frame.commandBuffer.begin(beginInfo));

        transitionImage(
            frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex],
            vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);

        vk::RenderingAttachmentInfo colorAttachment{};
        colorAttachment.imageView = renderer.swapchainImageViews[renderer.imageIndex];
        colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
        colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
        colorAttachment.clearValue = vk::ClearValue{ vk::ClearColorValue{ renderer.clearColor } };

        vk::RenderingInfo renderingInfo{};
        renderingInfo.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, renderer.swapchainExtent };
        renderingInfo.layerCount = 1;
        renderingInfo.setColorAttachments(colorAttachment);
        frame.commandBuffer.beginRendering(renderingInfo);

        vk::Viewport viewport{ 0.0f, 0.0f,
                               static_cast<f32>(renderer.swapchainExtent.width),
                               static_cast<f32>(renderer.swapchainExtent.height),
                               0.0f, 1.0f };
        vk::Rect2D scissor{ vk::Offset2D{ 0, 0 }, renderer.swapchainExtent };
        frame.commandBuffer.setViewport(0, viewport);
        frame.commandBuffer.setScissor(0, scissor);

        return true;
    }

    void submit(Renderer& renderer, RenderFn fn)
    {
        renderer.submissions.push_back(std::move(fn));
    }

    void endFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];

        for (RenderFn& fn : renderer.submissions)
        {
            fn(frame.commandBuffer);
        }

        frame.commandBuffer.endRendering();

        transitionImage(
            frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex],
            vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
            vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone);

        static_cast<void>(frame.commandBuffer.end());

        vk::Semaphore signalSemaphore = renderer.renderFinished[renderer.imageIndex];

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
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, frame.inFlight));

        vk::PresentInfoKHR presentInfo{};
        presentInfo.setWaitSemaphores(signalSemaphore);
        presentInfo.setSwapchains(renderer.swapchain);
        presentInfo.setImageIndices(renderer.imageIndex);
        vk::Result present = renderer.graphicsQueue.presentKHR(presentInfo);
        if (present == vk::Result::eErrorOutOfDateKHR || present == vk::Result::eSuboptimalKHR)
        {
            recreateSwapchain(renderer);
        }

        renderer.frameIndex = (renderer.frameIndex + 1) % MaxFramesInFlight;
    }

    std::string assetPath(std::string_view relative)
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

    std::expected<u32, std::string> newTrianglePipeline(Renderer& renderer, std::string_view shaderName)
    {
        std::string path = assetPath(shaderName);
        auto moduleResult = loadShaderModule(renderer.device, path);
        if (!moduleResult)
        {
            return std::unexpected(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";

        vk::PipelineVertexInputStateCreateInfo vertexInput{};

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
        renderingInfo.setColorAttachmentFormats(renderer.swapchainFormat);

        auto layoutResult = checked(renderer.device.createPipelineLayout(vk::PipelineLayoutCreateInfo{}), "createPipelineLayout");
        if (!layoutResult)
        {
            renderer.device.destroyShaderModule(shaderModule);
            return std::unexpected(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stages);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.device.destroyShaderModule(shaderModule);  // baked into the pipeline now
        if (created.result != vk::Result::eSuccess)
        {
            renderer.device.destroyPipelineLayout(*layoutResult);
            return std::unexpected(std::format("createGraphicsPipeline: {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;

        u32 handle = static_cast<u32>(renderer.pipelines.size());
        renderer.pipelines.push_back(std::move(pipeline));
        return handle;
    }

    void drawTriangle(Renderer& renderer, u32 pipelineHandle)
    {
        if (pipelineHandle >= renderer.pipelines.size())
        {
            return;
        }
        vk::Pipeline handle = renderer.pipelines[pipelineHandle].pipeline;
        submit(renderer, [handle](vk::CommandBuffer cmd)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
            cmd.draw(3, 1, 0, 0);
        });
    }
}
