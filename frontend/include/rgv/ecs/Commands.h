// Commands: things one system asks another to do.
//
// A panel cannot select a node itself -- it says SelectNode and moves on. One system
// applies it, so there is exactly one place where selection state changes and exactly
// one order in which it happens. That is what keeps the Selected component and the
// selected id from drifting apart, which is precisely how they drifted before.
//
// That used to be a comment, and the comment was broken at roughly thirty sites. It is
// now a type: `ui::Ui` holds every resource it touches by const reference except this
// queue, so a panel that writes state fails to compile. The cost is that every control
// on screen needs a command here, and this file is longer for it. The trade is
// deliberate -- a list of verbs that is tedious to extend is worth more than a rule
// that is free to break.
#pragma once

#include "rgv/contract/Types.h"

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace rgv::ecs {

struct SelectNode         { NodeId id; };
struct SelectEdge         { EdgeId id; };
struct ClearSelection     {};
// Back to the node the edge explains. Distinct from ClearSelection because the node is
// the context the user is still reading; dropping it too would close the inspector.
struct ClearEdgeSelection {};
struct CyclePath          { int delta = 1; };   // next/previous explanation

struct FitView         {};
struct FocusNodes      { std::vector<NodeId> ids; };
// Lay the whole graph out again. Every node moves, so it is only ever asked for.
struct Relayout        {};

struct SetViewMode     { int mode = 0; };    // ecs::ViewMode
struct SetImpactLevel  { int level = 0; };   // rgv::Level

// The boolean view switches, as one command rather than five. They differ only in
// which field they land on, and five near-identical variants would be five near-
// identical branches in CommandSystem for no added clarity.
enum class ViewToggle { LayoutRunning, ShowLabels, ShowArrows, ShowPanels, ShowTextSettings };
struct SetViewToggle   { ViewToggle which = ViewToggle::ShowPanels; bool on = true; };

// Clamped and applied by CommandSystem; persisted separately by SaveSettings, because
// a drag should reach the screen on every step and the disk only when it ends.
enum class TextScale { Ui, Graph };
struct SetTextScale    { TextScale which = TextScale::Ui; float value = 1.0f; };

// -- filters (FR-35). Each one re-tests visibility; none of them rebuilds the scene.
enum class FilterFlag { ShowUnaffected, ShowStale, ShowHeuristic, ShowExternal };
struct SetFilterFlag     { FilterFlag which = FilterFlag::ShowUnaffected; bool on = true; };
struct SetImpactDepth    { int depth = 8; };
struct SetMinRelevance   { float value = 0.0f; };
struct SetFilterText     { std::string text; };
struct AddHidePattern    { std::string source; };
struct RemoveHidePattern { int index = 0; };

// Hand a file to whatever the desktop opens files with, and hold a node where it is.
struct OpenNode        { NodeId id; };
struct TogglePin       { NodeId id; };

struct ReloadFixture   {};
struct SelectFixture   { int index = 0; };
struct SelectScenario  { int index = 0; };

struct SaveSettings    {};

// -- transport.
//
// Drained by TransportSystem, not by CommandSystem. The timeline is that system's to
// move -- it already owns the keyboard route -- and having the panel's buttons arrive
// by the same road is the difference between one owner and two. TransportSystem runs
// in Input, ahead of Sync, so it takes exactly these off the queue and leaves the rest
// for CommandSystem: a partition, not a shared drain.
struct TransportPlayPause {};
struct TransportStep      {};
struct TransportRestart   {};
struct TransportSeek      { double ms = 0.0; };
struct TransportRate      { double rate = 1.0; };

using Command =
    std::variant<SelectNode, SelectEdge, ClearSelection, ClearEdgeSelection, CyclePath,
                 FitView, FocusNodes, Relayout, SetViewMode, SetImpactLevel, SetViewToggle,
                 SetTextScale, SetFilterFlag, SetImpactDepth, SetMinRelevance, SetFilterText,
                 AddHidePattern, RemoveHidePattern, OpenNode, TogglePin, ReloadFixture,
                 SelectFixture, SelectScenario, SaveSettings, TransportPlayPause,
                 TransportStep, TransportRestart, TransportSeek, TransportRate>;

template <class T>
inline constexpr bool is_transport_command_v =
    std::is_same_v<T, TransportPlayPause> || std::is_same_v<T, TransportStep> ||
    std::is_same_v<T, TransportRestart> || std::is_same_v<T, TransportSeek> ||
    std::is_same_v<T, TransportRate>;

// Drained once per frame, in the order they were pushed.
struct CommandQueue {
    std::vector<Command> pending;

    template <class T> void push(T&& c) { pending.push_back(Command{std::forward<T>(c)}); }

    void clear() { pending.clear(); }
    bool empty() const { return pending.empty(); }
};

} // namespace rgv::ecs
