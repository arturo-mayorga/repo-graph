#include "rgv/systems/WindowSystem.h"

#include "rgv/ecs/Resources.h"
#include "rgv/render/GL.h"

#include <imgui.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <stdexcept>

namespace rgv::systems {
namespace {

void glfw_error(int code, const char* desc) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

} // namespace

void WindowSystem::setup(ecs::World& world) {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    // Real fullscreen through GLFW rather than a window-manager hint: demoing the
    // interaction should not depend on which compositor is running.
    GLFWmonitor*       monitor = config_.fullscreen ? glfwGetPrimaryMonitor() : nullptr;
    const GLFWvidmode* mode    = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (mode) {
        glfwWindowHint(GLFW_RED_BITS, mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
    }

    GLFWwindow* window = glfwCreateWindow(mode ? mode->width : config_.width,
                                          mode ? mode->height : config_.height,
                                          config_.title.c_str(), monitor, nullptr);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("failed to create a GL 3.3 core context");
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(config_.vsync ? 1 : 0);

    if (!gl::load(reinterpret_cast<gl::ProcLoader>(glfwGetProcAddress))) {
        throw std::runtime_error(std::string("missing OpenGL entry points: ") + gl::missing());
    }
    std::printf("GL %s | %s\n", reinterpret_cast<const char*>(gl::glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(gl::glGetString(GL_RENDERER)));
    std::fflush(stdout);

    world.resource<ecs::WindowHandle>().window = window;
}

void WindowSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto& handle = world.resource<ecs::WindowHandle>();
    auto* window = static_cast<GLFWwindow*>(handle.window);
    if (!window) return;

    glfwPollEvents();
    handle.should_close = glfwWindowShouldClose(window) != 0;

    auto& viewport = world.resource<ecs::Viewport>();
    int   fw = 0, fh = 0, ww = 0, wh = 0;
    glfwGetFramebufferSize(window, &fw, &fh);
    glfwGetWindowSize(window, &ww, &wh);
    viewport.framebuffer_w = static_cast<float>(std::max(1, fw));
    viewport.framebuffer_h = static_cast<float>(std::max(1, fh));

    // The framebuffer may be scaled relative to window coordinates on hidpi displays;
    // everything downstream works in framebuffer pixels.
    const float sx = static_cast<float>(fw) / static_cast<float>(std::max(1, ww));
    const float sy = static_cast<float>(fh) / static_cast<float>(std::max(1, wh));

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    const Vec2 mouse{static_cast<float>(mx) * sx, static_cast<float>(my) * sy};

    auto& input       = world.resource<ecs::FrameInput>();
    input.mouse_delta = mouse - last_mouse;
    input.mouse       = mouse;
    last_mouse        = mouse;

    const bool down    = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    input.mouse_pressed  = down && !was_down_;
    input.mouse_released = !down && was_down_;
    input.mouse_down     = down;
    was_down_            = down;

    // Double click tracked here rather than read from ImGui, so input has a single
    // origin and does not depend on where in the frame it is sampled.
    input.double_click = false;
    if (input.mouse_pressed) {
        if (last_click_time_ >= 0.0 && frame.time - last_click_time_ < 0.30) {
            input.double_click = true;
            last_click_time_   = -1.0;
        } else {
            last_click_time_ = frame.time;
        }
    }

    const ImGuiIO& io      = ImGui::GetIO();
    input.wheel            = io.MouseWheel;   // the GLFW backend accumulates it
    input.ui_wants_mouse   = io.WantCaptureMouse;
    input.ui_wants_keyboard = io.WantCaptureKeyboard;

    auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
    static bool prev_f = false, prev_esc = false, prev_space = false, prev_dot = false,
                prev_r = false;
    const bool f = key(GLFW_KEY_F), esc = key(GLFW_KEY_ESCAPE), space = key(GLFW_KEY_SPACE),
               dot = key(GLFW_KEY_PERIOD), r = key(GLFW_KEY_R);
    const bool typing = input.ui_wants_keyboard;

    input.fit_pressed     = !typing && f && !prev_f;
    input.escape_pressed  = !typing && esc && !prev_esc;
    input.play_pressed    = !typing && space && !prev_space;
    input.step_pressed    = !typing && dot && !prev_dot;
    input.restart_pressed = !typing && r && !prev_r;
    prev_f = f; prev_esc = esc; prev_space = space; prev_dot = dot; prev_r = r;
}

void WindowSystem::teardown(ecs::World& world) {
    auto& handle = world.resource<ecs::WindowHandle>();
    if (auto* window = static_cast<GLFWwindow*>(handle.window)) glfwDestroyWindow(window);
    handle.window = nullptr;
    glfwTerminate();
}

} // namespace rgv::systems
