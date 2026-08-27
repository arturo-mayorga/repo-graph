// rgv-replay: headless driver for the frontend contract.
//
// Same GraphStore and same FixtureSource the GL app uses, with the renderer removed.
// Two jobs:
//   * `--check` validates fixtures (dangling edges, unresolvable impact paths, and
//     authored impact results that disagree with the traversal the frontend would do).
//     Suitable for CI -- it needs no display.
//   * default mode prints the state after replay, so a scenario can be inspected
//     without opening a window.
#include "rgv/analysis/Specificity.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/sim/ImpactSim.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

using namespace rgv;

namespace {

struct Options {
    std::string fixture_dir;
    int         scenario = -1;   // -1 => all
    bool        check    = false;
    bool        verbose  = false;
    bool        log      = false;
};

void usage() {
    std::cout <<
        "usage: rgv-replay <fixture-dir> [options]\n"
        "  --scenario N   replay only scenario N (default: all)\n"
        "  --check        validate fixtures and exit non-zero on any problem\n"
        "  --log          print the event log\n"
        "  -v, --verbose  print per-node impact detail\n";
}

// Drives the source to completion without a frame loop. Uses a large fixed dt so a
// multi-second scenario finishes in a handful of iterations.
void replay(fixture::FixtureSource& src, GraphStore& store) {
    if (src.take_reset()) store.reset(src.baseline());
    for (int guard = 0; guard < 100000; ++guard) {
        src.poll(0.25, store);
        if (src.status().ended) break;
    }
}

const char* mark(bool ok) { return ok ? "  ok  " : " FAIL "; }

int check_scenario(const fixture::FixtureSet& set, std::size_t i, bool verbose) {
    fixture::FixtureSource src(set, i);
    GraphStore             store;
    replay(src, store);

    const auto& scn = src.scenario();
    int         problems = 0;

    std::cout << "\n[" << i << "] " << scn.name << "  (" << scn.events.size()
              << " events, " << std::fixed << std::setprecision(0)
              << scn.duration_ms() << " ms)\n";

    // 1. every edge endpoint resolves
    {
        std::vector<std::string> dangling;
        for (const auto& [id, e] : store.edges()) {
            if (!store.node(e.from) || !store.node(e.to)) dangling.push_back(id);
        }
        std::sort(dangling.begin(), dangling.end());
        std::cout << mark(dangling.empty()) << "edge endpoints resolve";
        if (!dangling.empty()) {
            ++problems;
            std::cout << " (" << dangling.size() << " dangling)";
            for (std::size_t k = 0; k < std::min<std::size_t>(5, dangling.size()); ++k) {
                std::cout << "\n         " << dangling[k];
            }
        }
        std::cout << "\n";
    }

    // 2. every containment parent resolves, and the tree has no cycle
    {
        int bad_parent = 0, cyclic = 0;
        for (const auto& [id, n] : store.nodes()) {
            if (n.parent.empty()) continue;
            if (!store.node(n.parent)) { ++bad_parent; continue; }
            NodeId cur   = n.parent;
            int    guard = 0;
            while (!cur.empty() && guard++ < 64) {
                if (cur == id) { ++cyclic; break; }
                const Node* p = store.node(cur);
                cur = p ? p->parent : NodeId{};
            }
        }
        std::cout << mark(bad_parent == 0 && cyclic == 0) << "containment tree is sound";
        if (bad_parent || cyclic) {
            ++problems;
            std::cout << " (" << bad_parent << " missing parent, " << cyclic << " cyclic)";
        }
        std::cout << "\n";
    }

    // 3. every impact path resolves to real edges and actually reaches a seed
    {
        int bad_edge = 0, disconnected = 0, no_path = 0;
        for (const auto& level : {Level::Package, Level::BuildTarget, Level::File, Level::Symbol}) {
            const ImpactResult* r = store.impact(level);
            if (!r) continue;
            std::set<NodeId> seeds(r->seed_nodes.begin(), r->seed_nodes.end());
            for (const auto& in : r->impacted_nodes) {
                if (in.min_distance > 0 && in.paths.empty()) { ++no_path; continue; }
                for (const auto& p : in.paths) {
                    NodeId cur = in.node_id;
                    for (const auto& eid : p.edges) {
                        const Edge* e = store.edge(eid);
                        if (!e) { ++bad_edge; cur.clear(); break; }
                        // Paths run impacted-node-first: each hop leaves the node we
                        // are standing on via its dependent -> dependency direction.
                        if (e->from != cur) { ++disconnected; cur.clear(); break; }
                        cur = e->to;
                    }
                    if (!cur.empty() && !seeds.count(cur)) ++disconnected;
                }
            }
        }
        const bool ok = !bad_edge && !disconnected && !no_path;
        std::cout << mark(ok) << "impact paths are explainable (FR-26)";
        if (!ok) {
            ++problems;
            std::cout << " (" << bad_edge << " unknown edge, " << disconnected
                      << " broken chain, " << no_path << " impacted with no path)";
        }
        std::cout << "\n";
    }

    // 4. authored impact agrees with the traversal the frontend would perform
    for (const auto& level : {Level::Package, Level::File}) {
        const ImpactResult* authored = store.impact(level);
        if (!authored) continue;
        const auto  simulated = sim::compute(store, authored->seed_nodes, level, authored->filters);

        std::set<NodeId> a, b;
        for (const auto& n : authored->impacted_nodes) a.insert(n.node_id);
        for (const auto& n : simulated.impacted_nodes) b.insert(n.node_id);

        std::vector<NodeId> only_authored, only_sim;
        std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(only_authored));
        std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(only_sim));

        std::map<NodeId, int> ad;
        for (const auto& n : authored->impacted_nodes) ad[n.node_id] = n.min_distance;
        int dist_mismatch = 0;
        for (const auto& n : simulated.impacted_nodes) {
            auto it = ad.find(n.node_id);
            if (it != ad.end() && it->second != n.min_distance) ++dist_mismatch;
        }

        const bool ok = only_authored.empty() && only_sim.empty() && dist_mismatch == 0;
        std::cout << mark(ok) << "authored " << to_string(level)
                  << " impact matches traversal (" << a.size() << " nodes)";
        if (!ok) {
            ++problems;
            std::cout << "\n         authored-only: " << only_authored.size()
                      << "  traversal-only: " << only_sim.size()
                      << "  distance mismatch: " << dist_mismatch;
            for (const auto& n : only_authored) std::cout << "\n         only in fixture: " << n;
            for (const auto& n : only_sim) std::cout << "\n         only in traversal: " << n;
        }
        std::cout << "\n";
    }

    // 5. store-level warnings raised while applying events
    {
        std::cout << mark(store.warnings().empty()) << "no store warnings";
        if (!store.warnings().empty()) {
            ++problems;
            std::cout << " (" << store.warnings().size() << ")";
            for (std::size_t k = 0; k < std::min<std::size_t>(5, store.warnings().size()); ++k) {
                std::cout << "\n         " << store.warnings()[k];
            }
        }
        std::cout << "\n";
    }

    if (verbose) {
        for (const auto& level : {Level::Package, Level::File}) {
            const ImpactResult* r = store.impact(level);
            if (!r) continue;
            std::cout << "       " << to_string(level) << " impact from ";
            for (const auto& s : r->seed_nodes) std::cout << s << " ";
            std::cout << "\n";
            for (const auto& n : r->impacted_nodes) {
                std::cout << "         d=" << n.min_distance << " " << std::setw(28)
                          << std::left << n.node_id << " " << to_string(n.cause)
                          << " " << to_string(n.freshness);
                if (!n.paths.empty()) {
                    std::cout << "  via";
                    for (const auto& e : n.paths[0].edges) std::cout << " " << e;
                }
                std::cout << "\n";
            }
        }
    }
    return problems;
}

void describe(const fixture::FixtureSet& set, std::size_t i, bool show_log) {
    fixture::FixtureSource src(set, i);
    GraphStore             store;
    replay(src, store);

    std::map<NodeKind, int> kinds;
    for (const auto& [id, n] : store.nodes()) ++kinds[n.kind];
    std::map<EdgeKind, int> ekinds;
    std::map<Freshness, int> efresh;
    for (const auto& [id, e] : store.edges()) { ++ekinds[e.kind]; ++efresh[e.freshness]; }

    std::cout << "\n=== [" << i << "] " << src.scenario().name << " ===\n"
              << src.scenario().description << "\n"
              << "generation " << store.generation()
              << " (baseline " << store.baseline().session.baseline_generation << ")\n";

    std::cout << "nodes:";
    for (const auto& [k, c] : kinds) std::cout << " " << to_string(k) << "=" << c;
    std::cout << "\nedges:";
    for (const auto& [k, c] : ekinds) std::cout << " " << to_string(k) << "=" << c;
    std::cout << "\nfreshness:";
    for (const auto& [k, c] : efresh) std::cout << " " << to_string(k) << "=" << c;
    std::cout << "\n";

    std::cout << "changed files (" << store.changed_files().size() << "):\n";
    for (const auto& c : store.changed_files()) {
        std::cout << "  " << std::setw(9) << std::left << to_string(c.change)
                  << std::setw(11) << std::left << to_string(c.processing)
                  << c.path << "\n";
    }
    std::cout << "adapters:\n";
    for (const auto& a : store.adapters()) {
        std::cout << "  " << std::setw(18) << std::left << a.name
                  << std::setw(10) << std::left << to_string(a.state)
                  << "queue=" << a.queue_depth << "  " << a.message << "\n";
    }
    for (const auto& level : {Level::Package, Level::File}) {
        const ImpactResult* r = store.impact(level);
        if (!r) continue;

        // Architectural specificity, so the CLI can answer "which of these results is
        // actually worth reading" and not just "how many are there".
        const auto index  = analysis::build(store, level, r->filters);
        const auto alerts = analysis::hub_seeds(store, index, *r, analysis::kHubThreshold);
        for (const auto& a : alerts) {
            std::cout << "HUB CHANGE  " << a.node << ": " << a.dependents << " of "
                      << a.population << " depend on it, blast radius " << a.reach << " ("
                      << static_cast<int>(a.reach_fraction * 100.0f) << "%)\n";
        }

        int informative = 0;
        for (const auto& n : r->impacted_nodes) {
            if (n.min_distance > 0 &&
                analysis::relevance(store, index, n) >= analysis::kHubThreshold) {
                ++informative;
            }
        }
        std::cout << to_string(level) << " impact: " << r->impacted_nodes.size()
                  << " node(s) from " << r->seed_nodes.size() << " seed(s); "
                  << informative << " above relevance " << analysis::kHubThreshold << "\n";

        for (const auto& n : r->impacted_nodes) {
            const float rel = analysis::relevance(store, index, n);
            std::cout << "  d=" << n.min_distance
                      << "  rel=" << std::fixed << std::setprecision(2) << rel
                      << "  spec=" << index.specificity(n.node_id) << "  "
                      << std::setw(26) << std::left << n.node_id << to_string(n.cause);
            if (n.freshness != Freshness::Current) std::cout << " [" << to_string(n.freshness) << "]";
            if (n.min_distance > 0 && rel < analysis::kHubThreshold) std::cout << "  (via hub)";
            std::cout << "\n";
        }
    }
    if (show_log) {
        std::cout << "event log:\n";
        for (const auto& l : store.log()) {
            std::cout << "  " << std::setw(7) << std::right << (long)l.t_ms << "ms g"
                      << l.generation << "  " << std::setw(20) << std::left
                      << to_string(l.type) << l.summary << "\n";
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--check") opt.check = true;
        else if (a == "--log") opt.log = true;
        else if (a == "-v" || a == "--verbose") opt.verbose = true;
        else if (a == "--scenario" && i + 1 < argc) opt.scenario = std::atoi(argv[++i]);
        else if (!a.empty() && a[0] == '-') { std::cerr << "unknown option " << a << "\n"; return 2; }
        else opt.fixture_dir = a;
    }
    if (opt.fixture_dir.empty()) { usage(); return 2; }

    fixture::FixtureSet set;
    try {
        set = fixture::FixtureSet::load(opt.fixture_dir);
    } catch (const std::exception& ex) {
        std::cerr << "load failed: " << ex.what() << "\n";
        return 1;
    }

    std::cout << set.name << "  (" << set.snapshot.nodes.size() << " nodes, "
              << set.snapshot.edges.size() << " edges, " << set.scenarios.size()
              << " scenarios)\n";

    const std::size_t first = opt.scenario < 0 ? 0 : static_cast<std::size_t>(opt.scenario);
    const std::size_t last  = opt.scenario < 0 ? set.scenarios.size() : first + 1;
    if (first >= set.scenarios.size()) { std::cerr << "no such scenario\n"; return 2; }

    int problems = 0;
    for (std::size_t i = first; i < last; ++i) {
        if (opt.check) problems += check_scenario(set, i, opt.verbose);
        else describe(set, i, opt.log);
    }
    if (opt.check) {
        std::cout << "\n" << (problems ? "FAILED: " + std::to_string(problems) + " problem(s)"
                                       : "all checks passed")
                  << "\n";
    }
    return problems ? 1 : 0;
}
