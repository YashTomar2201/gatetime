#pragma once
#include <iosfwd>

#include "gatetime/netlist.hpp"
#include "gatetime/paths.hpp"
#include "gatetime/timing.hpp"

namespace gt {

struct DotOptions {
    const TimingResult* timing = nullptr;  // if set, labels include arrival time and slack
    const Path*         highlight = nullptr;  // if set, this path is drawn in red
};

// Graphviz digraph, signal flow left to right. Inputs are ellipses, outputs have a
// double border, the highlighted path (nodes and edges) is red.
void writeDot(std::ostream& os, const Netlist& nl, const DotOptions& opt = {});

}  // namespace gt
