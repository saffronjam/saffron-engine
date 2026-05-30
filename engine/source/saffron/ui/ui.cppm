module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <array>
#include <expected>
#include <format>
#include <string>

export module Saffron.Ui;

import Saffron.Core;
import Saffron.Window;
import Saffron.Rendering;

export namespace se
{
    struct Ui
    {
        vk::DescriptorPool descriptorPool;
        bool initialized = false;
    };

    std::expected<Ui, std::string> newUi(Renderer& renderer, Window& window);
    void destroyUi(Renderer& renderer, Ui& ui);

    void uiBeginFrame(Ui& ui);                  // NewFrame + dockspace host
    void uiEndFrame(Ui& ui);                    // ImGui::Render()
    void uiRecordDrawData(Renderer& renderer);  // submit draw data into the frame
}

namespace se
{
    std::expected<Ui, std::string> newUi(Renderer& renderer, Window& window)
    {
        Ui ui;

        std::array<vk::DescriptorPoolSize, 3> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 1000 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eSampler, 1000 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eSampledImage, 1000 },
        };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        poolInfo.maxSets = 1000;
        poolInfo.setPoolSizes(poolSizes);
        vk::ResultValue<vk::DescriptorPool> pool = renderer.device.createDescriptorPool(poolInfo);
        if (pool.result != vk::Result::eSuccess)
        {
            return std::unexpected(std::format("createDescriptorPool: {}", vk::to_string(pool.result)));
        }
        ui.descriptorPool = pool.value;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForVulkan(window.handle))
        {
            return std::unexpected(std::string{ "ImGui_ImplSDL3_InitForVulkan failed" });
        }

        // ImGui consumes the color format during Init (when it builds its pipeline),
        // so a local is fine — the pointer is not read afterwards.
        VkFormat colorFormat = static_cast<VkFormat>(renderer.swapchainFormat);

        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = renderer.instance;
        init.PhysicalDevice = renderer.physicalDevice;
        init.Device = renderer.device;
        init.QueueFamily = renderer.graphicsQueueFamily;
        init.Queue = renderer.graphicsQueue;
        init.DescriptorPool = ui.descriptorPool;
        init.MinImageCount = 2;
        init.ImageCount = static_cast<u32>(renderer.swapchainImages.size());
        init.UseDynamicRendering = true;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
        if (!ImGui_ImplVulkan_Init(&init))
        {
            return std::unexpected(std::string{ "ImGui_ImplVulkan_Init failed" });
        }

        window.eventSinks.push_back([](const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); });

        ui.initialized = true;
        logInfo("imgui ready — docking enabled");
        return ui;
    }

    void destroyUi(Renderer& renderer, Ui& ui)
    {
        if (!ui.initialized)
        {
            return;
        }
        static_cast<void>(renderer.device.waitIdle());
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        renderer.device.destroyDescriptorPool(ui.descriptorPool);
        ui.descriptorPool = nullptr;
        ui.initialized = false;
    }

    void uiBeginFrame(Ui& ui)
    {
        static_cast<void>(ui);
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport();
    }

    void uiEndFrame(Ui& ui)
    {
        static_cast<void>(ui);
        ImGui::Render();
    }

    void uiRecordDrawData(Renderer& renderer)
    {
        submit(renderer, [](vk::CommandBuffer cmd)
        {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(cmd));
        });
    }
}
