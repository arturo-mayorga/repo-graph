// The world: entities, and the singleton state systems share.
//
// Two kinds of state live here, and the distinction is the whole point of the design:
//
//   * Components  -- per-entity, iterated by systems in bulk. Stored in the registry.
//   * Resources   -- exactly one of them exists (the camera, the filters, the graph
//                    store). Stored in the registry's context.
//
// Anything that would otherwise become a global, a singleton, or a "manager" belongs
// in the second category. Making resources first-class is what lets a system declare
// what it touches instead of reaching through an owner object for it.
#pragma once

#include <entt/entt.hpp>

#include <utility>

namespace rgv::ecs {

class World {
public:
    entt::registry registry;

    // -- resources -----------------------------------------------------------

    template <class T, class... Args>
    T& add_resource(Args&&... args) {
        return registry.ctx().emplace<T>(std::forward<Args>(args)...);
    }

    // Throws if absent. A system asking for a resource it was not given is a wiring
    // bug, and it should fail at startup rather than silently do nothing.
    template <class T> T&       resource() { return registry.ctx().get<T>(); }
    template <class T> const T& resource() const { return registry.ctx().get<T>(); }

    // For genuinely optional collaborators. Returns nullptr when absent.
    template <class T> T*       find_resource() { return registry.ctx().find<T>(); }
    template <class T> const T* find_resource() const { return registry.ctx().find<T>(); }

    template <class T> bool has_resource() const { return registry.ctx().contains<T>(); }
};

} // namespace rgv::ecs
