#include "rgv/contract/Types.h"
#include "rgv/contract/Event.h"

#include <array>
#include <utility>

namespace rgv {
namespace {

// One table per enum keeps parse and format in sync by construction: adding a member
// without adding its token fails to compile the table's size assertion.
template <class E, std::size_t N>
E parse_from(const std::array<std::pair<std::string_view, E>, N>& table,
             std::string_view s, bool* ok, E fallback) {
    for (const auto& [token, value] : table) {
        if (token == s) { if (ok) *ok = true; return value; }
    }
    if (ok) *ok = false;
    return fallback;
}

template <class E, std::size_t N>
std::string_view format_from(const std::array<std::pair<std::string_view, E>, N>& table,
                             E value, std::string_view fallback) {
    for (const auto& [token, v] : table) {
        if (v == value) return token;
    }
    return fallback;
}

constexpr std::array<std::pair<std::string_view, NodeKind>, 9> kNodeKinds{{
    {"repository", NodeKind::Repository},
    {"workspace", NodeKind::Workspace},
    {"package", NodeKind::Package},
    {"build_target", NodeKind::BuildTarget},
    {"directory", NodeKind::Directory},
    {"file", NodeKind::File},
    {"symbol", NodeKind::Symbol},
    {"external_package", NodeKind::ExternalPackage},
    {"agent_session", NodeKind::AgentSession},
}};

constexpr std::array<std::pair<std::string_view, EdgeKind>, 9> kEdgeKinds{{
    {"contains", EdgeKind::Contains},
    {"owns", EdgeKind::Owns},
    {"depends_on", EdgeKind::DependsOn},
    {"imports", EdgeKind::Imports},
    {"defines", EdgeKind::Defines},
    {"references", EdgeKind::References},
    {"calls", EdgeKind::Calls},
    {"inherits", EdgeKind::Inherits},
    {"generated_from", EdgeKind::GeneratedFrom},
}};

constexpr std::array<std::pair<std::string_view, Freshness>, 4> kFreshness{{
    {"current", Freshness::Current},
    {"stale", Freshness::Stale},
    {"pending", Freshness::Pending},
    {"invalid", Freshness::Invalid},
}};

constexpr std::array<std::pair<std::string_view, Confidence>, 4> kConfidence{{
    {"exact", Confidence::Exact},
    {"high", Confidence::High},
    {"heuristic", Confidence::Heuristic},
    {"unresolved", Confidence::Unresolved},
}};

constexpr std::array<std::pair<std::string_view, Level>, 4> kLevels{{
    {"package", Level::Package},
    {"build_target", Level::BuildTarget},
    {"file", Level::File},
    {"symbol", Level::Symbol},
}};

constexpr std::array<std::pair<std::string_view, FileChangeKind>, 4> kFileChanges{{
    {"created", FileChangeKind::Created},
    {"modified", FileChangeKind::Modified},
    {"deleted", FileChangeKind::Deleted},
    {"renamed", FileChangeKind::Renamed},
}};

constexpr std::array<std::pair<std::string_view, Processing>, 4> kProcessing{{
    {"pending", Processing::Pending},
    {"structural", Processing::Structural},
    {"semantic", Processing::Semantic},
    {"settled", Processing::Settled},
}};

constexpr std::array<std::pair<std::string_view, AdapterState>, 4> kAdapterStates{{
    {"idle", AdapterState::Idle},
    {"running", AdapterState::Running},
    {"degraded", AdapterState::Degraded},
    {"failed", AdapterState::Failed},
}};

constexpr std::array<std::pair<std::string_view, ImpactCause>, 3> kImpactCauses{{
    {"implementation", ImpactCause::Implementation},
    {"dependency_added", ImpactCause::DependencyAdded},
    {"dependency_removed", ImpactCause::DependencyRemoved},
}};

constexpr std::array<std::pair<std::string_view, EventType>, 6> kEventTypes{{
    {"session.started", EventType::SessionStarted},
    {"file.changed", EventType::FileChanged},
    {"graph.updated", EventType::GraphUpdated},
    {"impact.updated", EventType::ImpactUpdated},
    {"adapter.status", EventType::AdapterStatus},
    {"reconcile.checkpoint", EventType::ReconcileCheckpoint},
}};

} // namespace

bool is_dependency_edge(EdgeKind k) {
    switch (k) {
        case EdgeKind::DependsOn:
        case EdgeKind::Imports:
        case EdgeKind::References:
        case EdgeKind::Calls:
        case EdgeKind::Inherits:
        case EdgeKind::GeneratedFrom:
            return true;
        default:
            return false;
    }
}

NodeKind parse_node_kind(std::string_view s, bool* ok) {
    return parse_from(kNodeKinds, s, ok, NodeKind::Unknown);
}
EdgeKind parse_edge_kind(std::string_view s, bool* ok) {
    return parse_from(kEdgeKinds, s, ok, EdgeKind::Unknown);
}
Freshness parse_freshness(std::string_view s, bool* ok) {
    return parse_from(kFreshness, s, ok, Freshness::Current);
}
Confidence parse_confidence(std::string_view s, bool* ok) {
    return parse_from(kConfidence, s, ok, Confidence::Exact);
}
Level parse_level(std::string_view s, bool* ok) {
    return parse_from(kLevels, s, ok, Level::Package);
}
FileChangeKind parse_file_change(std::string_view s, bool* ok) {
    return parse_from(kFileChanges, s, ok, FileChangeKind::Modified);
}
Processing parse_processing(std::string_view s, bool* ok) {
    return parse_from(kProcessing, s, ok, Processing::Pending);
}
AdapterState parse_adapter_state(std::string_view s, bool* ok) {
    return parse_from(kAdapterStates, s, ok, AdapterState::Idle);
}
ImpactCause parse_impact_cause(std::string_view s, bool* ok) {
    return parse_from(kImpactCauses, s, ok, ImpactCause::Implementation);
}
EventType parse_event_type(std::string_view s, bool* ok) {
    return parse_from(kEventTypes, s, ok, EventType::FileChanged);
}

std::string_view to_string(NodeKind k)       { return format_from(kNodeKinds, k, "unknown"); }
std::string_view to_string(EdgeKind k)       { return format_from(kEdgeKinds, k, "unknown"); }
std::string_view to_string(Freshness f)      { return format_from(kFreshness, f, "current"); }
std::string_view to_string(Confidence c)     { return format_from(kConfidence, c, "exact"); }
std::string_view to_string(Level l)          { return format_from(kLevels, l, "package"); }
std::string_view to_string(FileChangeKind c) { return format_from(kFileChanges, c, "modified"); }
std::string_view to_string(Processing p)     { return format_from(kProcessing, p, "pending"); }
std::string_view to_string(AdapterState s)   { return format_from(kAdapterStates, s, "idle"); }
std::string_view to_string(ImpactCause c)    { return format_from(kImpactCauses, c, "implementation"); }
std::string_view to_string(EventType t)      { return format_from(kEventTypes, t, "file.changed"); }

} // namespace rgv
