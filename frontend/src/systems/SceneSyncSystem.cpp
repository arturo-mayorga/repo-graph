#include "rgv/systems/SceneSyncSystem.h"

#include "rgv/analysis/Specificity.h"
#include "rgv/ecs/Components.h"
#include "rgv/model/GraphStore.h"
#include "rgv/view/SemanticZoom.h"

#include <algorithm>
#include <cctype>

namespace rgv::systems {
namespace {

constexpr const char* kTreeEdgePrefix = "tree:";

// Deterministic pseudo-jitter. rand() here would make layout differ between runs,
// which makes screenshots and bug reports useless.
float hash_unit(const std::string& s, std::uint32_t salt) {
    std::uint32_t h = 2166136261u ^ salt;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != hay.end();
}

std::string secondary_line(const Node& n, ecs::ViewMode mode) {
    if (mode != ecs::ViewMode::Architecture && !n.language.empty()) return n.language;
    return n.path;
}

} // namespace

void SceneSyncSystem::setup(ecs::World& world) {
    built_mode_ = world.resource<ecs::ViewSettings>().mode;
}

void SceneSyncSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&       requests = world.resource<ecs::SceneRequests>();
    const auto& view     = world.resource<ecs::ViewSettings>();
    auto&       store    = world.resource<GraphStore>();

    if (requests.refresh_extents) {
        refresh_extents(world);
        requests.refresh_extents = false;
    }

    // The visible node set is a function of the view mode, so changing it is a rebuild
    // rather than a delta.
    if (!primed_ || built_mode_ != view.mode || requests.rebuild) {
        rebuild(world);
        requests.rebuild = false;
        store.clear_dirty();
        return;
    }

    if (store.dirty().any()) {
        incremental(world);
        store.clear_dirty();
    }
}

// -- visibility ---------------------------------------------------------------

bool SceneSyncSystem::node_visible(const ecs::World& world, const Node& n) const {
    const auto& view    = world.resource<ecs::ViewSettings>();
    const auto& f       = world.resource<ecs::Filters>();
    const auto& store   = world.resource<GraphStore>();
    const auto& derived = world.resource<ecs::DerivedState>();

    switch (view.mode) {
        case ecs::ViewMode::Architecture:
            if (n.kind == NodeKind::ExternalPackage) return f.show_external;
            if (n.kind != NodeKind::Package && n.kind != NodeKind::BuildTarget) return false;
            break;
        case ecs::ViewMode::Filesystem:
            if (n.kind != NodeKind::Directory && n.kind != NodeKind::File &&
                n.kind != NodeKind::Package) return false;
            break;
        case ecs::ViewMode::FileGraph:
            if (n.kind != NodeKind::File) return false;
            break;
    }

    if (!f.text.empty() && !contains_ci(n.name, f.text) && !contains_ci(n.path, f.text)) {
        return false;
    }
    if (n.freshness == Freshness::Stale && !f.show_stale) return false;

    // Stop words. A package most of the repository depends on adds no architectural
    // information -- "X depends on it" is true of nearly everything -- so above the
    // threshold it leaves the view entirely rather than being drawn dimmer. Same idea
    // as dropping "the" from a query, and the reason specificity is measured at all.
    if (f.min_relevance > 0.0f && derived.specificity.population() > 1 &&
        derived.specificity.specificity(n.id) < f.min_relevance &&
        !exempt_from_filters(world, n)) {
        return false;
    }

    if (f.show_unaffected) return true;

    // "Show me only what the agent touched plus affected context."
    if (exempt_from_filters(world, n)) return true;
    if (const ImpactResult* r = store.impact(view.level)) {
        for (const auto& in : r->impacted_nodes) {
            if (in.node_id != n.id || in.min_distance > f.max_impact_depth) continue;
            // Same rule as styling: a low-relevance result is not worth keeping on
            // screen when the user asked for only what matters.
            return in.min_distance == 0 ||
                   analysis::relevance(store, derived.specificity, in) >= f.min_relevance;
        }
    }
    return false;
}

bool SceneSyncSystem::exempt_from_filters(const ecs::World& world, const Node& n) const {
    const auto& store = world.resource<GraphStore>();
    const auto& view  = world.resource<ecs::ViewSettings>();

    for (const auto& c : store.changed_files()) {
        if (c.node_id == n.id) return true;
        // A changed file keeps its owning package on screen at architecture level.
        if (store.ancestor_of_kind(c.node_id, n.kind) == n.id) return true;
    }
    if (const ImpactResult* r = store.impact(view.level)) {
        for (const auto& seed : r->seed_nodes) {
            if (seed == n.id) return true;
        }
    }
    return false;
}

// Counted over the nodes the current view could show, so it reports stop words dropped
// rather than everything the view mode already excludes.
void SceneSyncSystem::count_hidden(ecs::World& world) const {
    const auto& store   = world.resource<GraphStore>();
    const auto& filters = world.resource<ecs::Filters>();
    const auto& derived = world.resource<ecs::DerivedState>();
    auto&       stats   = world.resource<ecs::SceneStats>();

    stats.hidden = 0;
    if (filters.min_relevance <= 0.0f || derived.specificity.population() <= 1) return;

    for (const auto& [id, n] : store.nodes()) {
        if (derived.specificity.dependents(id) == 0 &&
            derived.specificity.specificity(id) >= 1.0f) {
            continue;   // not in the scored population at all
        }
        if (derived.specificity.specificity(id) < filters.min_relevance &&
            !exempt_from_filters(world, n)) {
            ++stats.hidden;
        }
    }
}

bool SceneSyncSystem::edge_visible(const ecs::World& world, const Edge& e) const {
    const auto& view  = world.resource<ecs::ViewSettings>();
    const auto& f     = world.resource<ecs::Filters>();
    const auto& index = world.resource<ecs::EntityIndex>();

    if (!e.active()) return false;
    if (index.node(e.from) == entt::null || index.node(e.to) == entt::null) return false;
    if (!f.show_heuristic &&
        (e.confidence == Confidence::Heuristic || e.confidence == Confidence::Unresolved)) {
        return false;
    }
    if (!f.show_stale && e.freshness == Freshness::Stale) return false;

    switch (view.mode) {
        case ecs::ViewMode::Architecture: return e.kind == EdgeKind::DependsOn;
        case ecs::ViewMode::FileGraph:    return e.kind == EdgeKind::Imports;
        // Containment is drawn from the parent links, not from edges, so real
        // dependency edges do not clutter the filesystem view.
        case ecs::ViewMode::Filesystem:   return false;
    }
    return false;
}

// -- entity lifecycle ---------------------------------------------------------

void SceneSyncSystem::seed_position(ecs::World& world, entt::entity ent, const Node& n) {
    auto&       registry = world.registry;
    const auto& index    = world.resource<ecs::EntityIndex>();
    const auto& store    = world.resource<GraphStore>();

    // Place a new node near whatever it connects to that is already on screen, so a
    // package appearing mid-session does not fly in from the origin.
    Vec2 sum{0.0f, 0.0f};
    int  count  = 0;
    auto sample = [&](const NodeId& other) {
        const entt::entity e = index.node(other);
        if (e == entt::null) return;
        if (auto* p = registry.try_get<ecs::Position>(e)) { sum += p->p; ++count; }
    };
    for (const auto& eid : store.out_edges(n.id)) {
        if (const Edge* e = store.edge(eid)) sample(e->to);
    }
    for (const auto& eid : store.in_edges(n.id)) {
        if (const Edge* e = store.edge(eid)) sample(e->from);
    }
    if (!n.parent.empty()) sample(n.parent);

    const float jx   = (hash_unit(n.id, 1) - 0.5f) * 140.0f;
    const float jy   = (hash_unit(n.id, 2) - 0.5f) * 90.0f;
    const Vec2  base = count > 0 ? sum / static_cast<float>(count) : Vec2{0.0f, 0.0f};
    registry.emplace_or_replace<ecs::Position>(ent, ecs::Position{base + Vec2{jx, jy}});
}

void SceneSyncSystem::upsert_node(ecs::World& world, const Node& n) {
    auto&       registry = world.registry;
    auto&       index    = world.resource<ecs::EntityIndex>();
    const auto& view     = world.resource<ecs::ViewSettings>();

    entt::entity ent = index.node(n.id);
    if (ent == entt::null) {
        ent              = registry.create();
        index.nodes[n.id] = ent;
        registry.emplace<ecs::NodeRef>(ent, ecs::NodeRef{n.id, n.kind});
        registry.emplace<ecs::Depth>(ent);
        registry.emplace<ecs::Style>(ent);
        seed_position(world, ent, n);
        world.resource<ecs::SceneRequests>().relayout = true;
    } else {
        registry.get<ecs::NodeRef>(ent).kind = n.kind;
    }

    const std::string sub = secondary_line(n, view.mode);
    registry.emplace_or_replace<ecs::Label>(ent, ecs::Label{n.name, sub});
    registry.emplace_or_replace<ecs::Extent>(
        ent, ecs::Extent{view::text_extent(n.kind, n.name, sub, view.graph_text_scale)});
    registry.emplace_or_replace<ecs::FreshnessState>(ent, ecs::FreshnessState{n.freshness});
}

void SceneSyncSystem::upsert_edge(ecs::World& world, const Edge& e) {
    auto& registry = world.registry;
    auto& index    = world.resource<ecs::EntityIndex>();

    const entt::entity from = index.node(e.from);
    const entt::entity to   = index.node(e.to);
    if (from == entt::null || to == entt::null) return;

    entt::entity ent = index.edge(e.id);
    if (ent == entt::null) {
        ent               = registry.create();
        index.edges[e.id] = ent;
        registry.emplace<ecs::EdgeRef>(ent, ecs::EdgeRef{e.id, e.kind});
        registry.emplace<ecs::Style>(ent);
        world.resource<ecs::SceneRequests>().relayout = true;
    } else {
        registry.get<ecs::EdgeRef>(ent).kind = e.kind;
    }
    registry.emplace_or_replace<ecs::Endpoints>(ent, ecs::Endpoints{from, to});
    registry.emplace_or_replace<ecs::FreshnessState>(ent, ecs::FreshnessState{e.freshness});
    registry.emplace_or_replace<ecs::ConfidenceState>(ent, ecs::ConfidenceState{e.confidence});
}

void SceneSyncSystem::drop_node(ecs::World& world, const NodeId& id) {
    auto& registry = world.registry;
    auto& index    = world.resource<ecs::EntityIndex>();

    const entt::entity ent = index.node(id);
    if (ent == entt::null) return;

    // Edges referencing this entity go first, or the renderer dereferences a destroyed
    // entity next frame.
    std::vector<EdgeId> doomed;
    for (const auto& [eid, e] : index.edges) {
        const auto* ep = registry.try_get<ecs::Endpoints>(e);
        if (ep && (ep->from == ent || ep->to == ent)) doomed.push_back(eid);
    }
    for (const auto& eid : doomed) drop_edge(world, eid);

    registry.destroy(ent);
    index.nodes.erase(id);
    world.resource<ecs::SceneRequests>().relayout = true;
}

void SceneSyncSystem::drop_edge(ecs::World& world, const EdgeId& id) {
    auto& index = world.resource<ecs::EntityIndex>();
    const entt::entity ent = index.edge(id);
    if (ent == entt::null) return;
    world.registry.destroy(ent);
    index.edges.erase(id);
}

// -- whole and incremental passes ---------------------------------------------

void SceneSyncSystem::rebuild(ecs::World& world) {
    auto&       registry = world.registry;
    auto&       index    = world.resource<ecs::EntityIndex>();
    const auto& store    = world.resource<GraphStore>();
    const auto& view     = world.resource<ecs::ViewSettings>();

    registry.clear();
    index.nodes.clear();
    index.edges.clear();
    built_mode_ = view.mode;
    primed_     = true;
    world.resource<ecs::SceneRequests>().relayout = true;

    for (const auto& [id, n] : store.nodes()) {
        if (node_visible(world, n)) upsert_node(world, n);
    }
    count_hidden(world);
    for (const auto& [id, e] : store.edges()) {
        if (edge_visible(world, e)) upsert_edge(world, e);
    }

    if (view.mode == ecs::ViewMode::Filesystem) {
        // Containment rendered as synthetic edges. They are not contract edges, so
        // they carry a distinct id prefix and the inspector offers no provenance.
        for (const auto& [id, n] : store.nodes()) {
            if (index.node(id) == entt::null || n.parent.empty() ||
                index.node(n.parent) == entt::null) {
                continue;
            }
            const EdgeId eid = kTreeEdgePrefix + id;
            entt::entity ent = registry.create();
            index.edges[eid] = ent;
            registry.emplace<ecs::EdgeRef>(ent, ecs::EdgeRef{eid, EdgeKind::Contains});
            registry.emplace<ecs::Style>(ent);
            registry.emplace<ecs::Endpoints>(
                ent, ecs::Endpoints{index.node(id), index.node(n.parent)});
            registry.emplace<ecs::FreshnessState>(ent, ecs::FreshnessState{n.freshness});
            registry.emplace<ecs::ConfidenceState>(ent, ecs::ConfidenceState{Confidence::Exact});
        }
    }
}

void SceneSyncSystem::incremental(ecs::World& world) {
    const auto&     store = world.resource<GraphStore>();
    const DirtySet& dirty = store.dirty();

    for (const auto& id : dirty.nodes) {
        const Node* n = store.node(id);
        if (!n || !node_visible(world, *n)) drop_node(world, id);
        else upsert_node(world, *n);
    }
    // Twice over the edges: a node may have arrived after an edge that references it,
    // and an edge whose endpoint was just created is only resolvable on the second go.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& id : dirty.edges) {
            const Edge* e = store.edge(id);
            if (!e || !edge_visible(world, *e)) {
                if (pass == 0) drop_edge(world, id);
            } else {
                upsert_edge(world, *e);
            }
        }
    }
}

void SceneSyncSystem::refresh_extents(ecs::World& world) {
    const auto& view = world.resource<ecs::ViewSettings>();
    for (auto [ent, ref, label, ext] :
         world.registry.view<const ecs::NodeRef, const ecs::Label, ecs::Extent>().each()) {
        ext.half = view::text_extent(ref.kind, label.text, label.sub, view.graph_text_scale);
    }
    // Boxes changed size, so the row packing layout computed is now wrong.
    world.resource<ecs::SceneRequests>().relayout = true;
}

} // namespace rgv::systems
