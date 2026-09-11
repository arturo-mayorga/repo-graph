#include "rgv/systems/ImpactStateSystem.h"

#include "rgv/analysis/Specificity.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/view/Evidence.h"

namespace rgv::systems {

void ImpactStateSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&       registry = world.registry;
    const auto& store    = world.resource<GraphStore>();
    const auto& view     = world.resource<ecs::ViewSettings>();
    const auto& filters  = world.resource<ecs::Filters>();
    const auto& derived  = world.resource<ecs::DerivedState>();
    auto&       stats    = world.resource<ecs::SceneStats>();

    const ImpactResult* result = store.impact(view.level);

    impact_by_id_.clear();
    if (result) {
        for (const auto& in : result->impacted_nodes) impact_by_id_[in.node_id] = &in;
    }
    // The nested architecture view draws two levels at once, so it reads two results:
    // packages from the package level, the modules inside them from the file level.
    // The user's chosen level still wins for its own kind; the other level fills in.
    if (view.mode == ecs::ViewMode::Architecture) {
        for (Level other : {Level::Package, Level::File}) {
            if (other == view.level) continue;
            const ImpactResult* r = store.impact(other);
            if (!r) continue;
            for (const auto& in : r->impacted_nodes) {
                const Node* n = store.node(in.node_id);
                if (!n || !level_admits(other, n->kind)) continue;
                impact_by_id_.try_emplace(in.node_id, &in);
            }
        }
    }
    changed_by_id_.clear();
    for (const auto& c : store.changed_files()) changed_by_id_[c.node_id] = &c;
    hub_by_id_.clear();
    for (const auto& h : derived.hub_alerts) hub_by_id_[h.node] = &h;

    registry.clear<ecs::Changed>();
    registry.clear<ecs::Impacted>();
    registry.clear<ecs::HubSeed>();

    stats.nodes = stats.edges = 0;
    stats.changed = stats.impacted = stats.muted = stats.stale = 0;

    for (auto [ent, ref, fresh] :
         registry.view<const ecs::NodeRef, const ecs::FreshnessState>().each()) {
        ++stats.nodes;

        // A file's own change is what makes it red; a package is red when a file it
        // owns changed. The projection happens here so nothing downstream repeats it.
        const ChangedFile* changed = nullptr;
        if (auto it = changed_by_id_.find(ref.id); it != changed_by_id_.end()) {
            changed = it->second;
        } else if (ref.kind == NodeKind::Package || ref.kind == NodeKind::Directory) {
            for (const auto& [nid, c] : changed_by_id_) {
                if (store.ancestor_of_kind(nid, ref.kind) == ref.id) { changed = c; break; }
            }
        } else if (ref.kind == NodeKind::Symbol) {
            // The projection runs downward too: a symbol is changed when the file that
            // defines it is, since the file is the unit anything was saved at.
            if (const Node* n = store.node(ref.id)) {
                if (auto it = changed_by_id_.find(n->parent); it != changed_by_id_.end()) {
                    changed = it->second;
                }
            }
        }
        if (changed) {
            registry.emplace<ecs::Changed>(ent, ecs::Changed{changed->change, changed->processing});
            ++stats.changed;
            if (auto h = hub_by_id_.find(ref.id); h != hub_by_id_.end()) {
                registry.emplace<ecs::HubSeed>(ent, ecs::HubSeed{h->second->dependents,
                                                                 h->second->population,
                                                                 h->second->specificity,
                                                                 h->second->reach_fraction});
            }
        }

        Freshness effective = fresh.value;
        if (auto it = impact_by_id_.find(ref.id); it != impact_by_id_.end()) {
            const ImpactedNode* in = it->second;
            if (in->min_distance <= filters.max_impact_depth) {
                const float rel = analysis::relevance(store, derived.specificity, *in);

                // A changed node is never muted, whatever it scores. Its own hub-ness
                // is precisely why it matters, and filtering it would hide the loudest
                // event the product can report.
                const bool muted = !changed && in->min_distance > 0 &&
                                   rel < filters.min_relevance;

                registry.emplace<ecs::Impacted>(ent, ecs::Impacted{in->min_distance, in->direct,
                                                                   in->cause, rel, in->freshness,
                                                                   muted});
                if (in->min_distance > 0) {
                    if (muted) ++stats.muted;
                    else ++stats.impacted;
                }
                effective = view::worse_of(effective, in->freshness);
            }
        }
        if (effective != Freshness::Current) ++stats.stale;
    }

    for (auto [ent, ref, fresh] :
         registry.view<const ecs::EdgeRef, const ecs::FreshnessState>().each()) {
        ++stats.edges;
        if (fresh.value != Freshness::Current) ++stats.stale;
    }
}

} // namespace rgv::systems
