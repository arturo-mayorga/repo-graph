// Commands: things one system asks another to do.
//
// A panel cannot select a node itself -- it says SelectNode and moves on. One system
// applies it, so there is exactly one place where selection state changes and exactly
// one order in which it happens. That is what keeps the Selected component and the
// selected id from drifting apart, which is precisely how they drifted before.
#pragma once

#include "rgv/contract/Types.h"

#include <string>
#include <variant>
#include <vector>

namespace rgv::ecs {

struct SelectNode      { NodeId id; };
struct SelectEdge      { EdgeId id; };
struct ClearSelection  {};
struct CyclePath       { int delta = 1; };   // next/previous explanation

struct FitView         {};
struct FocusNodes      { std::vector<NodeId> ids; };

struct SetViewMode     { int mode = 0; };    // ecs::ViewMode
struct SetImpactLevel  { int level = 0; };   // rgv::Level

struct ReloadFixture   {};
struct SelectFixture   { int index = 0; };
struct SelectScenario  { int index = 0; };

struct SaveSettings    {};

using Command = std::variant<SelectNode, SelectEdge, ClearSelection, CyclePath, FitView,
                             FocusNodes, SetViewMode, SetImpactLevel, ReloadFixture,
                             SelectFixture, SelectScenario, SaveSettings>;

// Drained once per frame, in the order they were pushed.
struct CommandQueue {
    std::vector<Command> pending;

    template <class T> void push(T&& c) { pending.push_back(Command{std::forward<T>(c)}); }

    void clear() { pending.clear(); }
    bool empty() const { return pending.empty(); }
};

} // namespace rgv::ecs
