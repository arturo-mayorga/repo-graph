#include "rgv/ecs/Scene.h"

#include "rgv/ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace rgv::ecs {
namespace {

constexpr const char* kTreeEdgePrefix = "tree:";

entt::entity to_entity(std::uint32_t v) { return static_cast<entt::entity>(v); }
std::uint32_t to_raw(entt::entity e) { return static_cast<std::uint32_t>(e); }

// Deterministic pseudo-jitter. A rand() here would make layout differ between runs,
// which makes screenshots and bug reports useless.
float hash_unit(const std::string& s, std::uint32_t salt) {
    std::uint32_t h = 2166136261u ^ salt;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

// Severity order for evidence quality. Used to combine what a node says about itself
// with what the impact result says about the path that reached it: the weaker of the
// two wins, because a conclusion is only as fresh as its worst hop.
int freshness_rank(Freshness f) {
    switch (f) {
        case Freshness::Current: return 0;
        case Freshness::Pending: return 1;
        case Freshness::Stale:   return 2;
        case Freshness::Invalid: return 3;
    }
    return 0;
}

Freshness worse_of(Freshness a, Freshness b) {
    return freshness_rank(a) >= freshness_rank(b) ? a : b;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != hay.end();
}

// Node size follows its text. ImGui's default font is a fixed-advance bitmap face, so
// a character-count estimate matches CalcTextSize closely enough to size a box -- and
// it keeps the ECS layer free of any dependency on the UI toolkit.
Vec2 text_extent(NodeKind k, const std::string& name, const std::string& sub, float text_scale) {
    const float       advance = kBaseFontPx * text_scale * kCharAdvanceRatio;
    const std::size_t widest  = std::max(name.size(), sub.size());
    const float       text_w  = static_cast<float>(widest) * advance;

    float min_w = 92.0f, h = 16.0f;
    switch (k) {
        case NodeKind::Package:
        case NodeKind::BuildTarget:     min_w = 124.0f; h = 21.0f; break;
        case NodeKind::ExternalPackage: min_w = 104.0f; h = 17.0f; break;
        case NodeKind::Directory:       min_w = 96.0f;  h = 16.0f; break;
        default:                        break;
    }
    // Two text lines stack inside the box when zoomed in, so the height has to carry
    // the secondary line as well as the name.
    return Vec2{std::max(min_w * text_scale, text_w + 26.0f * text_scale) * 0.5f,
                h * text_scale};
}

} // namespace

const char* to_label(ViewMode m) {
    switch (m) {
        case ViewMode::Architecture: return "Architecture";
        case ViewMode::Filesystem:   return "Filesystem";
        case ViewMode::FileGraph:    return "File graph";
    }
    return "?";
}

NodeDetail node_detail(const ViewState& view) {
    NodeDetail d;
    // Labels are drawn in world space, so their on-screen size is the base font times
    // the user's scale times zoom. That single number decides everything: how big the
    // text is, whether it is worth drawing, and whether the node is a box or a dot.
    d.font_px = kBaseFontPx * view.graph_text_scale * view.camera.zoom;

    // Below ~6px nothing is legible; by ~11px a two-line label is comfortable.
    const float lo = 6.0f, hi = 11.0f;
    const float x  = std::clamp((d.font_px - lo) / (hi - lo), 0.0f, 1.0f);
    d.t      = x * x * (3.0f - 2.0f * x);   // smoothstep, so the transition is not a pop
    d.labels = d.t > 0.02f;
    return d;
}

float dot_px_for(bool changed, bool impacted, bool emphasised) {
    float r = 4.5f;
    if (impacted) r = 6.0f;
    if (changed) r = 7.5f;
    if (emphasised) r += 2.0f;   // selected or hovered stays findable at any zoom
    return r;
}

Vec2 render_half(const ViewState& view, const NodeDetail& detail, const Vec2& layout_half,
                 float dot_px) {
    // A dot holds a constant screen size, so an overview stays a readable constellation
    // instead of fading to nothing as the user zooms out.
    const float zoom = std::max(view.camera.zoom, 1e-4f);
    const Vec2  dot{dot_px / zoom, dot_px / zoom};
    return lerp(dot, layout_half, detail.t);
}

bool Scene::node_visible(const GraphStore& store, const Node& n) const {
    const auto& f = view.filters;

    switch (view.mode) {
        case ViewMode::Architecture:
            if (n.kind == NodeKind::ExternalPackage) return f.show_external;
            if (n.kind != NodeKind::Package && n.kind != NodeKind::BuildTarget) return false;
            break;
        case ViewMode::Filesystem:
            if (n.kind != NodeKind::Directory && n.kind != NodeKind::File &&
                n.kind != NodeKind::Package) return false;
            break;
        case ViewMode::FileGraph:
            if (n.kind != NodeKind::File) return false;
            break;
    }

    if (!f.text.empty() && !contains_ci(n.name, f.text) && !contains_ci(n.path, f.text)) {
        return false;
    }
    if (n.freshness == Freshness::Stale && !f.show_stale) return false;

    if (!f.show_unaffected) {
        // "Show me only what the agent touched plus affected context." A node stays if
        // it is changed, or if the current impact result names it.
        bool keep = false;
        for (const auto& c : store.changed_files()) {
            if (c.node_id == n.id) { keep = true; break; }
            // A changed file keeps its owning package on screen at architecture level.
            if (view.mode == ViewMode::Architecture &&
                store.ancestor_of_kind(c.node_id, n.kind) == n.id) { keep = true; break; }
        }
        if (!keep) {
            if (const ImpactResult* r = store.impact(view.level)) {
                for (const auto& in : r->impacted_nodes) {
                    if (in.node_id != n.id || in.min_distance > f.max_impact_depth) continue;
                    // Same rule as styling: a low-relevance result is not worth
                    // keeping on screen when the user asked for only what matters.
                    keep = in.min_distance == 0 ||
                           analysis::relevance(store, specificity_, in) >= f.min_relevance;
                    break;
                }
            }
        }
        if (!keep) return false;
    }
    return true;
}

bool Scene::edge_visible(const GraphStore& store, const Edge& e) const {
    if (!e.active()) return false;
    if (!node_index_.count(e.from) || !node_index_.count(e.to)) return false;
    if (!view.filters.show_heuristic &&
        (e.confidence == Confidence::Heuristic || e.confidence == Confidence::Unresolved)) {
        return false;
    }
    if (!view.filters.show_stale && e.freshness == Freshness::Stale) return false;

    switch (view.mode) {
        case ViewMode::Architecture: return e.kind == EdgeKind::DependsOn;
        case ViewMode::FileGraph:    return e.kind == EdgeKind::Imports;
        // Containment is drawn from the parent links, not from edges, so real
        // dependency edges do not clutter the filesystem view.
        case ViewMode::Filesystem:   return false;
    }
    return false;
}

void Scene::seed_position(entt::entity ent, const GraphStore& store, const Node& n) {
    // Place a new node near whatever it connects to that is already on screen, so a
    // package appearing mid-session does not fly in from the origin.
    Vec2 sum{0.0f, 0.0f};
    int  count = 0;
    auto sample = [&](const NodeId& other) {
        auto it = node_index_.find(other);
        if (it == node_index_.end()) return;
        if (auto* p = registry.try_get<Position>(it->second)) { sum += p->p; ++count; }
    };
    for (const auto& eid : store.out_edges(n.id)) {
        if (const Edge* e = store.edge(eid)) sample(e->to);
    }
    for (const auto& eid : store.in_edges(n.id)) {
        if (const Edge* e = store.edge(eid)) sample(e->from);
    }
    if (!n.parent.empty()) sample(n.parent);

    const float jx = (hash_unit(n.id, 1) - 0.5f) * 140.0f;
    const float jy = (hash_unit(n.id, 2) - 0.5f) * 90.0f;
    const Vec2  base = count > 0 ? sum / static_cast<float>(count) : Vec2{0.0f, 0.0f};
    registry.emplace_or_replace<Position>(ent, Position{base + Vec2{jx, jy}});
    registry.emplace_or_replace<Velocity>(ent, Velocity{});
}

void Scene::upsert_node(const GraphStore& store, const Node& n) {
    auto it = node_index_.find(n.id);
    entt::entity ent;
    if (it == node_index_.end()) {
        ent              = registry.create();
        node_index_[n.id] = ent;
        registry.emplace<NodeRef>(ent, NodeRef{n.id, n.kind});
        registry.emplace<Extent>(ent, Extent{});
        registry.emplace<Depth>(ent, Depth{0});
        registry.emplace<Style>(ent, Style{});
        seed_position(ent, store, n);
        needs_layout_reset = true;
    } else {
        ent = it->second;
        registry.get<NodeRef>(ent).kind = n.kind;
    }

    std::string sub = n.path;
    if (view.mode != ViewMode::Architecture && !n.language.empty()) sub = n.language;
    registry.emplace_or_replace<Label>(ent, Label{n.name, sub});
    registry.emplace_or_replace<Extent>(
        ent, Extent{text_extent(n.kind, n.name, sub, view.graph_text_scale)});
    registry.emplace_or_replace<FreshnessState>(ent, FreshnessState{n.freshness});
}

void Scene::upsert_edge(const GraphStore& store, const Edge& e) {
    auto from = node_index_.find(e.from);
    auto to   = node_index_.find(e.to);
    if (from == node_index_.end() || to == node_index_.end()) return;

    auto it = edge_index_.find(e.id);
    entt::entity ent;
    if (it == edge_index_.end()) {
        ent               = registry.create();
        edge_index_[e.id] = ent;
        registry.emplace<EdgeRef>(ent, EdgeRef{e.id, e.kind});
        registry.emplace<Style>(ent, Style{});
        needs_layout_reset = true;
    } else {
        ent = it->second;
        registry.get<EdgeRef>(ent).kind = e.kind;
    }
    registry.emplace_or_replace<Endpoints>(ent, Endpoints{to_raw(from->second), to_raw(to->second)});
    registry.emplace_or_replace<FreshnessState>(ent, FreshnessState{e.freshness});
    registry.emplace_or_replace<ConfidenceState>(ent, ConfidenceState{e.confidence});
}

void Scene::drop_node(const NodeId& id) {
    auto it = node_index_.find(id);
    if (it == node_index_.end()) return;
    // Edges referencing this entity must go first, or the renderer dereferences a
    // destroyed entity next frame.
    std::vector<EdgeId> doomed;
    for (const auto& [eid, ent] : edge_index_) {
        const auto* ep = registry.try_get<Endpoints>(ent);
        if (ep && (to_entity(ep->from) == it->second || to_entity(ep->to) == it->second)) {
            doomed.push_back(eid);
        }
    }
    for (const auto& eid : doomed) drop_edge(eid);

    registry.destroy(it->second);
    node_index_.erase(it);
    needs_layout_reset = true;
}

void Scene::drop_edge(const EdgeId& id) {
    auto it = edge_index_.find(id);
    if (it == edge_index_.end()) return;
    registry.destroy(it->second);
    edge_index_.erase(it);
}

void Scene::rebuild(const GraphStore& store) {
    // Before filtering, because node_visible() consults relevance.
    refresh_specificity(store);

    registry.clear();
    node_index_.clear();
    edge_index_.clear();
    built_mode_        = view.mode;
    needs_layout_reset = true;

    for (const auto& [id, n] : store.nodes()) {
        if (node_visible(store, n)) upsert_node(store, n);
    }
    for (const auto& [id, e] : store.edges()) {
        if (edge_visible(store, e)) upsert_edge(store, e);
    }
    if (view.mode == ViewMode::Filesystem) {
        // Containment rendered as synthetic edges. They are not contract edges, so
        // they carry a distinct id prefix and the inspector will not offer provenance.
        for (const auto& [id, n] : store.nodes()) {
            if (!node_index_.count(id) || n.parent.empty() || !node_index_.count(n.parent)) {
                continue;
            }
            const EdgeId  eid = kTreeEdgePrefix + id;
            entt::entity  ent = registry.create();
            edge_index_[eid]  = ent;
            registry.emplace<EdgeRef>(ent, EdgeRef{eid, EdgeKind::Contains});
            registry.emplace<Style>(ent, Style{});
            registry.emplace<Endpoints>(
                ent, Endpoints{to_raw(node_index_[id]), to_raw(node_index_[n.parent])});
            registry.emplace<FreshnessState>(ent, FreshnessState{n.freshness});
            registry.emplace<ConfidenceState>(ent, ConfidenceState{Confidence::Exact});
        }
    }
    restyle(store);
}

void Scene::sync(const GraphStore& store) {
    if (built_mode_ != view.mode) { rebuild(store); return; }

    const DirtySet& dirty = store.dirty();

    // Visibility is a predicate over impact and filters, so a change to either can
    // add or remove nodes without the graph itself changing. Only the dirty ids are
    // reconsidered here; a filter change goes through rebuild() instead.
    for (const auto& id : dirty.nodes) {
        const Node* n = store.node(id);
        if (!n || !node_visible(store, *n)) drop_node(id);
        else upsert_node(store, *n);
    }
    for (const auto& id : dirty.edges) {
        const Edge* e = store.edge(id);
        if (!e || !edge_visible(store, *e)) drop_edge(id);
        else upsert_edge(store, *e);
    }
    // A new node may have arrived after an edge that references it.
    for (const auto& id : dirty.edges) {
        if (const Edge* e = store.edge(id); e && edge_visible(store, *e)) upsert_edge(store, *e);
    }
    if (dirty.impact || dirty.changes || !dirty.nodes.empty() || !dirty.edges.empty()) {
        restyle(store);
    }
}

// Rebuilt rather than cached against topology: restyle already walks everything, and a
// stale index would mis-rank the very edit the user is looking at. It also has to run
// before visibility filtering, which consults relevance.
void Scene::refresh_specificity(const GraphStore& store) {
    const ImpactResult* result = store.impact(view.level);

    ImpactFilters traversal;
    if (result) traversal = result->filters;
    traversal.include_heuristic = view.filters.show_heuristic;

    specificity_ = analysis::build(store, view.level, traversal);

    hub_alerts_.clear();
    if (result) {
        hub_alerts_ = analysis::hub_seeds(store, specificity_, *result, analysis::kHubThreshold);
    }
}

void Scene::refresh_extents() {
    for (auto [ent, ref, label, ext] :
         registry.view<const NodeRef, const Label, Extent>().each()) {
        ext.half = text_extent(ref.kind, label.text, label.sub, view.graph_text_scale);
    }
    // Boxes changed size, so the row packing layout computed is now wrong.
    needs_layout_reset = true;
}

void Scene::restyle(const GraphStore& store) {
    const ui::Theme& t = ui::theme();

    refresh_specificity(store);
    const ImpactResult* result = store.impact(view.level);

    // Impact and changed state are cheap lookups built once per restyle rather than
    // per node, so this stays linear on large graphs.
    std::unordered_map<NodeId, const ImpactedNode*> impact_by_id;
    if (result) {
        for (const auto& in : result->impacted_nodes) impact_by_id[in.node_id] = &in;
    }
    std::unordered_map<NodeId, const analysis::HubAlert*> hub_by_id;
    for (const auto& h : hub_alerts_) hub_by_id[h.node] = &h;
    std::unordered_map<NodeId, const ChangedFile*> changed_by_id;
    for (const auto& c : store.changed_files()) changed_by_id[c.node_id] = &c;

    stats = Stats{};

    for (auto [ent, ref, style] : registry.view<NodeRef, Style>().each()) {
        ++stats.nodes;
        registry.remove<Impacted>(ent);
        registry.remove<Changed>(ent);
        registry.remove<HubSeed>(ent);

        int distance = -1;

        // A file's own change is what makes it red; a package is red when a file it
        // owns changed. Projection happens here so the renderer never has to know.
        const ChangedFile* changed = nullptr;
        if (auto it = changed_by_id.find(ref.id); it != changed_by_id.end()) {
            changed = it->second;
        } else if (ref.kind == NodeKind::Package || ref.kind == NodeKind::Directory) {
            for (const auto& [nid, c] : changed_by_id) {
                if (store.ancestor_of_kind(nid, ref.kind) == ref.id) { changed = c; break; }
            }
        }
        if (changed) {
            registry.emplace<Changed>(ent, Changed{changed->change, changed->processing});
            distance = 0;
            ++stats.changed;

            // The agent touched something most of the repository depends on. This is
            // the one case the view is allowed to shout about.
            if (auto h = hub_by_id.find(ref.id); h != hub_by_id.end()) {
                registry.emplace<HubSeed>(ent, HubSeed{h->second->dependents,
                                                       h->second->population,
                                                       h->second->specificity,
                                                       h->second->reach_fraction});
            }
        }

        // The impact result carries its own freshness: it describes the trustworthiness
        // of the PATH that reached this node, which can be worse than anything the
        // node itself reports. Ignoring it would render a stale conclusion as current.
        Freshness impact_freshness = Freshness::Current;
        bool      muted            = false;
        if (auto it = impact_by_id.find(ref.id); it != impact_by_id.end()) {
            const ImpactedNode* in = it->second;
            if (in->min_distance <= view.filters.max_impact_depth) {
                const float rel = analysis::relevance(store, specificity_, *in);

                // A changed node is never muted, whatever it scores. Its own hub-ness
                // is precisely why it matters, and filtering it would hide the loudest
                // event the product can report.
                muted = !changed && in->min_distance > 0 && rel < view.filters.min_relevance;

                registry.emplace<Impacted>(
                    ent, Impacted{in->min_distance, in->direct, in->cause, rel, muted});
                if (distance < 0 || in->min_distance < distance) distance = in->min_distance;
                if (in->min_distance > 0) {
                    if (muted) ++stats.muted;
                    else ++stats.impacted;
                }
                impact_freshness = in->freshness;
            }
        }
        // Muted nodes fall back to context styling: still on screen, still true, just
        // no longer competing for attention.
        if (muted) distance = -1;

        const auto* fresh = registry.try_get<FreshnessState>(ent);
        const Freshness f =
            worse_of(fresh ? fresh->value : Freshness::Current, impact_freshness);
        if (f != Freshness::Current) ++stats.stale;

        style.emphasis = distance < 0 ? 0.0f : 1.0f - std::min(0.6f, distance * 0.14f);
        style.fill     = t.node_fill;
        style.stroke   = distance >= 0 ? ui::impact_color(distance) : t.node_stroke;
        style.stroke_w = distance == 0 ? 3.0f : (distance == 1 ? 2.4f : 1.6f);
        style.dash     = 0.0f;

        switch (f) {
            case Freshness::Stale:
                // Colour plus a dashed outline: two channels, because a single colour
                // cue is not enough to stop someone trusting a stale relationship.
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
        if (registry.all_of<Selected>(ent)) {
            style.stroke   = t.selection;
            style.stroke_w = 3.2f;
        }
    }

    for (auto [ent, ref, ends, style] : registry.view<EdgeRef, Endpoints, Style>().each()) {
        ++stats.edges;
        const auto* fa = registry.try_get<Impacted>(to_entity(ends.from));
        const auto* fb = registry.try_get<Impacted>(to_entity(ends.to));
        const bool  on_impact = fa && fb && !fa->muted && !fb->muted;

        style.stroke   = on_impact ? t.edge_impact : t.edge;
        style.stroke_w = on_impact ? 2.0f : 1.2f;
        style.dash     = 0.0f;
        style.emphasis = on_impact ? 0.9f : 0.35f;

        if (ref.kind == EdgeKind::Contains) {
            style.stroke   = t.edge;
            style.stroke_w = 1.0f;
            style.emphasis = 0.3f;
        }
        if (const auto* c = registry.try_get<ConfidenceState>(ent)) {
            if (c->value == Confidence::Heuristic || c->value == Confidence::Unresolved) {
                style.stroke = t.heuristic;
                style.dash   = 7.0f;
            }
        }
        if (const auto* f = registry.try_get<FreshnessState>(ent)) {
            if (f->value == Freshness::Stale) {
                style.stroke = t.stale;
                style.dash   = 9.0f;
                ++stats.stale;
            } else if (f->value == Freshness::Invalid) {
                style.stroke = t.invalid;
                style.dash   = 4.0f;
            }
        }
        if (registry.all_of<Selected>(ent)) {
            style.stroke   = t.selection;
            style.stroke_w = 3.0f;
        }
    }

    stats.visible_nodes = stats.nodes;
    stats.visible_edges = stats.edges;
    update_explained_path(store);
}

void Scene::update_explained_path(const GraphStore& store) {
    registry.clear<OnExplainedPath>();
    if (view.selected_node.empty()) return;

    const ImpactResult* r = store.impact(view.level);
    if (!r) return;

    const ImpactedNode* target = nullptr;
    for (const auto& in : r->impacted_nodes) {
        if (in.node_id == view.selected_node) { target = &in; break; }
    }
    if (!target || target->paths.empty()) return;

    const int   idx  = std::clamp(view.path_index, 0, static_cast<int>(target->paths.size()) - 1);
    const auto& path = target->paths[idx].edges;

    int hop = 0;
    if (auto it = node_index_.find(view.selected_node); it != node_index_.end()) {
        registry.emplace_or_replace<OnExplainedPath>(it->second, OnExplainedPath{hop});
    }
    for (const auto& eid : path) {
        ++hop;
        if (auto it = edge_index_.find(eid); it != edge_index_.end()) {
            registry.emplace_or_replace<OnExplainedPath>(it->second, OnExplainedPath{hop});
        }
        if (const Edge* e = store.edge(eid)) {
            if (auto it = node_index_.find(e->to); it != node_index_.end()) {
                registry.emplace_or_replace<OnExplainedPath>(it->second, OnExplainedPath{hop});
            }
        }
    }
}

entt::entity Scene::find_node(const NodeId& id) const {
    auto it = node_index_.find(id);
    return it == node_index_.end() ? entt::null : it->second;
}

entt::entity Scene::find_edge(const EdgeId& id) const {
    auto it = edge_index_.find(id);
    return it == edge_index_.end() ? entt::null : it->second;
}

entt::entity Scene::pick(Vec2 screen) const {
    const Vec2       w      = view.camera.screen_to_world(screen);
    const NodeDetail detail = node_detail(view);
    const float      zoom   = std::max(view.camera.zoom, 1e-4f);

    entt::entity best      = entt::null;
    float        best_area = 0.0f;
    float        best_dist = 0.0f;

    for (auto [ent, pos, ext] : registry.view<const Position, const Extent>().each()) {
        if (registry.all_of<Hidden>(ent)) continue;

        const bool changed  = registry.all_of<Changed>(ent);
        const bool impacted = registry.all_of<Impacted>(ent);
        Vec2       half     = render_half(view, detail, ext.half,
                                          dot_px_for(changed, impacted, false));

        // A dot must stay clickable even when it is only a few pixels across, so the
        // hit area has a screen-space floor. Without it, overview zoom becomes a test
        // of mouse precision.
        const float min_world = 7.0f / zoom;
        half.x = std::max(half.x, min_world);
        half.y = std::max(half.y, min_world);

        if (std::abs(w.x - pos.p.x) > half.x || std::abs(w.y - pos.p.y) > half.y) continue;

        // Smallest hit wins, so a small node inside a big cluster stays clickable.
        // Ties break on distance to centre, because once every node is collapsed to
        // the same minimum hit size their areas are identical and their hit areas
        // overlap -- without this, which node a click selects is arbitrary.
        const float area = half.x * half.y;
        const float dist = length_sq(w - pos.p);
        const float eps  = area * 1e-3f;
        if (best == entt::null || area < best_area - eps ||
            (std::abs(area - best_area) <= eps && dist < best_dist)) {
            best      = ent;
            best_area = area;
            best_dist = dist;
        }
    }
    return best;
}

void Scene::focus_on(const std::vector<NodeId>& ids, float padding) {
    bool  any = false;
    Vec2  lo{0, 0}, hi{0, 0};
    auto  add = [&](const Position& p, const Extent& e) {
        const Vec2 a = p.p - e.half, b = p.p + e.half;
        if (!any) { lo = a; hi = b; any = true; return; }
        lo.x = std::min(lo.x, a.x); lo.y = std::min(lo.y, a.y);
        hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y);
    };

    if (ids.empty()) {
        for (auto [ent, p, e] : registry.view<const Position, const Extent>().each()) add(p, e);
    } else {
        for (const auto& id : ids) {
            auto it = node_index_.find(id);
            if (it == node_index_.end()) continue;
            const auto* p = registry.try_get<Position>(it->second);
            const auto* e = registry.try_get<Extent>(it->second);
            if (p && e) add(*p, *e);
        }
    }
    if (!any) return;

    view.camera.center = (lo + hi) * 0.5f;
    // Fit into the area not covered by panels, not the whole framebuffer, or the
    // graph ends up half-hidden behind the inspector.
    const Vec2  fit = view.free_size.x > 1.0f ? view.free_size
                                              : Vec2{view.camera.vw, view.camera.vh};
    const float w   = std::max(1.0f, hi.x - lo.x) + padding * 2.0f;
    const float h   = std::max(1.0f, hi.y - lo.y) + padding * 2.0f;
    view.camera.zoom = std::clamp(std::min(fit.x / w, fit.y / h), 0.02f, 3.0f);
}

} // namespace rgv::ecs
