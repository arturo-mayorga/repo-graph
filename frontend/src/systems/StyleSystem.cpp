#include "rgv/systems/StyleSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/ui/Theme.h"
#include "rgv/view/Evidence.h"

#include <algorithm>

namespace rgv::systems {

void StyleSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&        registry = world.registry;
    const ui::Theme& t    = ui::theme();

    // -- nodes
    for (auto [ent, ref, fresh, style] :
         registry.view<const ecs::NodeRef, const ecs::FreshnessState, ecs::Style>().each()) {
        const auto* impacted = registry.try_get<ecs::Impacted>(ent);
        const bool  changed  = registry.all_of<ecs::Changed>(ent);
        // A Disc means the view draws circles rather than boxes -- the filesystem
        // view. Nothing else needs to know which mode is active.
        const bool  disc     = registry.all_of<ecs::Disc>(ent);

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

        // A container is a region, not a node: a faint fill so the modules inside and
        // the edges crossing it stay readable, and an outline that still carries impact.
        if (registry.all_of<ecs::Hull>(ent)) {
            style.fill   = mix(t.node_fill, t.background, 0.55f);
            style.fill.a = 0.85f;
            if (distance < 0) {
                style.stroke   = mix(t.node_stroke, t.background, 0.15f);
                style.stroke_w = 1.2f;
                style.emphasis = 0.4f;
            }
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
        if (registry.all_of<ecs::OnExplainedPath>(ent)) {
            style.stroke   = t.path;
            style.stroke_w = std::max(style.stroke_w, 3.2f);
            style.emphasis = 1.0f;
        }
        if (registry.all_of<ecs::Selected>(ent)) {
            style.stroke   = t.selection;
            style.stroke_w = 3.0f;
        }
    }
}

} // namespace rgv::systems
