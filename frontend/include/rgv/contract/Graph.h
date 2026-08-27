// Node / Edge / Snapshot: the shape of graph data the frontend receives.
#pragma once

#include "rgv/contract/Types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rgv {

struct Node {
    NodeId      id;
    NodeKind    kind = NodeKind::Unknown;
    std::string name;                 // display label
    std::string path;                 // repo-relative, "" for synthetic nodes
    NodeId      parent;               // containment parent; "" for the root
    std::string language;
    Freshness   freshness = Freshness::Current;

    std::map<std::string, std::string> attrs;  // shown verbatim in the inspector
};

// Provenance for a single relationship (spec section 8.3). Optional because the live
// source may lazy-load it via GET /explain/edge/:id while fixtures inline it.
struct Evidence {
    std::string artifact;   // file / manifest / build definition it came from
    int         line = 0;   // 0 => unknown
    std::string snippet;
};

struct Edge {
    EdgeId      id;
    EdgeKind    kind = EdgeKind::Unknown;
    NodeId      from;                 // dependent
    NodeId      to;                   // dependency
    std::string provider;             // git|watchman|tree-sitter|scip|lsp:<n>|npm|...
    std::string provider_version;
    Confidence  confidence = Confidence::Exact;
    Freshness   freshness  = Freshness::Current;

    Generation                valid_from = 0;
    std::optional<Generation> valid_to;  // set => historical, kept for temporal compare

    std::optional<Evidence> evidence;

    bool active() const { return !valid_to.has_value(); }
};

struct RepoInfo {
    std::string root;
    std::string name;
    std::string head;     // commit SHA
    std::string branch;
    bool        detached = false;
};

struct SessionInfo {
    std::string id;
    std::string name;
    Generation  baseline_generation = 0;
    double      started_at_ms       = 0.0;
};

struct Snapshot {
    std::string       schema = "rgv.snapshot/1";
    RepoInfo          repo;
    SessionInfo       session;
    Generation        generation = 0;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

} // namespace rgv
