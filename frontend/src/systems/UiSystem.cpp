#include "rgv/systems/UiSystem.h"

#include "rgv/ecs/Resources.h"
#include "rgv/ui/Panels.h"
#include "rgv/ui/Theme.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

namespace rgv::systems {

void UiSystem::setup(ecs::World& world) {
    auto* window = static_cast<GLFWwindow*>(world.resource<ecs::WindowHandle>().window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // panel geometry is code, not user state
    ui::apply_imgui_style();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
}

void UiSystem::teardown(ecs::World&) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UiSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto& timing = world.resource<ecs::FrameTiming>();
    const float instant = frame.dt > 0.0f ? 1.0f / frame.dt : 60.0f;
    timing.fps      = timing.fps * 0.92f + instant * 0.08f;
    timing.frame_ms = frame.dt * 1000.0f;

    // Panel chrome scales with the same preference as the graph labels, so "text size"
    // means one thing everywhere.
    ImGui::GetIO().FontGlobalScale = world.resource<ecs::ViewSettings>().ui_text_scale;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ui::draw_panels(world);
}

void OverlaySystem::run(ecs::World& world, const ecs::FrameContext&) {
    ui::draw_graph_overlay(world, ImGui::GetBackgroundDrawList());
    ui::draw_hover_card(world);
    ui::draw_text_settings(world);
}

void PresentSystem::run(ecs::World& world, const ecs::FrameContext&) {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (auto* window = static_cast<GLFWwindow*>(world.resource<ecs::WindowHandle>().window)) {
        glfwSwapBuffers(window);
    }
}

} // namespace rgv::systems
