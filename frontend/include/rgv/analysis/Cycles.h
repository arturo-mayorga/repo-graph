// Cycles: which modules are mutually entangled.
//
// A layered architecture is a DAG. A cycle in it means two or more modules cannot be
// understood, tested, or replaced independently -- and it is precisely the fact the
// radial layout cannot show, because spanning a tree breadth first turns every back
// edge into a cross-link it ignores (`LayoutSystem::tree_parents`). A view that quietly
// drops its most important structural finding is the same failure NFR-04 forbids for
// evidence: not wrong, but presented as though there were nothing to say.
//
// The finding is the whole group, never "the edge that closes the ring". Which edge
// closes it depends on where the walk started, so naming one of three as the culprit
// would be an arbitrary accusation about code that is equally implicated.
#pragma once

#include "rgv/contract/Graph.h"

#include <utility>
#include <vector>

namespace rgv::analysis {

// A set of modules that all reach each other: every one depends, through some chain, on
// every other. A strongly connected component of more than one node.
struct CycleGroup {
    std::vector<NodeId> nodes;   // sorted, so the report is stable between frames
};

// Every cycle in `edges`, which are (dependent, dependency) pairs. Self-edges are not
// cycles and are ignored. Groups are sorted by their first member, and their members
// sorted, so the same graph always produces the same report however the edges arrived.
//
// Iterative rather than recursive: a file-level import chain is thousands of nodes deep
// in an ordinary repository, and a stack overflow in an analysis is a crash in the
// frame that reads it.
std::vector<CycleGroup> find_cycles(const std::vector<std::pair<NodeId, NodeId>>& edges);

} // namespace rgv::analysis
