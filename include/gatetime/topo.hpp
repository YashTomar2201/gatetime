#pragma once
#include <vector>

#include "gatetime/netlist.hpp"

namespace gt {

struct TopoResult {
    // Every node Kahn's algorithm could release, in a valid topological order
    // (each edge u -> v has u before v). Holds all nodes iff the graph is acyclic.
    std::vector<NodeId> order;

    // level[v] = length (in edges) of the longest path from any startpoint to v;
    // -1 for unresolved nodes. Nodes at the same level are independent.
    std::vector<int> level;
    int depth = 0;  // max level over resolved nodes

    // Nodes never released: on a combinational loop or downstream of one.
    // Ascending NodeId. Empty iff the netlist is acyclic.
    std::vector<NodeId> unresolved;

    // One concrete loop, in signal-flow order (each node feeds the next, the last
    // feeds the first). Empty iff acyclic.
    std::vector<NodeId> cycle;

    bool hasCycle() const { return !unresolved.empty(); }
};

// Kahn's algorithm, O(V + E), iterative. Detects combinational loops.
TopoResult topoSort(const Netlist& nl);

}  // namespace gt
