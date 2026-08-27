#include "rgv/systems/SpecificitySystem.h"

#include "rgv/analysis/Specificity.h"
#include "rgv/ecs/Resources.h"
#include "rgv/model/GraphStore.h"

namespace rgv::systems {

void SpecificitySystem::run(ecs::World& world, const ecs::FrameContext&) {
    const auto& store   = world.resource<GraphStore>();
    const auto& view    = world.resource<ecs::ViewSettings>();
    const auto& filters = world.resource<ecs::Filters>();

    const bool changed = !primed_ || store.generation() != last_generation_ ||
                         view.level != last_level_ ||
                         filters.show_heuristic != last_heuristic_ ||
                         store.dirty().topology || store.dirty().impact;
    if (!changed) return;

    last_generation_ = store.generation();
    last_level_      = view.level;
    last_heuristic_  = filters.show_heuristic;
    primed_          = true;

    const ImpactResult* result = store.impact(view.level);

    // Score over the same edges the traversal would follow, or the index would
    // disagree with the paths it is used to weigh.
    ImpactFilters traversal;
    if (result) traversal = result->filters;
    traversal.include_heuristic = filters.show_heuristic;

    auto& derived       = world.resource<ecs::DerivedState>();
    derived.specificity = analysis::build(store, view.level, traversal);
    derived.hub_alerts.clear();
    if (result) {
        derived.hub_alerts =
            analysis::hub_seeds(store, derived.specificity, *result, analysis::kHubThreshold);
    }
}

} // namespace rgv::systems
