// Framing the graph.
//
// Fits into the rectangle the panels leave, not the whole framebuffer -- otherwise the
// graph ends up half-hidden behind the inspector.
#pragma once

#include "rgv/contract/Types.h"
#include "rgv/ecs/World.h"

#include <vector>

namespace rgv::view {

// Frames the given nodes. An empty list frames everything visible.
void fit_camera(ecs::World& world, const std::vector<NodeId>& ids, float padding = 80.0f);

} // namespace rgv::view
