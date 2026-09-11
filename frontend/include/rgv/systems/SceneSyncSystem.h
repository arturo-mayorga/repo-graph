// Reconciles the graph store into entities.
//
// The only system that creates or destroys entities. It consumes the store's dirty
// sets, so an ordinary file save touches a handful of entities rather than rebuilding
// the world -- which is what "a file save must not trigger a full global layout"
// (spec 11.2) actually means in practice.
#pragma once

#include "rgv/ecs/Resources.h"
#include "rgv/ecs/System.h"

#include <unordered_map>
#include <utility>

namespace rgv::systems {

class SceneSyncSystem final : public ecs::System {
public:
    std::string_view name() const override { return "SceneSyncSystem"; }
    void             setup(ecs::World& world) override;
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    void rebuild(ecs::World& world);
    void incremental(ecs::World& world);
    void revisit(ecs::World& world);
    void refresh_extents(ecs::World& world);

    bool node_visible(const ecs::World& world, const Node& n) const;
    bool hidden(const ecs::World& world, const Node& n) const;
    NodeId representative(const ecs::World& world, NodeId id) const;
    void   choose_drawn_edges(ecs::World& world);
    void   sync_containment(ecs::World& world);
    // The change itself, or a seed everything else is explained against. Exempt from
    // every filter -- hiding either would remove the thing the view is about.
    bool exempt_from_filters(const ecs::World& world, const Node& n) const;
    void count_hidden(ecs::World& world) const;
    bool edge_visible(const ecs::World& world, const Edge& e) const;
    void upsert_node(ecs::World& world, const Node& n);
    void upsert_edge(ecs::World& world, const Edge& e);
    void drop_node(ecs::World& world, const NodeId& id);
    void drop_edge(ecs::World& world, const EdgeId& id);
    void seed_position(ecs::World& world, entt::entity e, const Node& n);

    ecs::ViewMode built_mode_ = ecs::ViewMode::Architecture;
    // Architecture view: the store edges that are drawn, and the nodes each one is
    // drawn between (its endpoints' representatives). See choose_drawn_edges.
    std::unordered_map<EdgeId, std::pair<NodeId, NodeId>> drawn_;
    // How many contract edges each drawn line stands for.
    std::unordered_map<EdgeId, int> weights_;
    bool          primed_     = false;
};

} // namespace rgv::systems
