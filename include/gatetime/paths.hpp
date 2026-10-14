#pragma once
#include <cstddef>
#include <vector>

#include "gatetime/netlist.hpp"
#include "gatetime/timing.hpp"

namespace gt {

struct Path {
    double delay = 0.0;            // arrival time at the endpoint along this path
    double slack = 0.0;            // endpoint's required time - delay (T, or T - setup at a flop)
    std::vector<NodeId> nodes;     // startpoint ... endpoint
};

// The single worst path, from the predecessor pointers of the forward pass.
// Empty `nodes` if the netlist has no endpoints.
Path criticalPath(const TimingResult& timing);

// The N worst startpoint-to-endpoint paths, smallest slack first (with a single
// clock period and no flops that is the same as non-increasing delay). Fewer are
// returned if the netlist has fewer paths. Best-first search from the endpoints
// backwards using the forward-pass arrival times as an exact heuristic, so each
// extracted path costs O(depth * fanin * log heap), never O(#paths).
//
// Paths are per fanin pin: NAND(a, a) yields two paths with identical nodes.
//
// `maxPartialPaths` bounds the search's memory (about 40 bytes per partial path, and
// roughly depth x fanin of them per requested path). A request that would exceed it
// throws std::length_error instead of exhausting memory.
inline constexpr std::size_t kDefaultMaxPartialPaths = 20'000'000;
std::vector<Path> topPaths(const Netlist& nl, const TimingResult& timing, std::size_t n,
                           std::size_t maxPartialPaths = kDefaultMaxPartialPaths);

}  // namespace gt
