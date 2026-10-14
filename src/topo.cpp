#include "gatetime/topo.hpp"

#include <algorithm>

namespace gt {

TopoResult topoSort(const Netlist& nl) {
    const std::size_t n = nl.gates.size();
    TopoResult r;

    std::vector<int> indeg(n);
    for (std::size_t i = 0; i < n; ++i) indeg[i] = static_cast<int>(nl.gates[i].fanin.size());

    r.level.assign(n, 0);
    r.order.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (indeg[i] == 0) r.order.push_back(static_cast<NodeId>(i));
    }

    // `order` is also the FIFO queue: everything released so far, in release order.
    for (std::size_t head = 0; head < r.order.size(); ++head) {
        const NodeId u = r.order[head];
        for (NodeId v : nl.gates[u].fanout) {
            r.level[v] = std::max(r.level[v], r.level[u] + 1);
            if (--indeg[v] == 0) r.order.push_back(v);
        }
    }

    for (NodeId u : r.order) r.depth = std::max(r.depth, r.level[u]);

    // A node whose in-degree never reached 0 is on a loop or downstream of one.
    for (std::size_t i = 0; i < n; ++i) {
        if (indeg[i] > 0) {
            r.unresolved.push_back(static_cast<NodeId>(i));
            r.level[i] = -1;
        }
    }
    if (r.unresolved.empty()) return r;

    // Every unresolved node has an unresolved fanin (otherwise all its inputs would
    // have been released and it would have been too). Walking backwards through
    // such fanins must therefore revisit a node: that is a loop.
    std::vector<int> posInPath(n, -1);
    std::vector<NodeId> path;
    NodeId cur = r.unresolved.front();
    while (posInPath[cur] < 0) {
        posInPath[cur] = static_cast<int>(path.size());
        path.push_back(cur);
        NodeId next = -1;
        for (NodeId f : nl.gates[cur].fanin) {
            if (indeg[f] > 0) {
                next = f;
                break;
            }
        }
        cur = next;
    }
    r.cycle.assign(path.begin() + posInPath[cur], path.end());
    std::reverse(r.cycle.begin(), r.cycle.end());  // walked against signal flow
    return r;
}

}  // namespace gt
