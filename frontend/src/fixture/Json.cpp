#include "rgv/fixture/Json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace rgv::fixture {
namespace {

using nlohmann::json;

[[noreturn]] void fail(const std::string& origin, const std::string& what) {
    throw ParseError(origin + ": " + what);
}

const json& require(const json& j, const char* key, const std::string& origin) {
    auto it = j.find(key);
    if (it == j.end()) fail(origin, std::string("missing required field '") + key + "'");
    return *it;
}

std::string str_or(const json& j, const char* key, const std::string& def = {}) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<std::string>();
}

int int_or(const json& j, const char* key, int def = 0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<int>();
}

double dbl_or(const json& j, const char* key, double def = 0.0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<double>();
}

bool bool_or(const json& j, const char* key, bool def = false) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<bool>();
}

Generation gen_or(const json& j, const char* key, Generation def = 0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<Generation>();
}

// Enum reads go through here so an unknown token is a hard error rather than a
// silent fallback to the first member.
template <class F>
auto enum_field(const json& j, const char* key, F parser, const std::string& origin,
                decltype(parser("", nullptr)) def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    bool ok = false;
    auto v  = parser(it->get<std::string>(), &ok);
    if (!ok) fail(origin, std::string("unknown value '") + it->get<std::string>() +
                              "' for field '" + key + "'");
    return v;
}

Node parse_node(const json& j, const std::string& origin) {
    Node n;
    n.id        = require(j, "id", origin).get<std::string>();
    n.kind      = enum_field(j, "kind", parse_node_kind, origin, NodeKind::Unknown);
    n.name      = str_or(j, "name", n.id);
    n.path      = str_or(j, "path");
    n.parent    = str_or(j, "parent");
    n.language  = str_or(j, "language");
    n.freshness = enum_field(j, "freshness", parse_freshness, origin, Freshness::Current);
    if (auto it = j.find("attrs"); it != j.end() && it->is_object()) {
        for (const auto& [k, v] : it->items()) {
            n.attrs[k] = v.is_string() ? v.get<std::string>() : v.dump();
        }
    }
    return n;
}

Edge parse_edge(const json& j, const std::string& origin) {
    Edge e;
    e.id               = require(j, "id", origin).get<std::string>();
    e.kind             = enum_field(j, "kind", parse_edge_kind, origin, EdgeKind::Unknown);
    e.from             = require(j, "from", origin).get<std::string>();
    e.to               = require(j, "to", origin).get<std::string>();
    e.provider         = str_or(j, "provider", "unknown");
    e.provider_version = str_or(j, "provider_version");
    e.confidence       = enum_field(j, "confidence", parse_confidence, origin, Confidence::Exact);
    e.freshness        = enum_field(j, "freshness", parse_freshness, origin, Freshness::Current);
    e.valid_from       = gen_or(j, "valid_from");
    if (auto it = j.find("valid_to"); it != j.end() && !it->is_null()) {
        e.valid_to = it->get<Generation>();
    }
    if (auto it = j.find("evidence"); it != j.end() && it->is_object()) {
        Evidence ev;
        ev.artifact = str_or(*it, "artifact");
        ev.line     = int_or(*it, "line");
        ev.snippet  = str_or(*it, "snippet");
        e.evidence  = ev;
    }
    return e;
}

ImpactResult parse_impact(const json& j, const std::string& origin) {
    ImpactResult r;
    r.level               = enum_field(j, "level", parse_level, origin, Level::Package);
    r.graph_generation    = gen_or(j, "generation");
    r.baseline_generation = gen_or(j, "baseline_generation");

    if (auto it = j.find("seed_nodes"); it != j.end()) {
        for (const auto& s : *it) r.seed_nodes.push_back(s.get<std::string>());
    }
    if (auto it = j.find("filters"); it != j.end() && it->is_object()) {
        r.filters.max_depth         = int_or(*it, "max_depth", 8);
        r.filters.include_heuristic = bool_or(*it, "include_heuristic", false);
        if (auto ek = it->find("edge_kinds"); ek != it->end()) {
            r.filters.edge_kinds.clear();
            for (const auto& k : *ek) {
                bool ok = false;
                auto v  = parse_edge_kind(k.get<std::string>(), &ok);
                if (!ok) fail(origin, "unknown edge kind '" + k.get<std::string>() + "' in filters");
                r.filters.edge_kinds.push_back(v);
            }
        }
    }
    for (const auto& n : require(j, "impacted_nodes", origin)) {
        ImpactedNode in;
        in.node_id      = require(n, "node_id", origin).get<std::string>();
        in.min_distance = int_or(n, "min_distance");
        in.direct       = bool_or(n, "direct");
        in.changed      = bool_or(n, "changed");
        in.freshness    = enum_field(n, "freshness", parse_freshness, origin, Freshness::Current);
        in.cause        = enum_field(n, "cause", parse_impact_cause, origin, ImpactCause::Implementation);
        in.paths_truncated = bool_or(n, "paths_truncated");
        if (auto it = n.find("paths"); it != n.end()) {
            for (const auto& p : *it) {
                ImpactPath path;
                for (const auto& eid : require(p, "edges", origin)) {
                    path.edges.push_back(eid.get<std::string>());
                }
                in.paths.push_back(std::move(path));
            }
        }
        r.impacted_nodes.push_back(std::move(in));
    }
    return r;
}

Event parse_event_json(const json& j, const std::string& origin) {
    Event e;
    e.t_ms       = dbl_or(j, "t_ms");
    e.generation = gen_or(j, "generation");

    const std::string type_str = require(j, "type", origin).get<std::string>();
    bool ok = false;
    e.type  = parse_event_type(type_str, &ok);
    if (!ok) fail(origin, "unknown event type '" + type_str + "'");

    switch (e.type) {
        case EventType::SessionStarted: {
            SessionStartedPayload p;
            const json& s          = require(j, "session", origin);
            p.session.id           = str_or(s, "id");
            p.session.name         = str_or(s, "name");
            p.session.baseline_generation = gen_or(s, "baseline_generation");
            p.session.started_at_ms       = dbl_or(s, "started_at_ms");
            e.payload = std::move(p);
            break;
        }
        case EventType::FileChanged: {
            FileChangedPayload p;
            p.path       = require(j, "path", origin).get<std::string>();
            p.node_id    = str_or(j, "node_id", "file:" + p.path);
            p.change     = enum_field(j, "change", parse_file_change, origin, FileChangeKind::Modified);
            p.from_path  = str_or(j, "from_path");
            p.processing = enum_field(j, "processing", parse_processing, origin, Processing::Pending);
            e.payload = std::move(p);
            break;
        }
        case EventType::GraphUpdated: {
            GraphUpdatedPayload p;
            auto nodes_into = [&](const char* key, std::vector<Node>& dst) {
                if (auto it = j.find(key); it != j.end()) {
                    for (const auto& n : *it) dst.push_back(parse_node(n, origin));
                }
            };
            auto edges_into = [&](const char* key, std::vector<Edge>& dst) {
                if (auto it = j.find(key); it != j.end()) {
                    for (const auto& x : *it) dst.push_back(parse_edge(x, origin));
                }
            };
            auto ids_into = [&](const char* key, std::vector<std::string>& dst) {
                if (auto it = j.find(key); it != j.end()) {
                    for (const auto& x : *it) dst.push_back(x.get<std::string>());
                }
            };
            nodes_into("added_nodes", p.added_nodes);
            nodes_into("updated_nodes", p.updated_nodes);
            edges_into("added_edges", p.added_edges);
            edges_into("updated_edges", p.updated_edges);
            ids_into("removed_nodes", p.removed_nodes);
            ids_into("removed_edges", p.removed_edges);
            p.note    = str_or(j, "note");
            e.payload = std::move(p);
            break;
        }
        case EventType::ImpactUpdated: {
            auto r = parse_impact(j, origin);
            if (r.graph_generation == 0) r.graph_generation = e.generation;
            e.payload = std::move(r);
            break;
        }
        case EventType::AdapterStatus: {
            AdapterStatusPayload p;
            p.adapter                 = require(j, "adapter", origin).get<std::string>();
            p.state                   = enum_field(j, "state", parse_adapter_state, origin, AdapterState::Idle);
            p.queue_depth             = int_or(j, "queue_depth");
            p.last_success_generation = gen_or(j, "last_success_generation");
            p.message                 = str_or(j, "message");
            e.payload = std::move(p);
            break;
        }
        case EventType::ReconcileCheckpoint: {
            ReconcileCheckpointPayload p;
            p.corrections = int_or(j, "corrections");
            p.message     = str_or(j, "message");
            e.payload = std::move(p);
            break;
        }
    }
    return e;
}

} // namespace

Snapshot parse_snapshot_json(const json& j, const std::string& origin);

LiveMessage parse_live_line(const std::string& line, const std::string& origin) {
    json j = json::parse(line);
    if (!j.is_object()) throw ParseError(origin + ": line is not a JSON object");

    LiveMessage msg;
    if (str_or(j, "type") == "snapshot") {
        msg.is_snapshot = true;
        msg.snapshot    = parse_snapshot_json(require(j, "snapshot", origin), origin);
        return msg;
    }
    msg.event = parse_event_json(j, origin);
    return msg;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ParseError(path + ": cannot open");
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

Snapshot parse_snapshot_json(const json& j, const std::string& origin) {
    Snapshot s;
    s.schema = str_or(j, "schema", "rgv.snapshot/1");
    if (s.schema != "rgv.snapshot/1") {
        fail(origin, "unsupported schema '" + s.schema + "', expected rgv.snapshot/1");
    }
    if (auto it = j.find("repo"); it != j.end()) {
        s.repo.root     = str_or(*it, "root");
        s.repo.name     = str_or(*it, "name");
        s.repo.head     = str_or(*it, "head");
        s.repo.branch   = str_or(*it, "branch");
        s.repo.detached = bool_or(*it, "detached");
    }
    if (auto it = j.find("session"); it != j.end()) {
        s.session.id                  = str_or(*it, "id");
        s.session.name                = str_or(*it, "name");
        s.session.baseline_generation = gen_or(*it, "baseline_generation");
        s.session.started_at_ms       = dbl_or(*it, "started_at_ms");
    }
    s.generation = gen_or(j, "generation", s.session.baseline_generation);

    for (const auto& n : require(j, "nodes", origin)) s.nodes.push_back(parse_node(n, origin));
    if (auto it = j.find("edges"); it != j.end()) {
        for (const auto& x : *it) s.edges.push_back(parse_edge(x, origin));
    }
    return s;
}

Snapshot parse_snapshot(const std::string& text, const std::string& origin) {
    json j;
    try {
        j = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const json::exception& ex) {
        fail(origin, ex.what());
    }
    return parse_snapshot_json(j, origin);
}

Snapshot load_snapshot(const std::string& path) {
    return parse_snapshot(read_file(path), path);
}

std::vector<Event> parse_scenario(const std::string& text, const std::string& origin) {
    std::vector<Event> out;
    std::istringstream in(text);
    std::string        line;
    int                lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;
        auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        if (line.compare(first, 2, "//") == 0) continue;

        const std::string where = origin + ":" + std::to_string(lineno);
        json              j;
        try {
            j = json::parse(line, nullptr, true, /*ignore_comments=*/true);
        } catch (const json::exception& ex) {
            fail(where, ex.what());
        }
        out.push_back(parse_event_json(j, where));
    }

    // A scenario is a timeline, so out-of-order authoring would replay incorrectly.
    // Stable sort keeps same-timestamp events in authored order, which is how a
    // burst (file.changed then graph.updated at the same instant) stays readable.
    std::stable_sort(out.begin(), out.end(),
                     [](const Event& a, const Event& b) { return a.t_ms < b.t_ms; });
    return out;
}

std::vector<Event> load_scenario(const std::string& path) {
    return parse_scenario(read_file(path), path);
}

} // namespace rgv::fixture
