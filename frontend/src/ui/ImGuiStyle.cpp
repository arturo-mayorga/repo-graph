// The ImGui half of the theme. Kept apart from Theme.cpp so that the colour rules --
// which encode "stale must never read as current" -- stay in a library that builds and
// tests without a UI toolkit or a GPU.
#include "rgv/ui/Theme.h"

#include <imgui.h>

namespace rgv::ui {

void apply_imgui_style() {
    const Theme& t     = theme();
    ImGuiStyle&  style = ImGui::GetStyle();

    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.TabRounding       = 3.0f;
    style.WindowPadding     = ImVec2(10, 8);
    style.FramePadding      = ImVec2(7, 4);
    style.ItemSpacing       = ImVec2(7, 5);
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;

    auto col = [](float r, float g, float b, float a) { return ImVec4(r, g, b, a); };
    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]        = col(0.085f, 0.090f, 0.105f, 1.00f);
    c[ImGuiCol_ChildBg]         = col(0.070f, 0.075f, 0.090f, 1.00f);
    c[ImGuiCol_PopupBg]         = col(0.090f, 0.095f, 0.115f, 0.98f);
    c[ImGuiCol_Border]          = col(0.180f, 0.195f, 0.230f, 1.00f);
    c[ImGuiCol_FrameBg]         = col(0.140f, 0.150f, 0.180f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = col(0.190f, 0.205f, 0.245f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = col(0.230f, 0.250f, 0.300f, 1.00f);
    c[ImGuiCol_TitleBg]         = col(0.070f, 0.075f, 0.090f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = col(0.110f, 0.120f, 0.145f, 1.00f);
    c[ImGuiCol_Header]          = col(0.180f, 0.200f, 0.250f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = col(0.230f, 0.255f, 0.315f, 1.00f);
    c[ImGuiCol_HeaderActive]    = col(0.270f, 0.300f, 0.370f, 1.00f);
    c[ImGuiCol_Button]          = col(0.160f, 0.175f, 0.215f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = col(0.220f, 0.240f, 0.300f, 1.00f);
    c[ImGuiCol_ButtonActive]    = col(0.280f, 0.310f, 0.390f, 1.00f);
    c[ImGuiCol_Tab]             = col(0.110f, 0.120f, 0.145f, 1.00f);
    c[ImGuiCol_TabHovered]      = col(0.230f, 0.255f, 0.315f, 1.00f);
    c[ImGuiCol_TabActive]       = col(0.180f, 0.200f, 0.250f, 1.00f);
    c[ImGuiCol_SliderGrab]      = col(0.400f, 0.450f, 0.560f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= col(0.520f, 0.580f, 0.700f, 1.00f);
    c[ImGuiCol_CheckMark]       = col(t.direct.r, t.direct.g, t.direct.b, 1.00f);
    c[ImGuiCol_Text]            = col(0.820f, 0.845f, 0.890f, 1.00f);
    c[ImGuiCol_TextDisabled]    = col(0.420f, 0.445f, 0.500f, 1.00f);
    c[ImGuiCol_Separator]       = col(0.170f, 0.185f, 0.220f, 1.00f);
    c[ImGuiCol_TableHeaderBg]   = col(0.120f, 0.130f, 0.158f, 1.00f);
    c[ImGuiCol_TableBorderLight]= col(0.150f, 0.165f, 0.200f, 1.00f);
}

} // namespace rgv::ui
