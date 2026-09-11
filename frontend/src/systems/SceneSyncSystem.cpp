#include "rgv/systems/SceneSyncSystem.h"

#include "rgv/analysis/Specificity.h"
#include "rgv/ecs/Components.h"
#include "rgv/model/GraphStore.h"
#include "rgv/view/SemanticZoom.h"

#include <vector>
#include <algorithm>
#include <map>
#include <set>
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


// Size on the golden-ratio ladder, by how much of the repository depends on this node.
//
// The radial view sizes a disc by what it holds, which is what makes a Gource frame
// readable before you have read a single label. The box views had no equivalent: their
// footprint is whatever their name needs, so a package six others import was drawn the
// same as one nothing imports. `dependent_fraction` is the same number the relevance
// filter already runs on, so this adds a reading of the graph rather than a new measure
// of it.
//
// The filesystem view is left alone -- its discs already carry this, and scaling the
// box underneath them would double-count.
float prominence_of(const ecs::World& world, const NodeId& id) {
    constexpr float kGolden = 1.6180339887f;

    const auto& view = world.resource<ecs::ViewSettings>();
    if (view.mode == ecs::ViewMode::Filesystem) return 1.0f;

    const auto& spec = world.resource<ecs::DerivedState>().specificity;
    if (spec.population() <= 1) return 1.0f;

    const float frac = spec.dependent_fraction(id);
    if (frac >= 0.55f) return kGolden * kGolden;   // most of the repository depends on it
    if (frac >= 0.20f) return kGolden;             // a shared dependency, not a hub
    return 1.0f;
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

    if (requests.revisit) {
        revisit(world);
        requests.revisit = false;
    }

    if (store.dirty().any()) {
        incremental(world);
        store.clear_dirty();
    }
}

// The filters moved. Every node is re-tested and the difference applied, which is O(N)
// predicate calls and no allocation -- against a rebuild, which destroys the registry
// and reseeds every position from scratch. Dragging the relevance slider does this on
// every frame it moves, so what survives has to survive untouched.
void SceneSyncSystem::revisit(ecs::World& world) {
    const auto& store = world.resource<GraphStore>();
    auto&       index = world.resource<ecs::EntityIndex>();

    std::vector<NodeId> gone;
    for (const auto& [id, n] : store.nodes()) {
        const bool visible = node_visible(world, n);
        const bool present = index.node(id) != entt::null;
        if (visible && !present) upsert_node(world, n);
        else if (!visible && present) gone.push_back(id);
    }
    for (const auto& id : gone) drop_node(world, id);
    if (world.resource<ecs::ViewSettings>().mode == ecs::ViewMode::Architecture) {
        choose_drawn_edges(world);
        // What an edge is drawn between can move when a file arrives or leaves, so the
        // drawn set is re-derived and its endpoints refreshed.
        std::vector<EdgeId> stale;
        for (const auto& [eid, ent] : index.edges) {
            if (eid.rfind(kTreeEdgePrefix, 0) == 0) continue;
            if (!drawn_.count(eid)) stale.push_back(eid);
        }
        for (const auto& eid : stale) drop_edge(world, eid);
        for (const auto& [eid, pair] : drawn_) {
            if (const Edge* e = store.edge(eid)) upsert_edge(world, *e);
        }
        sync_containment(world);
    }

    // Edges follow: one may have become visible because its endpoint just arrived.
    std::vector<EdgeId> dead;
    for (const auto& [id, e] : store.edges()) {
        const bool visible = edge_visible(world, e);
        const bool present = index.edge(id) != entt::null;
        if (visible && !present) upsert_edge(world, e);
        else if (!visible && present) dead.push_back(id);
    }
    for (const auto& id : dead) drop_edge(world, id);

    count_hidden(world);
}

// -- visibility ---------------------------------------------------------------

namespace {

bool dependency_kind(EdgeKind k) {
    return k == EdgeKind::Imports || k == EdgeKind::Calls || k == EdgeKind::References;
}

// A file with any dependency in or out, counting the symbols it defines: a component
// module nobody imports directly is still what every system reads.
bool participates(const GraphStore& store, const Node& n) {
    for (const auto& eid : store.out_edges(n.id)) {
        if (const Edge* e = store.edge(eid); e && e->active() && dependency_kind(e->kind)) return true;
    }
    for (const auto& eid : store.in_edges(n.id)) {
        if (const Edge* e = store.edge(eid); e && e->active() && dependency_kind(e->kind)) return true;
    }
    for (const auto& cid : store.children(n.id)) {
        const Node* c = store.node(cid);
        if (!c || c->kind != NodeKind::Symbol) continue;
        for (const auto& eid : store.in_edges(cid)) {
            if (const Edge* e = store.edge(eid); e && e->active()) return true;
        }
    }
    return false;
}

} // namespace

// Hidden by a pattern the user typed: itself, or anything above it. A decision, not
// a heuristic, so nothing below -- not even a change -- exempts a node from it.
bool SceneSyncSystem::hidden(const ecs::World& world, const Node& n) const {
    const auto& f     = world.resource<ecs::Filters>();
    const auto& store = world.resource<GraphStore>();
    if (f.hidden.empty()) return false;
    const Node* cur = &n;
    for (int guard = 0; cur && guard < 64; ++guard) {
        if (ecs::hidden_by_pattern(f, cur->name, cur->path)) return true;
        if (cur->parent.empty()) break;
        cur = store.node(cur->parent);
    }
    return false;
}

bool SceneSyncSystem::node_visible(const ecs::World& world, const Node& n) const {
    const auto& view    = world.resource<ecs::ViewSettings>();
    const auto& f       = world.resource<ecs::Filters>();
    const auto& store   = world.resource<GraphStore>();
    const auto& derived = world.resource<ecs::DerivedState>();

    if (hidden(world, n)) return false;

    switch (view.mode) {
        case ecs::ViewMode::Architecture:
            // Packages, and inside them the files that take part in a dependency: a
            // module that imports, is imported, or defines something another file
            // uses. A README or a config file is not architecture and stays out.
            if (n.kind == NodeKind::ExternalPackage) return f.show_external;
            if (n.kind == NodeKind::File) {
                if (!participates(store, n)) return false;
                break;
            }
            if (n.kind != NodeKind::Package && n.kind != NodeKind::BuildTarget) return false;
            break;
        case ecs::ViewMode::Filesystem:
            // The repository is included so the tree has one centre to grow from.
            // Without it every package is a root and the layout is a ring with a hole.
            if (n.kind != NodeKind::Repository && n.kind != NodeKind::Directory &&
                n.kind != NodeKind::File && n.kind != NodeKind::Package) return false;
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

// The node that stands for `id` on screen: itself when it is drawn, else the nearest
// drawn ancestor. A symbol is represented by its file; a file that is filtered out, by
// its package.
NodeId SceneSyncSystem::representative(const ecs::World& world, NodeId id) const {
    const auto& store = world.resource<GraphStore>();
    const auto& index = world.resource<ecs::EntityIndex>();
    for (int guard = 0; guard < 64 && !id.empty(); ++guard) {
        if (index.node(id) != entt::null) return id;
        const Node* n = store.node(id);
        if (!n) return {};
        // A node hidden on purpose has no representative: its edges are gone, not
        // handed to its package. (Its ancestors are checked by `hidden` itself.)
        if (hidden(world, *n)) return {};
        id = n->parent;
    }
    return {};
}

// Which store edges are drawn in the nested architecture view, and between what.
//
// Edges live at their own level -- imports between files, reads and writes from a file
// to a symbol, declared dependencies between packages -- and the view draws each one
// between the nodes that stand for its endpoints. Several then land on the same pair:
// the import of `motion.py` and every read of a component inside it are all
// `movement.py -> motion.py`. One is drawn, the most specific, so the line says "reads
// CarPosition" rather than "imports". And a package-level edge whose contents already
// explain it -- a `depends_on` between two packages with a drawn file edge between
// their modules -- is not drawn at all: the modules are the explanation.
void SceneSyncSystem::choose_drawn_edges(ecs::World& world) {
    const auto& store = world.resource<GraphStore>();
    const auto& f     = world.resource<ecs::Filters>();
    auto&       index = world.resource<ecs::EntityIndex>();
    drawn_.clear();
    index.aliases.clear();

    auto priority = [](EdgeKind k) {
        switch (k) {
            case EdgeKind::Calls:      return 3;
            case EdgeKind::References: return 2;
            case EdgeKind::Imports:    return 1;
            default:                   return 0;
        }
    };
    auto passes = [&](const Edge& e) {
        if (!e.active()) return false;
        if (!f.show_heuristic &&
            (e.confidence == Confidence::Heuristic || e.confidence == Confidence::Unresolved)) {
            return false;
        }
        if (!f.show_stale && e.freshness == Freshness::Stale) return false;
        return true;
    };

    struct Pick { int prio; EdgeId id; NodeId from, to; };
    std::map<std::pair<NodeId, NodeId>, Pick>         best;
    std::set<std::pair<NodeId, NodeId>>               explained;   // package pairs with a drawn file edge
    std::vector<std::pair<const Edge*, std::pair<NodeId, NodeId>>> declared;

    for (const auto& [id, e] : store.edges()) {
        if (!passes(e)) continue;
        const bool dep = e.kind == EdgeKind::DependsOn;
        if (!dep && !dependency_kind(e.kind)) continue;
        const NodeId rf = representative(world, e.from);
        const NodeId rt = representative(world, e.to);
        if (rf.empty() || rt.empty() || rf == rt) continue;
        if (dep) { declared.emplace_back(&e, std::make_pair(rf, rt)); continue; }

        const Pick pick{priority(e.kind), id, rf, rt};
        auto [it, fresh] = best.try_emplace({rf, rt}, pick);
        if (!fresh && (pick.prio > it->second.prio ||
                       (pick.prio == it->second.prio && pick.id < it->second.id))) {
            it->second = pick;
        }
    }
    std::map<std::pair<NodeId, NodeId>, std::vector<EdgeId>> by_owner_pair;
    for (const auto& [pair, pick] : best) {
        auto owner = [&](const NodeId& id) {
            const Node* n = store.node(id);
            if (n && n->kind == NodeKind::Package) return id;
            return store.ancestor_of_kind(id, NodeKind::Package);
        };
        const auto owners = std::make_pair(owner(pick.from), owner(pick.to));
        explained.insert(owners);
        by_owner_pair[owners].push_back(pick.id);
    }
    for (const auto& [e, pair] : declared) {
        if (auto it = by_owner_pair.find({e->from, e->to}); it != by_owner_pair.end()) {
            index.aliases[e->id] = it->second;
            continue;
        }
        const Pick pick{0, e->id, pair.first, pair.second};
        auto [it, fresh] = best.try_emplace(pair, pick);
        if (!fresh && pick.prio == it->second.prio && pick.id < it->second.id) it->second = pick;
    }
    for (const auto& [pair, pick] : best) drawn_[pick.id] = pair;
}

bool SceneSyncSystem::edge_visible(const ecs::World& world, const Edge& e) const {
    const auto& view  = world.resource<ecs::ViewSettings>();
    const auto& f     = world.resource<ecs::Filters>();
    const auto& index = world.resource<ecs::EntityIndex>();

    if (!e.active()) return false;
    if (view.mode == ecs::ViewMode::Architecture) return drawn_.count(e.id) > 0;
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
        registry.emplace<ecs::Unplaced>(ent);
        seed_position(world, ent, n);
    } else {
        registry.get<ecs::NodeRef>(ent).kind = n.kind;
    }

    const std::string sub = secondary_line(n, view.mode);
    registry.emplace_or_replace<ecs::Label>(ent, ecs::Label{n.name, sub});
    const float prom = prominence_of(world, n.id);
    registry.emplace_or_replace<ecs::Prominence>(ent, ecs::Prominence{prom});
    registry.emplace_or_replace<ecs::Extent>(
        ent, ecs::Extent{view::text_extent(n.kind, n.name, sub, view.graph_text_scale) * prom});
    registry.emplace_or_replace<ecs::FreshnessState>(ent, ecs::FreshnessState{n.freshness});
}

void SceneSyncSystem::upsert_edge(ecs::World& world, const Edge& e) {
    auto& registry = world.registry;
    auto& index    = world.resource<ecs::EntityIndex>();

    entt::entity from = index.node(e.from);
    entt::entity to   = index.node(e.to);
    if (auto it = drawn_.find(e.id); it != drawn_.end()) {
        from = index.node(it->second.first);
        to   = index.node(it->second.second);
    }
    if (from == entt::null || to == entt::null) return;

    entt::entity ent = index.edge(e.id);
    if (ent == entt::null) {
        ent               = registry.create();
        index.edges[e.id] = ent;
        registry.emplace<ecs::EdgeRef>(ent, ecs::EdgeRef{e.id, e.kind});
        registry.emplace<ecs::Style>(ent);
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
    if (view.mode == ecs::ViewMode::Architecture) choose_drawn_edges(world);
    for (const auto& [id, e] : store.edges()) {
        if (edge_visible(world, e)) upsert_edge(world, e);
    }

    if (view.mode == ecs::ViewMode::Filesystem || view.mode == ecs::ViewMode::Architecture) {
        sync_containment(world);
    }
    (void)registry;
    (void)index;
}

// Containment rendered as synthetic edges, child -> the node that stands for its
// parent. They are not contract edges, so they carry a distinct id prefix and the
// inspector offers no provenance. The filesystem view draws them as the tree; the
// architecture view draws nothing for them and lays a child out inside its parent.
void SceneSyncSystem::sync_containment(ecs::World& world) {
    auto&       registry = world.registry;
    auto&       index    = world.resource<ecs::EntityIndex>();
    const auto& store    = world.resource<GraphStore>();

    std::vector<EdgeId> old;
    for (const auto& [eid, ent] : index.edges) {
        if (eid.rfind(kTreeEdgePrefix, 0) == 0) old.push_back(eid);
    }
    for (const auto& eid : old) drop_edge(world, eid);

    for (const auto& [id, n] : store.nodes()) {
        if (index.node(id) == entt::null || n.parent.empty()) continue;
        const NodeId parent = representative(world, n.parent);
        if (parent.empty() || parent == id) continue;
        const EdgeId eid = kTreeEdgePrefix + id;
        entt::entity ent = registry.create();
        index.edges[eid] = ent;
        registry.emplace<ecs::EdgeRef>(ent, ecs::EdgeRef{eid, EdgeKind::Contains});
        registry.emplace<ecs::Style>(ent);
        registry.emplace<ecs::Endpoints>(ent, ecs::Endpoints{index.node(id), index.node(parent)});
        registry.emplace<ecs::FreshnessState>(ent, ecs::FreshnessState{n.freshness});
        registry.emplace<ecs::ConfidenceState>(ent, ecs::ConfidenceState{Confidence::Exact});
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
    if (world.resource<ecs::ViewSettings>().mode == ecs::ViewMode::Architecture) {
        // Representatives may have moved and pairs may have a new best edge, and a
        // dirty set does not say which, so the drawn set is re-derived whole. It is a
        // pass over the edges, not a rebuild: nothing on screen is torn down.
        choose_drawn_edges(world);
        const auto& index = world.resource<ecs::EntityIndex>();
        std::vector<EdgeId> stale;
        for (const auto& [eid, ent] : index.edges) {
            if (eid.rfind(kTreeEdgePrefix, 0) == 0) continue;
            if (!drawn_.count(eid)) stale.push_back(eid);
        }
        for (const auto& eid : stale) drop_edge(world, eid);
        for (const auto& [eid, pair] : drawn_) {
            if (const Edge* e = store.edge(eid)) upsert_edge(world, *e);
        }
        if (!dirty.nodes.empty()) sync_containment(world);
        return;
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
        const float prom = prominence_of(world, ref.id);
        world.registry.emplace_or_replace<ecs::Prominence>(ent, ecs::Prominence{prom});
        ext.half = view::text_extent(ref.kind, label.text, label.sub, view.graph_text_scale) * prom;
    }
    // Boxes changed size, so neighbours that used to clear each other may not any
    // more. That is a reason to let them push apart, not a reason to move every node in
    // the graph -- this slider is dragged, and a relayout per frame is the jitter.
    world.resource<ecs::SceneRequests>().resettle = true;
}

} // namespace rgv::systems
