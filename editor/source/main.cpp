// imgui.h is a heavy C++ header, so this TU uses classic includes (no `import
// std`) — consistent with the engine's rendering/ui/scene modules.
#include <imgui.h>

#include <expected>
#include <memory>
#include <string>
#include <utility>

import Saffron.Core;
import Saffron.App;
import Saffron.Window;
import Saffron.Scene;
import Saffron.Rendering;
import Saffron.Ui;

namespace
{
    constexpr se::i32 KeyEscape = 27;  // SDLK_ESCAPE

    // State shared across the app lifecycle closures. Holds only a pipeline
    // handle — the renderer owns the pipeline itself.
    struct EditorState
    {
        se::u32 trianglePipeline = 0;
        bool pipelineReady = false;
    };
}

int main()
{
    auto state = std::make_shared<EditorState>();

    se::AppConfig config;
    config.window = se::WindowConfig{ .title = "Saffron Editor", .width = 1600, .height = 900 };

    config.onCreate = [state](se::App& app)
    {
        se::runSceneSelfTest();

        std::expected<se::u32, std::string> pipeline =
            se::newTrianglePipeline(app.renderer, "shaders/triangle.spv");
        if (pipeline)
        {
            state->trianglePipeline = *pipeline;
            state->pipelineReady = true;
            se::logInfo("triangle pipeline ready");
        }
        else
        {
            se::logError(pipeline.error());
        }

        se::Layer layer;
        layer.name = "EditorLayer";
        layer.onAttach = []() { se::logInfo("editor layer attached"); };
        layer.onRender = [state, &app]()
        {
            if (state->pipelineReady)
            {
                se::drawTriangle(app.renderer, state->trianglePipeline);
            }
        };
        layer.onUi = [&app]()
        {
            se::viewportPanel(app.ui, app.renderer);
            ImGui::ShowDemoWindow();
            ImGui::Begin("Saffron");
            ImGui::Text("FPS: %.1f", static_cast<double>(ImGui::GetIO().Framerate));
            ImGui::End();
        };
        layer.onDetach = []() { se::logInfo("editor layer detached"); };
        se::attachLayer(app, std::move(layer));

        app.window.onKeyPressed.subscribe([&app](se::i32 key, bool isRepeat)
        {
            static_cast<void>(isRepeat);
            if (key == KeyEscape)
            {
                app.window.shouldClose = true;
            }
            return false;
        });
    };

    return se::run(std::move(config));
}
