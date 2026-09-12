#include "rgv/systems/LabelSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/model/GraphStore.h"
#include "rgv/view/LabelLayout.h"
#include "rgv/view/SemanticZoom.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rgv::systems {
namespace {

// Long enough to read as a fade rather than a flicker, short enough that panning does
// not feel like it is dragging the names behind it.
constexpr float kFadeSeconds = 0.25f;

// What the user is pointing at outranks everything, and what they have selected
// outranks the rest. The gaps are wide enough that no amount of degree closes them.
constexpr float kHovered  = 1.0e9f;
constexpr float kSelected = 1.0e6f;
constexpr float kOnPath   = 1.0e3f;

} // namespace

void LabelSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto&       reg      = world.registry;
    const auto& cam      = world.resource<Camera>();
    const auto& view     = world.resource<ecs::ViewSettings>();
    const auto& viewport = world.resource<ecs::Viewport>();

    if (!view.show_labels) {
        reg.clear<ecs::SideLabel>();
        return;
    }

    const view::NodeDetail detail = view::node_detail(cam.zoom, view.graph_text_scale);
    const float            px     = view::kBaseFontPx * view.graph_text_scale;

    // How much is attached to a node: its dependency degree in the repository, plus
    // what it holds on screen.
    //
    // Both halves are needed because the two views draw different edges. Dependency
    // degree alone gives a directory nothing -- directories do not import -- and the
    // filesystem view is unreadable without directory names. Drawn degree alone gives
    // every file in a directory the same 1, because the only edge drawn there is the
    // one to its parent, and then the module everything imports is named no more often
    // than the module nothing imports.
    const auto& store = world.resource<GraphStore>();
    const auto& index = world.resource<ecs::EntityIndex>();
    auto        degree_of = [&](const NodeId& id) {
        int n = static_cast<int>(store.in_edges(id).size() + store.out_edges(id).size());
        for (const auto& child : store.children(id)) {
            if (index.node(child) != entt::null) ++n;
        }
        return n;
    };

    std::vector<entt::entity>   entities;
    std::vector<Vec2>           anchors;
    std::vector<view::LabelBox> boxes;
    std::vector<entt::entity>   dropped;

    for (auto [ent, ref, pos, ext, label] :
         reg.view<const ecs::NodeRef, const ecs::Position, const ecs::Extent,
                  const ecs::Label>().each()) {
        const Vec2 s = cam.world_to_screen(pos.p);
        if (s.x < viewport.free_origin.x - 240 ||
            s.x > viewport.free_origin.x + viewport.free_size.x + 240 ||
            s.y < viewport.free_origin.y - 90 ||
            s.y > viewport.free_origin.y + viewport.free_size.y + 90) {
            dropped.push_back(ent);
            continue;
        }

        const auto* disc  = reg.try_get<ecs::Disc>(ent);
        const auto* space = reg.try_get<ecs::Spacing>(ent);
        const auto* prom  = reg.try_get<ecs::Prominence>(ent);

        const view::DiscShape shape{disc ? disc->radius : 0.0f, space ? space->room : 1e9f};
        const Vec2            half = view::node_half(
            cam.zoom, detail, ext.half, shape,
            view::dot_px_for(reg.all_of<ecs::Changed>(ent), reg.all_of<ecs::Impacted>(ent),
                             false, prom ? prom->scale : 1.0f));

        // A node that has grown into a box holds its own name; this is only about the
        // ones whose name has to go somewhere beside them.
        if (view::label_belongs_inside(view::disc_morph(detail, shape, ext.half))) {
            dropped.push_back(ent);
            continue;
        }

        Vec2 anchor{s.x, s.y + half.y * cam.zoom + px * 0.25f};
        if (disc != nullptr) {
            // Along the direction it orbits away from, so names around a ring fan
            // outward instead of stacking, staggered so neighbours miss each other.
            std::uint32_t hash = 2166136261u;
            for (unsigned char ch : ref.id) {
                hash ^= ch;
                hash *= 16777619u;
            }
            const float stagger = (hash & 1u) ? px * 1.05f : 0.0f;
            // Clear the whole cluster: a directory's files orbit it, so its own name has
            // to sit outside the outermost orbit.
            const float reach = std::max(half.y, disc->halo);
            const float away  = reach * cam.zoom + px * 0.55f + stagger;
            anchor            = Vec2{s.x + disc->outward.x * away, s.y + disc->outward.y * away};
            anchor.y -= px * 0.5f;
        }

        const float w = static_cast<float>(label.text.size()) * px * view::kCharAdvanceRatio;
        float       priority = static_cast<float>(degree_of(ref.id));
        if (reg.all_of<ecs::OnExplainedPath>(ent)) priority += kOnPath;
        if (reg.all_of<ecs::Selected>(ent)) priority += kSelected;
        if (reg.all_of<ecs::Hovered>(ent)) priority += kHovered;

        entities.push_back(ent);
        anchors.push_back(anchor);
        boxes.push_back(view::LabelBox{Vec2{anchor.x - w * 0.5f, anchor.y},
                                       Vec2{anchor.x + w * 0.5f, anchor.y + px}, priority});
    }

    std::vector<bool> wanted(entities.size(), false);
    for (int i : view::choose_labels(boxes)) wanted[static_cast<std::size_t>(i)] = true;

    const float step = kFadeSeconds > 0.0f ? frame.dt / kFadeSeconds : 1.0f;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        auto& side  = reg.get_or_emplace<ecs::SideLabel>(entities[i]);
        side.anchor = anchors[i];
        side.px     = px;
        side.alpha  = std::clamp(side.alpha + (wanted[i] ? step : -step), 0.0f, 1.0f);
    }
    // Off screen or grown into a box: there is nothing to fade, and leaving a stale
    // anchor behind would draw the name where the node used to be.
    for (auto e : dropped) reg.remove<ecs::SideLabel>(e);
}

} // namespace rgv::systems
