// Turns the backend's impact result into per-entity state.
//
// Owns Changed, Impacted, and HubSeed, and the domain half of SceneStats. Nothing else
// writes them. Runs every frame rather than on a cache key: it is a linear pass with
// no allocation after warm-up, and a gate here would be one more thing to get subtly
// wrong when a filter moves.
#pragma once

#include "rgv/analysis/Specificity.h"
#include "rgv/contract/Impact.h"
#include "rgv/ecs/System.h"
#include "rgv/model/GraphStore.h"

#include <unordered_map>

namespace rgv::systems {

class ImpactStateSystem final : public ecs::System {
public:
    std::string_view name() const override { return "ImpactStateSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    // Held across frames so the per-frame pass allocates nothing.
    std::unordered_map<NodeId, const ImpactedNode*>       impact_by_id_;
    std::unordered_map<NodeId, const ChangedFile*>        changed_by_id_;
    std::unordered_map<NodeId, const analysis::HubAlert*> hub_by_id_;
};

} // namespace rgv::systems
