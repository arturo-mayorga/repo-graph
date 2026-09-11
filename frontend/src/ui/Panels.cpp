#include "rgv/ui/Panels.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/view/SemanticZoom.h"

#include "rgv/analysis/Specificity.h"
#include "rgv/config/Settings.h"
#include "rgv/sim/ImpactSim.h"
#include "rgv/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace rgv::ui {
namespace {

// Gathers the resources the panels touch. Constructed per call rather than stored, so
// a panel can never hold a reference across a frame boundary.
struct Ui {
    ecs::World&          world;
    GraphStore&          store;
    ecs::ViewSettings&   view;
    ecs::Filters&        filters;
    ecs::Selection&      selection;
    ecs::Viewport&       viewport;
    ecs::SceneStats&     stats;
    ecs::DerivedState&   derived;
    ecs::EntityIndex&    index;
    ecs::CommandQueue&   cmd;
    ecs::SourceHandle&   source;
    ecs::FixtureLibrary& library;
    ecs::FrameTiming&    timing;
    Camera&              camera;

    explicit Ui(ecs::World& w)
        : world(w),
          store(w.resource<GraphStore>()),
          view(w.resource<ecs::ViewSettings>()),
          filters(w.resource<ecs::Filters>()),
          selection(w.resource<ecs::Selection>()),
          viewport(w.resource<ecs::Viewport>()),
          stats(w.resource<ecs::SceneStats>()),
          derived(w.resource<ecs::DerivedState>()),
          index(w.resource<ecs::EntityIndex>()),
          cmd(w.resource<ecs::CommandQueue>()),
          source(w.resource<ecs::SourceHandle>()),
          library(w.resource<ecs::FixtureLibrary>()),
          timing(w.resource<ecs::FrameTiming>()),
          camera(w.resource<Camera>()) {}
};


// Panel sizes at 1x text. They scale with the user's text preference -- a panel that
// keeps its pixel width while its text grows simply clips, which is worse than a
// smaller graph area.
constexpr float kLeftW   = 326.0f;
constexpr float kRightW  = 400.0f;
constexpr float kBottomH = 208.0f;

struct PanelMetrics {
    float left = kLeftW, right = kRightW, bottom = kBottomH, top = 64.0f;
};

// Scaled and then clamped: at the largest text size the raw multiples would leave
// almost no graph, and the graph is the product.
PanelMetrics panel_metrics(const Ui& ui) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float          s  = ui.view.ui_text_scale;

    PanelMetrics m;
    m.top    = ui.viewport.top_bar_height;
    m.left   = std::min(kLeftW * s, vp->WorkSize.x * 0.28f);
    m.right  = std::min(kRightW * s, vp->WorkSize.x * 0.34f);
    m.bottom = std::min(kBottomH * s, vp->WorkSize.y * 0.34f);
    m.top    = std::min(m.top, vp->WorkSize.y * 0.25f);
    return m;
}

// Widths of common controls, so the toolbar can decide whether the next one fits
// before drawing it. ImGui gives no way to un-draw something that overflowed.
float button_w(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}
float checkbox_w(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x +
           ImGui::GetFrameHeight();
}

// Lays controls left to right and wraps to a new line when the next one would not
// fit. Without this the toolbar silently clips at large text sizes.
struct Flow {
    float right   = 0.0f;    // screen-space right edge of the content region
    float end     = -1.0f;   // where the previous control ended

    void item(float width) {
        if (end >= 0.0f && end + ImGui::GetStyle().ItemSpacing.x + width <= right) {
            ImGui::SameLine();
        }
    }
    void placed() { end = ImGui::GetItemRectMax().x; }
};

ImU32  to_u32(const Vec4& c) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a)); }
ImVec4 to_v4(const Vec4& c) { return ImVec4(c.r, c.g, c.b, c.a); }

// A small filled pill. Every state that matters gets one, and the pill carries the
// word as well as the colour -- colour alone would make "stale" a thing you have to
// learn, and NFR-04 says stale must never be mistaken for current.
void chip(const char* text, const Vec4& color, const char* tooltip = nullptr) {
    const ImVec2 sz   = ImGui::CalcTextSize(text);
    const ImVec2 pos  = ImGui::GetCursorScreenPos();
    const float  padx = 6.0f, pady = 2.0f;
    ImDrawList*  dl   = ImGui::GetWindowDrawList();
    const ImVec2 br(pos.x + sz.x + padx * 2, pos.y + sz.y + pady * 2);

    dl->AddRectFilled(pos, br, ImGui::GetColorU32(ImVec4(color.r, color.g, color.b, 0.18f)), 3.0f);
    dl->AddRect(pos, br, to_u32(color), 3.0f);
    dl->AddText(ImVec2(pos.x + padx, pos.y + pady), to_u32(color), text);

    ImGui::Dummy(ImVec2(sz.x + padx * 2, sz.y + pady * 2));
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
}

Vec4 freshness_color(Freshness f) {
    const Theme& t = theme();
    switch (f) {
        case Freshness::Current: return Vec4{0.42f, 0.72f, 0.55f, 1.0f};
        case Freshness::Stale:   return t.stale;
        case Freshness::Pending: return t.pending;
        case Freshness::Invalid: return t.invalid;
    }
    return t.pending;
}

Vec4 confidence_color(Confidence c) {
    const Theme& t = theme();
    switch (c) {
        case Confidence::Exact:      return Vec4{0.42f, 0.72f, 0.55f, 1.0f};
        case Confidence::High:       return Vec4{0.50f, 0.65f, 0.80f, 1.0f};
        case Confidence::Heuristic:  return t.heuristic;
        case Confidence::Unresolved: return t.invalid;
    }
    return t.pending;
}

Vec4 adapter_color(AdapterState s) {
    const Theme& t = theme();
    switch (s) {
        case AdapterState::Idle:     return Vec4{0.45f, 0.48f, 0.56f, 1.0f};
        case AdapterState::Running:  return Vec4{0.38f, 0.68f, 0.92f, 1.0f};
        case AdapterState::Degraded: return t.stale;
        case AdapterState::Failed:   return t.invalid;
    }
    return t.pending;
}

Vec4 processing_color(Processing p) {
    switch (p) {
        case Processing::Pending:    return Vec4{0.55f, 0.57f, 0.64f, 1.0f};
        case Processing::Structural: return Vec4{0.38f, 0.68f, 0.92f, 1.0f};
        case Processing::Semantic:   return Vec4{0.55f, 0.74f, 0.55f, 1.0f};
        case Processing::Settled:    return Vec4{0.38f, 0.60f, 0.46f, 1.0f};
    }
    return Vec4{};
}

Vec4 change_color(FileChangeKind k) {
    const Theme& t = theme();
    switch (k) {
        case FileChangeKind::Created:  return t.added;
        case FileChangeKind::Deleted:  return t.removed;
        case FileChangeKind::Renamed:  return t.heuristic;
        case FileChangeKind::Modified: return t.changed;
    }
    return t.changed;
}

// Panels never write selection state. They ask, and CommandSystem is the single place
// that applies it -- which is what stops the id and the Selected component drifting.
void select_node(Ui& ui, const NodeId& id) {
    ui.cmd.push(ecs::SelectNode{id});
}

const ImpactedNode* impacted_for(const GraphStore& store, Level level, const NodeId& id) {
    const ImpactResult* r = store.impact(level);
    if (!r) return nullptr;
    for (const auto& n : r->impacted_nodes) {
        if (n.node_id == id) return &n;
    }
    return nullptr;
}

std::string short_id(const std::string& id) {
    const auto pos = id.find(':');
    return pos == std::string::npos ? id : id.substr(pos + 1);
}

// ------------------------------------------------------------------ top bar

void draw_top_bar(ecs::World& world) {
    Ui ui(world);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, ui.viewport.top_bar_height));
    ImGui::Begin("##topbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);

    auto& vs = ui.view;
    auto& f  = ui.filters;

    Flow flow;
    flow.right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    auto label = [&](const char* text) {
        flow.item(ImGui::CalcTextSize(text).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text);
        flow.placed();
    };

    label("VIEW");
    for (auto m : {ecs::ViewMode::Architecture, ecs::ViewMode::Filesystem,
                   ecs::ViewMode::FileGraph}) {
        const char* name   = ecs::to_label(m);
        const bool  active = vs.mode == m;
        flow.item(button_w(name));
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, to_v4(Vec4{0.26f, 0.30f, 0.39f, 1.0f}));
        if (ImGui::Button(name) && !active) {
            // Selection is deliberately preserved across the switch (FR-30). Only the
            // visible node set changes.
            vs.mode           = m;
            vs.level          = ecs::default_level(m);
            ui.world.resource<ecs::SceneRequests>().rebuild = true;
            ui.cmd.push(ecs::FitView{});
        }
        if (active) ImGui::PopStyleColor();
        flow.placed();
    }

    label("IMPACT AT");
    {
        const float w = 130.0f * vs.ui_text_scale;
        flow.item(w);
        ImGui::SetNextItemWidth(w);
        int lvl = static_cast<int>(vs.level);
        if (ImGui::Combo("##level", &lvl, "package\0build target\0file\0symbol\0")) {
            // Levels coexist, so this is a free switch: no re-query, no lost context.
            vs.level = static_cast<Level>(lvl);
            
        }
        flow.placed();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Which blast-radius result to read.\n"
                              "Independent of the view: you can inspect files while\n"
                              "reading package-level impact.");
        }
    }

    flow.item(button_w("Fit"));
    if (ImGui::Button("Fit")) ui.cmd.push(ecs::FitView{});
    flow.placed();

    // A full layout moves every node on screen, so it is something the user asks for
    // rather than something that happens to them. Nodes coming and going are seated
    // where they belong and left to the relaxation instead.
    flow.item(button_w("Re-layout"));
    if (ImGui::Button("Re-layout")) {
        ui.world.resource<ecs::SceneRequests>().relayout = true;
        ui.cmd.push(ecs::FitView{});
    }
    flow.placed();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lay the whole graph out again from scratch.\n\n"
                          "Filters and edits place what arrives and leave the rest\n"
                          "where it is, so the picture can drift from what a clean\n"
                          "layout would give you. This is the reset.");
    }

    flow.item(button_w("Focus impact"));
    if (ImGui::Button("Focus impact")) {
        std::vector<NodeId> ids;
        if (const ImpactResult* r = ui.store.impact(ui.view.level)) {
            for (const auto& n : r->impacted_nodes) ids.push_back(n.node_id);
        }
        ui.cmd.push(ecs::FocusNodes{std::move(ids)});
    }
    flow.placed();

    flow.item(checkbox_w("Layout"));
    ImGui::Checkbox("Layout", &vs.layout_running);
    flow.placed();

    flow.item(checkbox_w("Labels"));
    ImGui::Checkbox("Labels", &vs.show_labels);
    flow.placed();

    flow.item(checkbox_w("Arrows"));
    ImGui::Checkbox("Arrows", &vs.show_arrows);
    flow.placed();

    // -- filters (FR-35)
    label("FILTER");
    struct Toggle { const char* name; bool* value; const char* tip; };
    const Toggle toggles[] = {
        {"Unaffected", &f.show_unaffected,
         "Off: show only what the agent touched plus affected context."},
        {"Stale", &f.show_stale, nullptr},
        {"Heuristic", &f.show_heuristic, nullptr},
        {"External", &f.show_external, nullptr},
    };
    for (const auto& tg : toggles) {
        flow.item(checkbox_w(tg.name));
        if (ImGui::Checkbox(tg.name, tg.value)) ui.world.resource<ecs::SceneRequests>().revisit = true;
        flow.placed();
        if (tg.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tg.tip);
    }

    {
        const float w = 150.0f * vs.ui_text_scale;
        flow.item(w);
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("depth", &f.max_impact_depth, 1, 12)) {
            
            if (!f.show_unaffected) ui.world.resource<ecs::SceneRequests>().revisit = true;
        }
        flow.placed();

        // Architectural specificity, borrowed from inverse document frequency: a
        // package everything depends on explains nothing, so results that only reach
        // the change through one carry little information.
        flow.item(w);
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("relevance", &f.min_relevance, 0.0f, 1.0f, "%.2f")) {
            // Visibility depends on this now, not just emphasis. A revisit rather than
            // a rebuild: this is dragged, so what stays visible must not move.
            ui.world.resource<ecs::SceneRequests>().revisit = true;
        }
        flow.placed();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Hide impact that only reaches the change through a hub.\n\n"
                "Scored like inverse document frequency: a package most of the repo\n"
                "depends on is the architectural equivalent of the word \"the\", so\n"
                "\"X depends on it\" explains nothing.\n\n"
                "Never applies to what the agent actually changed.");
        }
    }

    {
        const float w = 200.0f * vs.ui_text_scale;
        flow.item(w);
        ImGui::SetNextItemWidth(w);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", f.text.c_str());
        if (ImGui::InputTextWithHint("##search", "filter by name or path", buf, sizeof(buf))) {
            f.text              = buf;
            ui.world.resource<ecs::SceneRequests>().revisit = true;
        }
        flow.placed();
    }

    // Text sizes live in their own window rather than on the toolbar. Editing the UI
    // scale from a control that is itself scaled by it means the control moves and
    // resizes under the cursor mid-drag, which makes hitting a value impossible.
    {
        char label[48];
        // Fixed-width formatting: a label whose length changes with its own value
        // would reflow the toolbar on every drag step.
        std::snprintf(label, sizeof(label), "Text  %.2f / %.2f",
                      static_cast<double>(vs.ui_text_scale),
                      static_cast<double>(vs.graph_text_scale));
        flow.item(button_w(label));
        if (ImGui::Button(label)) vs.show_text_settings = !vs.show_text_settings;
        flow.placed();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("UI and graph text size.");
    }

    {
        char fps[64];
        std::snprintf(fps, sizeof(fps), "%5.1f fps  %4.1f ms", static_cast<double>(ui.timing.fps),
                      static_cast<double>(ui.timing.frame_ms));
        flow.item(ImGui::CalcTextSize(fps).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", fps);
        flow.placed();
    }

    // Measured now, used to size the window on the next frame. Read at the top of this
    // function, written here: the toolbar wraps, so its height is only knowable after
    // its contents have been laid out.
    ui.viewport.top_bar_height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;

    ImGui::End();
}

// --------------------------------------------------------------- left panel

void draw_session_panel(ecs::World& world) {
    Ui ui(world);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const PanelMetrics   m  = panel_metrics(ui);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + m.top));
    ImGui::SetNextWindowSize(ImVec2(m.left, vp->WorkSize.y - m.top - m.bottom));
    ImGui::Begin("Session", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    const auto& base = ui.store.baseline();
    ImGui::TextUnformatted(base.repo.name.c_str());
    ImGui::TextDisabled("%s", base.repo.root.c_str());

    ImGui::Spacing();
    ImGui::Text("%s @ %.8s", base.repo.branch.c_str(), base.repo.head.c_str());
    ImGui::Text("baseline g%llu  ->  current g%llu",
                static_cast<unsigned long long>(base.session.baseline_generation),
                static_cast<unsigned long long>(ui.store.generation()));
    if (!ui.store.checkpoints().empty()) {
        ImGui::SameLine();
        chip("reconciled", Vec4{0.42f, 0.72f, 0.55f, 1.0f},
             "A git reconcile checkpoint has confirmed this generation.");
    }

    // -- the loudest thing this product can say. A change to something most of the
    //    repository depends on: the radius is enormous and every individual entry in
    //    it is unsurprising, so the hub itself has to be the headline.
    for (const auto& hub : ui.derived.hub_alerts) {
        const Node* n   = ui.store.node(hub.node);
        const Vec4  hot = theme().changed;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(hot.r * 0.22f, hot.g * 0.10f,
                                                       hot.b * 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, to_v4(hot));
        ImGui::BeginChild(("##hub" + hub.node).c_str(), ImVec2(0, 0),
                          ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);

        ImGui::TextColored(to_v4(hot), "HUB CHANGE");
        ImGui::TextUnformatted(n ? n->name.c_str() : hub.node.c_str());
        ImGui::TextDisabled("%d of %d depend on it", hub.dependents, hub.population);
        ImGui::TextDisabled("blast radius %d (%.0f%%)", hub.reach,
                            static_cast<double>(hub.reach_fraction * 100.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Everything below follows from this. The individual "
                            "results carry little information on their own.");
        ImGui::PopTextWrapPos();

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }

    ImGui::Separator();

    // -- changed files (FR-09): the agent's footprint, in arrival order.
    const auto& changed = ui.store.changed_files();
    ImGui::Text("Changed files");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", changed.size());

    // Height is what is left after the sections below, not a fixed fraction. A
    // fraction pushes the provider list and the counters off the bottom as soon as
    // the user turns up the UI text.
    {
        const float line     = ImGui::GetTextLineHeightWithSpacing();
        const float reserved = 118.0f * ui.view.ui_text_scale + line * 9.0f;
        const float h = std::max(line * 3.0f, ImGui::GetContentRegionAvail().y - reserved);
        ImGui::BeginChild("##changed", ImVec2(0, h), true);
    }
    if (changed.empty()) {
        ImGui::TextDisabled("nothing changed since the baseline");
    }
    for (const auto& c : changed) {
        ImGui::PushID(c.node_id.c_str());
        const bool selected = ui.selection.node == c.node_id;

        chip(std::string(to_string(c.change)).substr(0, 3).c_str(), change_color(c.change),
             std::string("change: ").append(to_string(c.change)).c_str());
        ImGui::SameLine();

        // The pipeline tier this file is in. This is the only in-flight signal the
        // contract gives per file, so it gets a permanent slot rather than a spinner.
        chip(std::string(to_string(c.processing)).substr(0, 4).c_str(),
             processing_color(c.processing),
             "T0 pending -> T1 structural -> T2 semantic -> settled");
        ImGui::SameLine();

        if (ImGui::Selectable(c.path.c_str(), selected)) {
            select_node(ui, c.node_id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(c.path.c_str());
            if (!c.from_path.empty()) ImGui::Text("renamed from %s", c.from_path.c_str());
            ImGui::Text("generation %llu at %.0f ms",
                        static_cast<unsigned long long>(c.generation), c.t_ms);
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // -- adapters (NFR-02/NFR-09): the UI must show pending work, not freeze on it.
    ImGui::Text("Providers");
    ImGui::BeginChild("##adapters", ImVec2(0, 118.0f * ui.view.ui_text_scale), true);
    if (ui.store.adapters().empty()) ImGui::TextDisabled("no provider has reported yet");
    for (const auto& a : ui.store.adapters()) {
        chip(std::string(to_string(a.state)).c_str(), adapter_color(a.state), a.message.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(a.name.c_str());
        if (a.queue_depth > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("queue %d", a.queue_depth);
        }
    }
    ImGui::EndChild();

    // -- what is on screen right now
    const auto& s = ui.stats;
    ImGui::Separator();
    ImGui::TextDisabled("on screen");
    ImGui::Text("%d nodes   %d edges", s.nodes, s.edges);
    ImGui::Text("%d changed   %d impacted   %d stale", s.changed, s.impacted, s.stale);
    if (s.muted > 0 || s.hidden > 0) {
        ImGui::TextDisabled("%d hidden   %d muted   below relevance %.2f", s.hidden, s.muted,
                            static_cast<double>(ui.filters.min_relevance));
    }
    ImGui::TextDisabled("%d draw calls", ui.stats.render_draw_calls);
    ImGui::TextDisabled("layout energy %.2f%s", static_cast<double>(ui.stats.layout_energy),
                        ui.stats.layout_settled ? " (settled)" : "");

    ImGui::End();
}

// -------------------------------------------------------------- right panel

void draw_node_inspector(Ui& ui, const Node& n) {
    ImGui::TextUnformatted(n.name.c_str());
    ImGui::SameLine();
    chip(std::string(to_string(n.kind)).c_str(), Vec4{0.50f, 0.55f, 0.68f, 1.0f});
    if (!n.path.empty()) ImGui::TextDisabled("%s", n.path.c_str());

    ImGui::Spacing();
    chip(std::string(to_string(n.freshness)).c_str(), freshness_color(n.freshness),
         n.freshness == Freshness::Stale
             ? "Last known good. A re-parse or re-index has not confirmed it."
             : "");
    if (!n.language.empty()) {
        ImGui::SameLine();
        chip(n.language.c_str(), Vec4{0.45f, 0.50f, 0.60f, 1.0f});
    }

    // How much information "something depends on this" carries.
    {
        const auto& idx  = ui.derived.specificity;
        const float spec = idx.specificity(n.id);
        const int   deps = idx.dependents(n.id);
        if (!idx.empty() && idx.population() > 1) {
            ImGui::Spacing();
            ImGui::TextDisabled("SPECIFICITY %.2f", static_cast<double>(spec));
            ImGui::SameLine();
            if (spec <= analysis::kHubThreshold) {
                chip("hub", theme().changed,
                     "Most of the repository depends on this, so \"X depends on it\"\n"
                     "explains almost nothing -- the architectural equivalent of a\n"
                     "stop word. If it CHANGES, though, the blast radius is real.");
            } else {
                chip("specific", Vec4{0.42f, 0.72f, 0.55f, 1.0f},
                     "Few things depend on this, so a dependency on it is informative.");
            }
            ImGui::TextDisabled("%d of %d %s depend on this", deps, idx.population(),
                                to_string(ui.view.level).data());
        }
    }

    if (!n.attrs.empty() && ImGui::CollapsingHeader("Attributes")) {
        if (ImGui::BeginTable("##attrs", 2, ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& [k, v] : n.attrs) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", k.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.c_str());
            }
            ImGui::EndTable();
        }
    }

    ImGui::Separator();

    // -- impact and its explanation (FR-26, FR-33)
    const ImpactedNode* in = impacted_for(ui.store, ui.view.level, n.id);
    if (!in) {
        ImGui::TextDisabled("Not in the current blast radius.");
    } else {
        ImGui::Text("Impact");
        ImGui::SameLine();
        chip(in->changed ? "changed" : (in->direct ? "direct" : "transitive"),
             impact_color(in->min_distance));
        ImGui::SameLine();
        ImGui::TextDisabled("distance %d", in->min_distance);
        ImGui::SameLine();
        chip(std::string(to_string(in->cause)).c_str(),
             in->cause == ImpactCause::Implementation ? Vec4{0.50f, 0.55f, 0.68f, 1.0f}
                                                      : theme().added,
             "Why this node is in the radius: a changed implementation, or a\n"
             "dependency relationship that was added or removed (FR-29).");
        if (in->freshness != Freshness::Current) {
            ImGui::SameLine();
            chip(std::string(to_string(in->freshness)).c_str(), freshness_color(in->freshness),
                 "At least one hop on this path rests on evidence that is not current.");
        }

        // How much this result is worth reading, as opposed to how close it is.
        if (in->min_distance > 0) {
            const float rel = analysis::relevance(ui.store, ui.derived.specificity, *in);
            ImGui::TextDisabled("relevance %.2f", static_cast<double>(rel));
            ImGui::SameLine();
            if (rel <= analysis::kHubThreshold) {
                chip("via hub", theme().pending,
                     "This is only impacted through a package most of the repo depends\n"
                     "on, so its presence here says little. Raise the relevance filter\n"
                     "to hide results like it.");
            }
            if (rel < ui.filters.min_relevance) {
                ImGui::SameLine();
                chip("muted", theme().pending, "Below the current relevance filter.");
            }
        }

        if (in->paths.empty()) {
            ImGui::TextDisabled("seed node -- nothing to explain");
        } else {
            const int count = static_cast<int>(in->paths.size());
            ui.selection.path_index = std::clamp(ui.selection.path_index, 0, count - 1);

            ImGui::Spacing();
            ImGui::Text("Why:");
            ImGui::SameLine();
            if (count > 1) {
                if (ImGui::SmallButton("<")) {
                    ui.selection.path_index = (ui.selection.path_index + count - 1) % count;
                    
                }
                ImGui::SameLine();
                ImGui::Text("path %d/%d", ui.selection.path_index + 1, count);
                ImGui::SameLine();
                if (ImGui::SmallButton(">")) {
                    ui.selection.path_index = (ui.selection.path_index + 1) % count;
                    
                }
            } else {
                ImGui::TextDisabled("1 path");
            }
            if (in->paths_truncated) {
                ImGui::SameLine();
                chip("truncated", theme().stale, "The backend capped path enumeration.");
            }

            // The chain, hop by hop. Every hop is clickable and opens the evidence
            // behind that specific relationship -- the difference between a claim and
            // an explanation.
            const auto& path = in->paths[ui.selection.path_index].edges;
            NodeId      cur  = n.id;
            for (std::size_t hop = 0; hop < path.size(); ++hop) {
                const Edge* e = ui.store.edge(path[hop]);
                if (!e) {
                    ImGui::TextColored(to_v4(theme().invalid), "  ? unknown edge %s",
                                       path[hop].c_str());
                    break;
                }
                const Node* dst = ui.store.node(e->to);
                ImGui::PushID(static_cast<int>(hop));
                ImGui::Text("  %zu.", hop + 1);
                ImGui::SameLine();
                chip(std::string(to_string(e->kind)).c_str(), Vec4{0.45f, 0.50f, 0.62f, 1.0f});
                ImGui::SameLine();
                if (ImGui::SmallButton(dst ? dst->name.c_str() : short_id(e->to).c_str())) {
                    select_node(ui, e->to);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("evidence")) ui.selection.edge = e->id;
                if (e->confidence != Confidence::Exact || e->freshness != Freshness::Current) {
                    ImGui::SameLine();
                    chip(e->freshness != Freshness::Current
                             ? std::string(to_string(e->freshness)).c_str()
                             : std::string(to_string(e->confidence)).c_str(),
                         e->freshness != Freshness::Current ? freshness_color(e->freshness)
                                                            : confidence_color(e->confidence));
                }
                ImGui::PopID();
                cur = e->to;
            }
            ImGui::TextDisabled("  -> %s (changed)", short_id(cur).c_str());
        }
    }

    ImGui::Separator();

    // -- neighbourhood, so the user can walk the graph without hunting on canvas
    const auto& dependents   = ui.store.in_edges(n.id);
    const auto& dependencies = ui.store.out_edges(n.id);

    if (ImGui::CollapsingHeader("Dependents", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (dependents.empty()) ImGui::TextDisabled("  nothing depends on this");
        for (const auto& eid : dependents) {
            const Edge* e = ui.store.edge(eid);
            if (!e) continue;
            const Node* other = ui.store.node(e->from);
            ImGui::PushID(eid.c_str());
            if (ImGui::SmallButton(other ? other->name.c_str() : short_id(e->from).c_str())) {
                select_node(ui, e->from);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", std::string(to_string(e->kind)).c_str());
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Dependencies")) {
        if (dependencies.empty()) ImGui::TextDisabled("  depends on nothing in this repo");
        for (const auto& eid : dependencies) {
            const Edge* e = ui.store.edge(eid);
            if (!e) continue;
            const Node* other = ui.store.node(e->to);
            ImGui::PushID(eid.c_str());
            if (ImGui::SmallButton(other ? other->name.c_str() : short_id(e->to).c_str())) {
                select_node(ui, e->to);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", std::string(to_string(e->kind)).c_str());
            ImGui::PopID();
        }
    }

    // Speculative probe: what would break if THIS changed? Answered locally, and
    // labelled as a simulation so it is never confused with backend truth.
    if (ImGui::Button("Simulate: what if this changed?")) {
        ImGui::OpenPopup("##whatif");
    }
    if (ImGui::BeginPopup("##whatif")) {
        ImGui::TextDisabled("computed in the frontend, not reported by a provider");
        const auto r = sim::compute(ui.store, {n.id}, ui.view.level,
                                    ImpactFilters{ui.filters.max_impact_depth,
                                                  {EdgeKind::DependsOn, EdgeKind::Imports},
                                                  ui.filters.show_heuristic});
        ImGui::Text("%zu node(s) would be in the radius", r.impacted_nodes.size());
        for (const auto& x : r.impacted_nodes) {
            if (x.min_distance == 0) continue;
            ImGui::BulletText("d=%d  %s", x.min_distance, short_id(x.node_id).c_str());
        }
        ImGui::EndPopup();
    }
}

void draw_edge_inspector(Ui& ui, const Edge& e) {
    const Node* from = ui.store.node(e.from);
    const Node* to   = ui.store.node(e.to);

    ImGui::TextUnformatted("Relationship");
    ImGui::SameLine();
    chip(std::string(to_string(e.kind)).c_str(), Vec4{0.50f, 0.55f, 0.68f, 1.0f});

    ImGui::Spacing();
    if (ImGui::SmallButton(from ? from->name.c_str() : short_id(e.from).c_str())) {
        select_node(ui, e.from);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("depends on");
    ImGui::SameLine();
    if (ImGui::SmallButton(to ? to->name.c_str() : short_id(e.to).c_str())) {
        select_node(ui, e.to);
    }

    ImGui::Separator();
    ImGui::TextDisabled("PROVENANCE");
    chip(std::string(to_string(e.confidence)).c_str(), confidence_color(e.confidence),
         "How much this relationship can be trusted.");
    ImGui::SameLine();
    chip(std::string(to_string(e.freshness)).c_str(), freshness_color(e.freshness));

    if (ImGui::BeginTable("##prov", 2, ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.c_str());
        };
        row("provider", e.provider);
        if (!e.provider_version.empty()) row("version", e.provider_version);
        row("valid from", "generation " + std::to_string(e.valid_from));
        row("valid to", e.valid_to ? "generation " + std::to_string(*e.valid_to) : "still active");
        ImGui::EndTable();
    }

    if (e.evidence) {
        ImGui::Spacing();
        ImGui::TextDisabled("EVIDENCE");
        ImGui::Text("%s:%d", e.evidence->artifact.c_str(), e.evidence->line);
        if (!e.evidence->snippet.empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, to_v4(Vec4{0.05f, 0.055f, 0.07f, 1.0f}));
            ImGui::BeginChild("##snip", ImVec2(0, 46.0f * ui.view.ui_text_scale), true);
            ImGui::TextWrapped("%s", e.evidence->snippet.c_str());
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    } else {
        // The live backend may lazy-load evidence via GET /explain/edge/:id, so
        // "absent" is a legitimate state and gets said out loud rather than left blank.
        ImGui::TextDisabled("evidence not loaded for this edge");
    }
}

void draw_inspector(ecs::World& world) {
    Ui ui(world);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const PanelMetrics   m  = panel_metrics(ui);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - m.right, vp->WorkPos.y + m.top));
    ImGui::SetNextWindowSize(ImVec2(m.right, vp->WorkSize.y - m.top - m.bottom));
    ImGui::Begin("Inspector", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    if (!ui.selection.edge.empty()) {
        if (const Edge* e = ui.store.edge(ui.selection.edge)) {
            if (ImGui::SmallButton("< back to node")) ui.selection.edge.clear();
            ImGui::Separator();
            draw_edge_inspector(ui, *e);
            ImGui::End();
            return;
        }
        ui.selection.edge.clear();
    }

    if (ui.selection.node.empty()) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Click a node to see what depends on it and why. Click a hop in the "
            "explanation to see the evidence behind that relationship.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("CONTROLS");
        ImGui::BulletText("drag           pan");
        ImGui::BulletText("wheel          zoom");
        ImGui::BulletText("drag a node    move it; it settles back");
        ImGui::BulletText("double click   pin / unpin in place");
        ImGui::BulletText("F              fit to view");
        ImGui::BulletText("space          play / pause the scenario");
        ImGui::BulletText(".              step one event");
        ImGui::End();
        return;
    }

    if (const Node* n = ui.store.node(ui.selection.node)) {
        draw_node_inspector(ui, *n);
    } else {
        // The selected node was deleted by an event. Say so rather than blanking:
        // the disappearance is itself information about what the agent did.
        ImGui::TextColored(to_v4(theme().removed), "%s", ui.selection.node.c_str());
        ImGui::TextDisabled("This node no longer exists in the current generation.");
        if (ImGui::SmallButton("clear selection")) ui.selection.node.clear();
    }
    ImGui::End();
}

// ------------------------------------------------------------- bottom panel

void draw_timeline(ecs::World& world) {
    Ui ui(world);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const PanelMetrics   m  = panel_metrics(ui);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - m.bottom));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, m.bottom));
    ImGui::Begin("Scenario", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    Timeline* tl = ui.source.source->timeline();

    ImGui::BeginChild("##transport", ImVec2(ImGui::GetContentRegionAvail().x * 0.56f, 0), false);

    if (!tl) {
        // A live backend has no timeline. The panel degrades to a status readout
        // rather than disappearing, so the layout does not jump when sources swap.
        ImGui::TextDisabled("live source -- no timeline");
        ImGui::Text("%s", ui.source.source->status().description.c_str());
    } else {
        if (!ui.library.names.empty()) {
            ImGui::SetNextItemWidth(220.0f * ui.view.ui_text_scale);
            int fx = ui.library.current;
            std::string items;
            for (const auto& n : ui.library.names) { items += n; items.push_back('\0'); }
            items.push_back('\0');
            if (ImGui::Combo("##fixture", &fx, items.c_str())) ui.cmd.push(ecs::SelectFixture{fx});
            ImGui::SameLine();
        }
        if (ui.source.fixtures) {
            ImGui::SetNextItemWidth(280.0f * ui.view.ui_text_scale);
            int  idx = static_cast<int>(ui.source.fixtures->scenario_index());
            std::string items;
            for (const auto& s : ui.source.fixtures->scenarios()) {
                items += s.name;
                items.push_back('\0');
            }
            items.push_back('\0');
            if (ImGui::Combo("##scenario", &idx, items.c_str())) {
                ui.source.fixtures->select_scenario(static_cast<std::size_t>(idx));
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload")) ui.cmd.push(ecs::ReloadFixture{});
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Re-read the fixture files from disk.\n"
                                  "Edit a scenario in an editor, hit this, watch it run.");
            }
            if (!ui.source.fixtures->scenario().description.empty()) {
                ImGui::TextDisabled("%s", ui.source.fixtures->scenario().description.c_str());
            }
        }

        const SourceStatus st = ui.source.source->status();

        if (ImGui::Button("|<")) tl->restart();
        ImGui::SameLine();
        if (ImGui::Button(tl->playing() ? "Pause" : "Play")) {
            tl->playing() ? tl->pause() : tl->play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Step >|")) tl->step_event();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f * ui.view.ui_text_scale);
        float rate = static_cast<float>(tl->rate());
        if (ImGui::SliderFloat("##rate", &rate, 0.25f, 8.0f, "%.2fx",
                               ImGuiSliderFlags_Logarithmic)) {
            tl->set_rate(rate);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("g%llu   event %d/%d",
                            static_cast<unsigned long long>(ui.store.generation()),
                            st.events_emitted, st.events_total);

        // Scrubber with beat markers. Seeking replays from the baseline, so the state
        // it lands in is exactly the state playing forward would have produced.
        const float dur = static_cast<float>(std::max(1.0, st.duration_ms));
        float       pos = static_cast<float>(st.position_ms);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##scrub", &pos, 0.0f, dur, "%.0f ms")) {
            tl->pause();
            tl->seek_ms(pos);
        }

        const ImVec2 bar_min = ImGui::GetItemRectMin();
        const ImVec2 bar_max = ImGui::GetItemRectMax();
        ImDrawList*  dl      = ImGui::GetWindowDrawList();
        for (const auto& m : tl->markers()) {
            const float x = bar_min.x + (bar_max.x - bar_min.x) * (static_cast<float>(m.t_ms) / dur);
            dl->AddLine(ImVec2(x, bar_max.y - 3), ImVec2(x, bar_max.y + 4),
                        to_u32(theme().direct), 1.5f);
            if (ImGui::IsMouseHoveringRect(ImVec2(x - 4, bar_max.y - 4),
                                           ImVec2(x + 4, bar_max.y + 6))) {
                ImGui::SetTooltip("%.0f ms  %s", m.t_ms, m.label.c_str());
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // -- event log: the running story of the session
    ImGui::BeginChild("##log", ImVec2(0, 0), true);
    if (ImGui::BeginTable("##logtab", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit)) {
        const float ts = ui.view.ui_text_scale;
        ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, 60 * ts);
        ImGui::TableSetupColumn("gen", ImGuiTableColumnFlags_WidthFixed, 42 * ts);
        ImGui::TableSetupColumn("event", ImGuiTableColumnFlags_WidthFixed, 132 * ts);
        ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& l : ui.store.log()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.0f", l.t_ms);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(l.generation));
            ImGui::TableNextColumn();
            Vec4 c{0.55f, 0.58f, 0.66f, 1.0f};
            if (l.type == EventType::GraphUpdated) c = theme().transitive;
            else if (l.type == EventType::ImpactUpdated) c = theme().direct;
            else if (l.type == EventType::FileChanged) c = theme().changed;
            else if (l.type == EventType::ReconcileCheckpoint) c = theme().added;
            ImGui::TextColored(to_v4(c), "%s", std::string(to_string(l.type)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(l.summary.c_str());
        }
        // Follow the tail: the newest event is the one being watched for.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace

void draw_panels(ecs::World& world) {
    draw_top_bar(world);
    draw_session_panel(world);
    draw_inspector(world);
    draw_timeline(world);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Ui                   ui(world);
    const PanelMetrics   m = panel_metrics(ui);

    ui.viewport.free_origin = Vec2{vp->WorkPos.x + m.left, vp->WorkPos.y + m.top};
    ui.viewport.free_size   = Vec2{std::max(1.0f, vp->WorkSize.x - m.left - m.right),
                                   std::max(1.0f, vp->WorkSize.y - m.top - m.bottom)};
}

// ------------------------------------------------------------------ overlay

void draw_graph_overlay(ecs::World& world, ImDrawList* dl) {
    Ui ui(world);
    const auto&  vs = ui.view;
    const auto&  cam  = ui.camera;
    const Theme& t    = theme();
    auto&        reg  = ui.world.registry;

    // Semantic zoom decides everything about text here. Below the point where a label
    // is legible it is not drawn at all -- a smear of unreadable glyphs is worse than
    // an honest dot, and the hover card covers what the label would have said.
    const rgv::view::NodeDetail detail = rgv::view::node_detail(ui.camera.zoom, vs.graph_text_scale);

    // One label model for every view.
    //
    //   inside  -- the node has grown into a box, so the name goes in it, scaled down
    //              to fit whatever the box currently is.
    //   outside -- the node is still collapsed, so the name sits beside it at a
    //              constant screen size and is dropped when there is no room for it.
    //
    // The screen-space part is what makes zooming reveal names. World-scaled text grows
    // in step with the space between nodes, so crowding never eases however far you
    // zoom; screen-space text stays put while the nodes spread apart beneath it.
    if (vs.show_labels) {
        ImFont*     font    = ImGui::GetFont();
        const float outside_px = rgv::view::kBaseFontPx * vs.graph_text_scale;

        for (auto [ent, pos, ext, ref, label] :
             reg.view<const ecs::Position, const ecs::Extent, const ecs::NodeRef,
                      const ecs::Label>().each()) {
            const Vec2 s = cam.world_to_screen(pos.p);
            if (s.x < ui.viewport.free_origin.x - 240 ||
                s.x > ui.viewport.free_origin.x + ui.viewport.free_size.x + 240 ||
                s.y < ui.viewport.free_origin.y - 90 ||
                s.y > ui.viewport.free_origin.y + ui.viewport.free_size.y + 90) {
                continue;
            }

            const bool  changed = reg.all_of<ecs::Changed>(ent);
            const auto* imp     = reg.try_get<ecs::Impacted>(ent);

            // A container is named along its top edge, at screen size, and only when
            // the box is wide enough on screen to carry the name.
            if (const auto* hull = reg.try_get<ecs::Hull>(ent)) {
                Vec4 hc = t.node_text;
                if (changed) hc = t.changed;
                else if (imp) hc = impact_color(imp->distance);
                const Vec2  tl   = cam.world_to_screen(pos.p - hull->half);
                const float wpx  = hull->half.x * 2.0f * cam.zoom;
                // The full dotted name when it fits, else the last component: a box
                // called `elevators.components` inside `elevators` is `components`.
                std::string text = label.text;
                ImVec2      sz   = font->CalcTextSizeA(outside_px, FLT_MAX, 0.0f, text.c_str());
                if (sz.x + 12.0f > wpx) {
                    if (const auto dot = text.rfind('.'); dot != std::string::npos) text = text.substr(dot + 1);
                    sz = font->CalcTextSizeA(outside_px, FLT_MAX, 0.0f, text.c_str());
                }
                if (sz.x + 12.0f <= wpx) {
                    dl->AddText(font, outside_px, ImVec2(tl.x + 8.0f, tl.y + 5.0f), to_u32(hc), text.c_str());
                }
                continue;
            }
            const auto* disc    = reg.try_get<ecs::Disc>(ent);
            const auto* space   = reg.try_get<ecs::Spacing>(ent);
            const auto* prom    = reg.try_get<ecs::Prominence>(ent);
            const float pscale  = prom ? prom->scale : 1.0f;

            // What the user is pointing at is always named, however crowded it is.
            const bool asked_for = reg.all_of<ecs::Selected>(ent) ||
                                   reg.all_of<ecs::Hovered>(ent) ||
                                   reg.all_of<ecs::OnExplainedPath>(ent);

            const rgv::view::DiscShape shape{disc ? disc->radius : 0.0f,
                                             space ? space->room : 1e9f};
            Vec2  half = rgv::view::node_half(
                cam.zoom, detail, ext.half, shape,
                rgv::view::dot_px_for(changed, imp != nullptr, false, pscale));
            float morph  = rgv::view::disc_morph(detail, shape, ext.half);
            bool  inside = rgv::view::label_belongs_inside(morph);
            if (reg.all_of<ecs::WorldBox>(ent)) {
                // Inside its own rectangle once that is wide enough to read, otherwise
                // unnamed: a name beside every module at overview is the clutter the
                // container exists to avoid, and the hover card names it.
                half   = rgv::view::world_box_half(cam.zoom, ext.half);
                inside = true;
                morph  = 1.0f;
                if (half.x * 2.0f * cam.zoom < 36.0f && !asked_for) continue;
            }

            Vec4 col = t.node_text;
            if (changed) col = t.changed;
            else if (imp) col = impact_color(imp->distance);

            // A prominent node's box is bigger because it matters, so its name grows
            // with it. Leaving the font alone would turn the extra size into padding,
            // which reads as a rendering accident rather than as emphasis.
            float px    = inside ? detail.font_px * pscale : outside_px;
            float alpha = 1.0f;

            if (inside) {
                // Shrunk to fit. Half-morphed the box is narrower than the text wants,
                // and drawing at full size spills the name out of the rectangle that is
                // supposed to contain it.
                const ImVec2 want   = font->CalcTextSizeA(px, FLT_MAX, 0.0f, label.text.c_str());
                const float  room_x = half.x * cam.zoom * 1.80f;
                const float  room_y = half.y * cam.zoom * 1.70f;
                if (want.x > room_x && want.x > 0.0f) px *= room_x / want.x;
                px = std::max(std::min(px, room_y), 1.0f);
            } else {
                // Fade out rather than pop when the node itself is barely visible.
                alpha = std::clamp(half.y * cam.zoom / 3.0f, 0.0f, 1.0f);
                if (!asked_for && alpha < 0.05f) continue;
            }
            col.a *= alpha;

            const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, label.text.c_str());

            // Too crowded to name. Eases as the user zooms in and the nodes spread out.
            //
            // Directories in the radial view are exempt: their nearest neighbour is a
            // file they own, so `room` understates the empty space their name goes into,
            // and applying the test there hides the structure.
            const bool dense_exempt = disc && ref.kind != NodeKind::File;
            if (!inside && !asked_for && !dense_exempt && space &&
                space->room * cam.zoom < sz.x * 0.55f) {
                continue;
            }

            const bool  two_lines = inside && !label.sub.empty() &&
                                   half.y * cam.zoom > px * 1.15f;
            const float line_h    = px;

            Vec2 anchor{s.x, 0.0f};
            if (inside) {
                anchor.y = s.y - (two_lines ? line_h * 0.98f : line_h * 0.5f);
            } else if (disc) {
                // Along the direction it orbits away from, so names around a ring fan
                // outward instead of stacking, staggered so neighbours miss each other.
                std::uint32_t hash = 2166136261u;
                for (unsigned char ch : ref.id) { hash ^= ch; hash *= 16777619u; }
                const float stagger = (hash & 1u) ? px * 1.05f : 0.0f;
                // Clear the whole cluster: a directory's files orbit it, so its own name
                // has to sit outside the outermost orbit.
                const float reach = std::max(half.y, disc->halo);
                const float away  = reach * cam.zoom + px * 0.55f + stagger;
                anchor   = Vec2{s.x + disc->outward.x * away, s.y + disc->outward.y * away};
                anchor.y -= px * 0.5f;
            } else {
                // Below the node in the layered views, where rows already separate them.
                anchor.y = s.y + half.y * cam.zoom + px * 0.25f;
            }

            dl->AddText(font, px, ImVec2(anchor.x - sz.x * 0.5f, anchor.y), to_u32(col),
                        label.text.c_str());

            if (two_lines) {
                Vec4 sub = t.node_text;
                sub.a *= 0.5f * alpha;
                const float  sub_size = px * 0.85f;
                const ImVec2 ssz =
                    font->CalcTextSizeA(sub_size, FLT_MAX, 0.0f, label.sub.c_str());
                dl->AddText(font, sub_size,
                            ImVec2(anchor.x - ssz.x * 0.5f, anchor.y + line_h * 0.96f),
                            to_u32(sub), label.sub.c_str());
            }

            // Distance badge. The number is the whole point of the impact view.
            if (imp && imp->distance > 0) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", imp->distance);
                const float  r = std::clamp(9.0f * cam.zoom * vs.graph_text_scale, 7.0f, 14.0f);
                const ImVec2 c(s.x + half.x * cam.zoom - r * 0.5f,
                               s.y - half.y * cam.zoom + r * 0.5f);
                dl->AddCircleFilled(c, r, ImGui::GetColorU32(ImVec4(0.06f, 0.07f, 0.08f,
                                                                   0.96f * alpha)));
                dl->AddCircle(c, r, to_u32(impact_color(imp->distance)));
                const float  bs  = r * 1.35f;
                const ImVec2 bsz = font->CalcTextSizeA(bs, FLT_MAX, 0.0f, buf);
                Vec4 bc = impact_color(imp->distance);
                bc.a *= alpha;
                dl->AddText(font, bs, ImVec2(c.x - bsz.x * 0.5f, c.y - bsz.y * 0.5f),
                            to_u32(bc), buf);
            }
        }
    }

    // Legend. FR-32 asks for four distinct states; a legend is how they stop being a
    // colour code the user has to memorise.
    struct Row { const char* label; Vec4 color; bool dashed; };
    std::vector<Row> rows = {
        {"changed by the agent", t.changed, false},
        {"direct dependent", t.direct, false},
        {"transitive dependent", t.transitive, false},
        {"unaffected context", t.node_stroke, false},
        {"stale evidence", t.stale, true},
        {"heuristic edge", t.heuristic, true},
    };
    if (vs.mode == ecs::ViewMode::Architecture) {
        rows.push_back({"writes: constructs or calls", t.writes, false});
        rows.push_back({"reads: names or queries", t.reads, false});
    }
    // Only while the relevance filter is actually muting something: a legend entry
    // for a state nothing is in is just clutter.
    if (ui.stats.muted > 0) {
        rows.push_back({"muted: only via a hub", t.pending, true});
    }
    // The legend's glyphs come from the global font, so its box follows the UI scale.
    const float  us   = vs.ui_text_scale;
    const float  line = ImGui::GetTextLineHeight() + 5.0f;
    const float  lw   = 178.0f * us;
    const float  lh   = line * static_cast<float>(rows.size()) + 4.0f;
    const float  lx   = ui.viewport.free_origin.x + 14.0f;
    float        ly   = ui.viewport.free_origin.y + ui.viewport.free_size.y - lh - 10.0f;
    dl->AddRectFilled(ImVec2(lx - 8, ly - 8), ImVec2(lx + lw, ly + lh),
                      ImGui::GetColorU32(ImVec4(0.06f, 0.065f, 0.08f, 0.86f)), 4.0f);
    dl->AddRect(ImVec2(lx - 8, ly - 8), ImVec2(lx + lw, ly + lh),
                ImGui::GetColorU32(ImVec4(0.16f, 0.17f, 0.21f, 1.0f)), 4.0f);

    for (const auto& r : rows) {
        const float mid = ly + line * 0.35f;
        if (r.dashed) {
            for (int i = 0; i < 3; ++i) {
                dl->AddLine(ImVec2(lx + i * 7.0f * us, mid),
                            ImVec2(lx + (i * 7.0f + 4.0f) * us, mid), to_u32(r.color), 2.0f);
            }
        } else {
            dl->AddRectFilled(ImVec2(lx, mid - 4.0f), ImVec2(lx + 16.0f * us, mid + 4.0f),
                              to_u32(r.color), 2.0f);
        }
        dl->AddText(ImVec2(lx + 24.0f * us, ly), to_u32(t.node_text), r.label);
        ly += line;
    }
}


void draw_hover_card(ecs::World& world) {
    Ui ui(world);
    const auto& vs = ui.view;
    if (ui.selection.hovered.empty()) return;

    const Node* n = ui.store.node(ui.selection.hovered);
    if (!n) return;

    // A short delay then a quick fade. Without the delay, sweeping the pointer across
    // a dense graph strobes a card per node and the whole view flickers.
    constexpr float kDelay = 0.16f, kFade = 0.11f;
    const float alpha = std::clamp((ui.selection.hover_time - kDelay) / kFade, 0.0f, 1.0f);
    if (alpha <= 0.01f) return;

    // Anchored to the node, not to the cursor. A cursor-anchored card jitters as the
    // pointer moves inside a node and sits under the thing the user is aiming at; one
    // pinned to the node stays still and visibly belongs to it.
    const entt::entity ent = ui.index.node(ui.selection.hovered);
    if (ent == entt::null) return;
    const auto* pos = ui.world.registry.try_get<ecs::Position>(ent);
    const auto* ext = ui.world.registry.try_get<ecs::Extent>(ent);
    if (!pos || !ext) return;

    const rgv::view::NodeDetail detail = rgv::view::node_detail(ui.camera.zoom, vs.graph_text_scale);
    const Vec2            half   = rgv::view::render_half(ui.camera.zoom, detail, ext->half,
        rgv::view::dot_px_for(ui.world.registry.all_of<ecs::Changed>(ent),
                        ui.world.registry.all_of<ecs::Impacted>(ent), true,
                        [&] { const auto* p = ui.world.registry.try_get<ecs::Prominence>(ent);
                              return p ? p->scale : 1.0f; }()));
    const Vec2 anchor = ui.camera.world_to_screen(pos->p);

    // The card is chrome, not graph: it is an ImGui surface and follows the UI scale,
    // so it stays readable even when graph text has been turned right down.
    const float w = 300.0f * vs.ui_text_scale;
    const float h = 210.0f * vs.ui_text_scale;   // estimate, only used for flipping

    // Flip to the other side rather than let the card run off the graph area.
    float x = anchor.x + half.x * ui.camera.zoom + 14.0f;
    float y = anchor.y + half.y * ui.camera.zoom + 10.0f;
    if (x + w > ui.viewport.free_origin.x + ui.viewport.free_size.x) {
        x = anchor.x - half.x * ui.camera.zoom - w - 14.0f;
    }
    if (y + h > ui.viewport.free_origin.y + ui.viewport.free_size.y) {
        y = std::max(ui.viewport.free_origin.y, anchor.y - h);
    }
    x = std::max(x, ui.viewport.free_origin.x);

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.95f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::Begin("##hovercard", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

    ImGui::TextUnformatted(n->name.c_str());
    ImGui::SameLine();
    chip(std::string(to_string(n->kind)).c_str(), Vec4{0.50f, 0.55f, 0.68f, 1.0f});
    if (!n->path.empty()) ImGui::TextDisabled("%s", n->path.c_str());

    chip(std::string(to_string(n->freshness)).c_str(), freshness_color(n->freshness));
    if (!n->language.empty()) {
        ImGui::SameLine();
        chip(n->language.c_str(), Vec4{0.45f, 0.50f, 0.60f, 1.0f});
    }

    // What the agent did to it, if anything.
    for (const auto& c : ui.store.changed_files()) {
        if (c.node_id != n->id) continue;
        ImGui::Separator();
        chip(std::string(to_string(c.change)).c_str(), change_color(c.change));
        ImGui::SameLine();
        chip(std::string(to_string(c.processing)).c_str(), processing_color(c.processing));
        ImGui::SameLine();
        ImGui::TextDisabled("g%llu", static_cast<unsigned long long>(c.generation));
        break;
    }

    if (const ImpactedNode* in = impacted_for(ui.store, vs.level, n->id)) {
        ImGui::Separator();
        chip(in->changed ? "changed" : (in->direct ? "direct" : "transitive"),
             impact_color(in->min_distance));
        ImGui::SameLine();
        ImGui::TextDisabled("distance %d", in->min_distance);
        if (in->freshness != Freshness::Current) {
            ImGui::SameLine();
            chip(std::string(to_string(in->freshness)).c_str(), freshness_color(in->freshness));
        }
        if (!in->paths.empty()) {
            // One line of the reason, so the card answers "why" without a click.
            const auto& path = in->paths[std::clamp(ui.selection.path_index, 0,
                                                    static_cast<int>(in->paths.size()) - 1)].edges;
            std::string chain = n->name;
            NodeId      cur   = n->id;
            for (const auto& eid : path) {
                const Edge* e = ui.store.edge(eid);
                if (!e) break;
                const Node* dst = ui.store.node(e->to);
                chain += "  ->  ";
                chain += dst ? dst->name : short_id(e->to);
                cur = e->to;
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", chain.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("%zu dependents   %zu dependencies",
                        ui.store.in_edges(n->id).size(), ui.store.out_edges(n->id).size());
    {
        const auto& idx = ui.derived.specificity;
        if (!idx.empty() && idx.population() > 1 &&
            idx.specificity(n->id) <= analysis::kHubThreshold) {
            chip("hub", theme().changed);
            ImGui::SameLine();
            ImGui::TextDisabled("%d of %d depend on it", idx.dependents(n->id),
                                idx.population());
        }
    }
    ImGui::TextDisabled("click to explain");

    ImGui::End();
    ImGui::PopStyleVar();
}


void draw_text_settings(ecs::World& world) {
    Ui ui(world);
    auto& vs = ui.view;
    if (!vs.show_text_settings) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // Width pinned in screen pixels, height auto. Position frozen on appearance and
    // the window fixed in place, so it cannot chase the toolbar button as the toolbar
    // reflows underneath it.
    // Placed just clear of the inspector, once, when it opens.
    const PanelMetrics pm = panel_metrics(ui);
    ImGui::SetNextWindowSizeConstraints(ImVec2(372.0f, 0.0f), ImVec2(372.0f, FLT_MAX));
    ImGui::SetNextWindowPos(
        ImVec2(std::max(vp->WorkPos.x + pm.left + 8.0f,
                        vp->WorkPos.x + vp->WorkSize.x - pm.right - 388.0f),
               vp->WorkPos.y + std::max(ui.viewport.top_bar_height, 64.0f) + 16.0f),
        ImGuiCond_Appearing);

    // No title bar on purpose. Begin() computes the title bar's height from the font
    // size *before* the scale correction below can take effect, so a titled window
    // would still shift its contents by a frame's worth of scale change on every drag
    // step -- the exact thing this window exists to avoid.
    if (!ImGui::Begin("##textsettings", nullptr,
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // Cancel the global font scale so this window renders at the base size whatever
    // that scale currently is. Applied immediately, so every item below is laid out at
    // a constant size: the panels behind update live while the control under the
    // cursor stays exactly where it is.
    ImGui::SetWindowFontScale(1.0f / std::max(vs.ui_text_scale, 0.01f));

    ImGui::TextUnformatted("Text size");
    ImGui::SameLine(298.0f);
    if (ImGui::SmallButton("close")) vs.show_text_settings = false;
    ImGui::Separator();
    ImGui::Spacing();

    // Returns true when the value moved; sets `commit` when it should reach disk.
    auto row = [&](const char* name, const char* id, const char* help, float* value,
                   bool* commit) {
        bool moved = false;
        *commit    = false;

        ImGui::TextUnformatted(name);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", help);
        ImGui::PopTextWrapPos();

        constexpr float kStep = 0.05f;
        auto nudge = [&](float delta) {
            *value = std::clamp(*value + delta, config::Settings::kMinTextScale,
                                config::Settings::kMaxTextScale);
            moved = *commit = true;
        };

        ImGui::PushID(id);
        // Steppers, for when a drag is not precise enough.
        if (ImGui::Button("-", ImVec2(28.0f, 0.0f))) nudge(-kStep);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(232.0f);
        if (ImGui::SliderFloat("##v", value, config::Settings::kMinTextScale,
                               config::Settings::kMaxTextScale, "%.2fx")) {
            moved = true;
        }
        // Written when the drag ends, not on every pixel of it.
        if (ImGui::IsItemDeactivatedAfterEdit()) *commit = true;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Drag, or ctrl+click to type an exact value.");
        }
        ImGui::SameLine();
        if (ImGui::Button("+", ImVec2(28.0f, 0.0f))) nudge(kStep);
        ImGui::PopID();
        return moved;
    };

    bool commit = false;

    // Panel geometry is derived from this every frame, so there is nothing to
    // invalidate -- the next frame simply lays out at the new size.
    row("UI text", "ui", "Panels, inspector, event log. Larger leaves less room for the graph.",
        &vs.ui_text_scale, &commit);
    if (commit) ui.cmd.push(ecs::SaveSettings{});

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (row("Graph text", "graph",
            "Node labels, and so node sizes. Larger means fewer nodes fit on screen.",
            &vs.graph_text_scale, &commit)) {
        ui.world.resource<ecs::SceneRequests>().refresh_extents = true;   // node boxes are sized to hold their label
    }
    if (commit) ui.cmd.push(ecs::SaveSettings{});

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Reset both")) {
        vs.ui_text_scale        = 1.0f;
        vs.graph_text_scale     = 1.0f;
        ui.world.resource<ecs::SceneRequests>().refresh_extents = true;
        ui.cmd.push(ecs::SaveSettings{});
    }
    ImGui::SameLine();
    ImGui::TextDisabled("saved to ~/.config/rgv");

    ImGui::End();
}

} // namespace rgv::ui
