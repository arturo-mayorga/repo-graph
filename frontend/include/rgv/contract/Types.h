// Core vocabulary of the frontend data contract. See docs/frontend-contract.md.
//
// Identifiers are opaque strings. The frontend treats them as keys and never parses
// them; the readable "kind:path" convention exists only so fixtures stay hand-editable.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rgv {

using NodeId     = std::string;
using EdgeId     = std::string;
using Generation = std::uint64_t;

enum class NodeKind {
    Repository,
    Workspace,
    Package,
    BuildTarget,
    Directory,
    File,
    Symbol,
    ExternalPackage,
    AgentSession,
    Unknown,
};

enum class EdgeKind {
    Contains,
    Owns,
    DependsOn,
    Imports,
    Defines,
    References,
    Calls,
    Inherits,
    GeneratedFrom,
    Unknown,
};

// Whether an edge participates in blast-radius traversal at all. Structural edges
// (contains/owns/defines) express hierarchy, not dependency, and must never be
// traversed as impact (spec section 10.2).
bool is_dependency_edge(EdgeKind k);

enum class Freshness {
    Current,  // reflects the working tree as of `generation`
    Stale,    // last-known-good; not yet reconfirmed. Never render as current.
    Pending,  // work queued, no trustworthy value yet
    Invalid,  // provider failed on this artifact
};

enum class Confidence {
    Exact,
    High,
    Heuristic,
    Unresolved,
};

// Abstraction level an impact result was computed at.
enum class Level {
    Package,
    BuildTarget,
    File,
    Symbol,
};

enum class FileChangeKind {
    Created,
    Modified,
    Deleted,
    Renamed,
};

// Per-file pipeline position, mirroring the spec's T0..T3 tiers. Drives the
// in-flight affordance next to a changed file.
enum class Processing {
    Pending,     // T0: change observed, nothing computed yet
    Structural,  // T1: fast parse / package ownership done
    Semantic,    // T2: semantic enrichment done
    Settled,     // nothing outstanding
};

enum class AdapterState {
    Idle,
    Running,
    Degraded,
    Failed,
};

// Why a node is in the blast radius (FR-29): a changed implementation is a different
// product event from a rewired dependency.
enum class ImpactCause {
    Implementation,
    DependencyAdded,
    DependencyRemoved,
};

// -- string conversion -------------------------------------------------------
// parse_* return the `Unknown`/first member and set `ok=false` on an unrecognized
// token so a malformed fixture surfaces as a diagnostic instead of a silent default.

NodeKind       parse_node_kind(std::string_view s, bool* ok = nullptr);
EdgeKind       parse_edge_kind(std::string_view s, bool* ok = nullptr);
Freshness      parse_freshness(std::string_view s, bool* ok = nullptr);
Confidence     parse_confidence(std::string_view s, bool* ok = nullptr);
Level          parse_level(std::string_view s, bool* ok = nullptr);
FileChangeKind parse_file_change(std::string_view s, bool* ok = nullptr);
Processing     parse_processing(std::string_view s, bool* ok = nullptr);
AdapterState   parse_adapter_state(std::string_view s, bool* ok = nullptr);
ImpactCause    parse_impact_cause(std::string_view s, bool* ok = nullptr);

std::string_view to_string(NodeKind k);
std::string_view to_string(EdgeKind k);
std::string_view to_string(Freshness f);
std::string_view to_string(Confidence c);
std::string_view to_string(Level l);
std::string_view to_string(FileChangeKind c);
std::string_view to_string(Processing p);
std::string_view to_string(AdapterState s);
std::string_view to_string(ImpactCause c);

} // namespace rgv
