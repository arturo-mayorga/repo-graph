#include "rgv/systems/StyleSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/ui/Theme.h"
#include "rgv/view/Evidence.h"
#include "rgv/view/Focus.h"

#include <algorithm>
#include <cmath>

namespace rgv::systems {

namespace {

Vec4 dim(const Vec4& c, float b) { return {c.r * b, c.g * b, c.b * b, c.a}; }

} // namespace

void StyleSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&        registry = world.registry;
    const ui::Theme& t    = ui::theme();

    // -- nodes
    for (auto [ent, ref, fresh, style] :
         registry.view<const ecs::NodeRef, const ecs::FreshnessState, ecs::Style>().each()) {
        const auto* impacted = registry.try_get<ecs::Impacted>(ent);
        const bool  changed  = registry.all_of<ecs::Changed>(ent);
        // The view decided this, once, in ShapeSystem. Nothing here knows the mode.
        const auto* ns   = registry.try_get<ecs::NodeShape>(ent);
        const bool  disc = ns && ns->form == ecs::NodeShape::Form::Disc;

        // A muted result falls back to context: still on screen, still true, no longer
        // competing for attention.
        int distance = -1;
        if (changed) distance = 0;
        else if (impacted && !impacted->muted) distance = impacted->distance;

        Freshness effective = fresh.value;
        if (impacted) effective = view::worse_of(effective, impacted->freshness);

        style.emphasis = distance < 0 ? 0.0f : 1.0f - std::min(0.6f, distance * 0.14f);
        style.fill     = t.node_fill;
        style.stroke   = distance >= 0 ? ui::impact_color(distance) : t.node_stroke;
        style.stroke_w = distance == 0 ? 3.0f : (distance == 1 ? 2.4f : 1.6f);
        style.dash     = 0.0f;

        if (disc && distance < 0) {
            // Untouched files carry their language's colour, which is what makes a
            // repository recognisable at a glance. Impact still wins where it applies:
            // the blast radius has to be readable on top of the palette, not lost in it.
            if (ref.kind == NodeKind::File) {
                const auto* label = registry.try_get<ecs::Label>(ent);
                const Vec4  hue   = ui::extension_color(label ? label->text : std::string{});
                style.fill        = mix(hue, t.node_fill, 0.42f);
                style.stroke      = hue;
                style.stroke_w    = 1.0f;
            } else {
                // Directories recede: they are structure, not content.
                style.fill     = mix(t.node_fill, t.background, 0.35f);
                style.stroke   = mix(t.node_stroke, t.background, 0.25f);
                style.stroke_w = 1.4f;
            }
            style.emphasis = 0.55f;
        }

        // A finding, before evidence quality rather than after it: a stale node in a
        // cycle must still read as stale (NFR-04), so the colour may be overwritten
        // below and the weight is the channel that survives.
        if (registry.all_of<ecs::InCycle>(ent)) {
            style.stroke   = t.cycle;
            style.stroke_w = std::max(style.stroke_w, 2.8f);
            style.emphasis = std::max(style.emphasis, 0.85f);
        }

        switch (effective) {
            case Freshness::Stale:
                // Colour plus a dashed outline: two channels, because one colour cue is
                // not enough to stop someone trusting a stale relationship.
                style.stroke = t.stale;
                style.dash   = 9.0f;
                break;
            case Freshness::Pending:
                style.stroke = t.pending;
                style.dash   = 5.0f;
                break;
            case Freshness::Invalid:
                style.stroke   = t.invalid;
                style.stroke_w = 3.0f;
                break;
            case Freshness::Current:
                break;
        }

        // Interaction wins over evidence, and does so HERE rather than at draw time.
        if (registry.all_of<ecs::OnExplainedPath>(ent)) {
            style.stroke   = t.path;
            style.stroke_w = std::max(style.stroke_w, 3.0f);
        }
        if (registry.all_of<ecs::Hovered>(ent)) {
            style.stroke   = t.hover;
            style.stroke_w = std::max(style.stroke_w, 2.6f);
        }
        if (registry.all_of<ecs::Selected>(ent)) {
            style.stroke   = t.selection;
            style.stroke_w = 3.4f;
        }
        if (registry.all_of<ecs::Pinned>(ent)) style.fill = mix(style.fill, t.direct, 0.10f);

        // Distance from the focus, applied last, to whatever colour the rules above
        // settled on -- so it darkens the answer rather than replacing it, and a stale
        // node five hops out still reads as stale (NFR-04), just further away.
        //
        // What the agent touched is exempt. A change is the thing the view exists to
        // report, and dimming it because the user happens to be looking elsewhere is
        // precisely the failure the relevance filter is forbidden from committing.
        if (!changed) {
            const float b = view::focus_brightness(registry.try_get<ecs::FocusDistance>(ent));
            style.fill    = dim(style.fill, b);
            style.stroke  = dim(style.stroke, b);
        }
    }

    // -- edges
    for (auto [ent, ref, ends, style] :
         registry.view<const ecs::EdgeRef, const ecs::Endpoints, ecs::Style>().each()) {
        const auto* a = registry.try_get<ecs::Impacted>(ends.from);
        const auto* b = registry.try_get<ecs::Impacted>(ends.to);
        const bool  on_impact = a && b && !a->muted && !b->muted;

        style.stroke   = on_impact ? t.edge_impact : t.edge;
        style.stroke_w = on_impact ? 2.0f : 1.2f;
        style.dash     = 0.0f;
        style.emphasis = on_impact ? 0.9f : 0.35f;

        if (ref.kind == EdgeKind::Contains) {
            style.stroke   = t.edge;
            style.stroke_w = 1.0f;
            style.emphasis = 0.3f;
        }
        // Reads and writes are told apart by colour, on and off the impact set alike:
        // the question this edge answers is which way the data flows.
        if (ref.kind == EdgeKind::Calls) {
            style.stroke   = t.writes;
            style.emphasis = on_impact ? 0.95f : 0.55f;
        } else if (ref.kind == EdgeKind::References) {
            style.stroke   = t.reads;
            style.emphasis = on_impact ? 0.95f : 0.45f;
        }
        // Same rule as the nodes: the finding is stated first so evidence quality can
        // still overrule the colour, and the extra weight carries it either way.
        if (registry.all_of<ecs::InCycle>(ent)) {
            style.stroke   = t.cycle;
            style.stroke_w = std::max(style.stroke_w, 2.6f);
            style.emphasis = std::max(style.emphasis, 0.9f);
        }
        if (const auto* c = registry.try_get<ecs::ConfidenceState>(ent)) {
            if (c->value == Confidence::Heuristic || c->value == Confidence::Unresolved) {
                style.stroke = t.heuristic;
                style.dash   = 7.0f;
            }
        }
        if (const auto* f = registry.try_get<ecs::FreshnessState>(ent)) {
            if (f->value == Freshness::Stale) {
                style.stroke = t.stale;
                style.dash   = 9.0f;
            } else if (f->value == Freshness::Invalid) {
                style.stroke = t.invalid;
                style.dash   = 4.0f;
            }
        }
        // What the user is pointing at or has selected has every edge lit, in the
        // edge's own colour: point at a system and its reads and writes stand out of
        // the field, which is how "what does this depend on" is answered without a
        // click.
        auto attended = [&](entt::entity n) {
            return registry.valid(n) && (registry.all_of<ecs::Hovered>(n) || registry.all_of<ecs::Selected>(n));
        };
        if (attended(ends.from) || attended(ends.to)) {
            style.stroke_w = std::max(style.stroke_w, 2.4f);
            style.emphasis = 1.0f;
        }
        // A line standing for twelve relationships is drawn heavier than one standing
        // for one. Logarithmic, so a hub thickens without swamping the picture.
        if (const auto* w = registry.try_get<ecs::EdgeWeight>(ent); w && w->count > 1) {
            style.stroke_w *= 1.0f + 0.40f * std::log2(static_cast<float>(w->count));
        }
        if (registry.all_of<ecs::OnExplainedPath>(ent)) {
            style.stroke   = t.path;
            style.stroke_w = std::max(style.stroke_w, 3.2f);
            style.emphasis = 1.0f;
        }
        if (registry.all_of<ecs::Selected>(ent)) {
            style.stroke   = t.selection;
            style.stroke_w = 3.0f;
        }

        // A line dims with its farther end, so the neighbourhood's own relationships
        // stay legible and the rest recedes without leaving. A line on the explained
        // path is exempt for the same reason a changed node is: it is the answer to a
        // question the user asked, not context around one.
        if (!registry.all_of<ecs::OnExplainedPath>(ent)) {
            style.stroke = dim(style.stroke, view::focus_brightness(
                                                 registry.try_get<ecs::FocusDistance>(ent)));
        }
    }
}

} // namespace rgv::systems
