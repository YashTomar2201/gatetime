#include "gatetime/paths.hpp"

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <string>

namespace gt {

Path criticalPath(const TimingResult& timing) {
    Path p;
    if (timing.criticalEndpoint < 0) return p;
    for (NodeId u = timing.criticalEndpoint; u >= 0; u = timing.pred[u]) p.nodes.push_back(u);
    std::reverse(p.nodes.begin(), p.nodes.end());
    p.delay = timing.arrival[timing.criticalEndpoint];
    p.slack = timing.endpointRequired[timing.criticalEndpoint] - p.delay;
    return p;
}

namespace {

// A partial path is a suffix ending at an endpoint. It is stored as a link to the
// entry one step closer to the endpoint, so partial paths share structure and a
// push costs O(1) instead of copying the whole suffix.
struct Entry {
    NodeId node;      // head of the suffix (the node furthest from the endpoint)
    int    parent;    // arena index of the next node towards the endpoint, -1 at the endpoint
    double suffix;    // sum of delays of the nodes *after* head on this suffix
    double required;  // required time of the endpoint this suffix ends at
};

struct HeapItem {
    // Badness of the best full path through this suffix: arrival[head] + suffix is its
    // exact delay, minus the endpoint's required time gives its negated slack. Larger
    // = worse = extracted first.
    double key;
    int    entry;
};

// std::priority_queue pops the "largest": worst path first. On ties the newer entry
// wins, which makes the search dive to a complete path instead of expanding every
// equal-key partial path breadth-first (exponential on ladders of parallel paths).
struct LowerPriority {
    bool operator()(const HeapItem& a, const HeapItem& b) const {
        if (a.key != b.key) return a.key < b.key;
        return a.entry < b.entry;
    }
};

}  // namespace

std::vector<Path> topPaths(const Netlist& nl, const TimingResult& timing, std::size_t n,
                           std::size_t maxPartialPaths) {
    std::vector<Path> results;
    if (n == 0) return results;

    std::vector<Entry> arena;
    std::priority_queue<HeapItem, std::vector<HeapItem>, LowerPriority> heap;

    for (NodeId e : timing.endpoints) {
        const double req = timing.endpointRequired[e];
        arena.push_back({e, -1, 0.0, req});
        heap.push({timing.arrival[e] - req, static_cast<int>(arena.size()) - 1});
    }

    while (!heap.empty() && results.size() < n) {
        const HeapItem item = heap.top();
        heap.pop();
        const Entry head = arena[item.entry];  // copy: pushes below may reallocate the arena
        const auto& fanin = nl.gates[head.node].fanin;

        // Guard: fail with a clear message instead of exhausting memory when the request
        // is out of proportion (each path costs about depth x fanin entries).
        if (arena.size() > maxPartialPaths) {
            throw std::length_error("topPaths: search exceeded " + std::to_string(maxPartialPaths) +
                                    " partial paths; ask for fewer paths");
        }

        if (fanin.empty()) {
            // Reached a startpoint: the parent chain *is* the path, startpoint first.
            Path p;
            for (int i = item.entry; i >= 0; i = arena[i].parent) p.nodes.push_back(arena[i].node);
            // Re-sum forwards, in the same order as the forward pass, so the delay of
            // the worst path equals arrival[endpoint] exactly.
            double d = timing.arrival[p.nodes.front()];
            for (std::size_t i = 1; i < p.nodes.size(); ++i) d += timing.delay[p.nodes[i]];
            p.delay = d;
            p.slack = head.required - d;
            results.push_back(std::move(p));
            continue;
        }

        const double suffix = head.suffix + timing.delay[head.node];
        for (NodeId f : fanin) {
            // The key of a child is mathematically <= its parent's (AT(h) >= AT(f) + d(h)),
            // and equal when f is on a worst path to h. In floating point the two are
            // computed with different additions, so "equal" can come out an ulp apart in
            // either direction. Treat anything within the slack tolerance as equal and
            // never let a child outrank its parent: exact ties are then really ties, and
            // the newest-first rule dives straight down a plateau of equal paths. Without
            // this, a circuit like c6288 (~10^20 near-equal paths) goes breadth-first.
            double key = timing.arrival[f] + suffix - head.required;
            if (key > item.key || item.key - key <= timing.tolerance) key = item.key;

            arena.push_back({f, item.entry, suffix, head.required});
            heap.push({key, static_cast<int>(arena.size()) - 1});
        }
    }
    return results;
}

}  // namespace gt
