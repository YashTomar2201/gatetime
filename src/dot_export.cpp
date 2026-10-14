#include "gatetime/dot_export.hpp"

#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <unordered_set>

namespace gt {

namespace {

// Escapes a name for use inside a double-quoted DOT string.
std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string fmt2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

}  // namespace

void writeDot(std::ostream& os, const Netlist& nl, const DotOptions& opt) {
    std::vector<char> onPath(nl.size(), 0), isOutput(nl.size(), 0);
    std::unordered_set<std::uint64_t> pathEdges;
    auto key = [](NodeId u, NodeId v) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(u)) << 32) |
               static_cast<std::uint32_t>(v);
    };
    if (opt.highlight) {
        const auto& p = opt.highlight->nodes;
        for (std::size_t i = 0; i < p.size(); ++i) {
            onPath[p[i]] = 1;
            if (i) pathEdges.insert(key(p[i - 1], p[i]));
        }
    }
    for (NodeId e : nl.endpoints()) isOutput[e] = 1;  // primary outputs and flop D nets

    os << "digraph gatetime {\n";
    os << "  rankdir=LR;\n";
    os << "  node [shape=box, fontname=\"Helvetica\", fontsize=10];\n";
    os << "  edge [arrowsize=0.7];\n";

    for (NodeId id = 0; id < static_cast<NodeId>(nl.size()); ++id) {
        const Gate& g = nl.gates[id];
        std::string label = escape(g.name);
        if (g.type != GateType::Input) label += std::string("\\n") + gateTypeName(g.type);
        if (opt.timing) {
            label += "\\nAT " + fmt2(opt.timing->arrival[id]);
            if (opt.timing->slack[id] != kInf) label += "  slack " + fmt2(opt.timing->slack[id]);
        }
        os << "  n" << id << " [label=\"" << label << "\"";
        if (g.fanin.empty()) os << ", shape=ellipse";
        if (g.type == GateType::DffQ) os << ", style=dashed";
        if (isOutput[id]) os << ", peripheries=2";
        if (onPath[id]) os << ", color=red, fontcolor=red, penwidth=2";
        os << "];\n";
    }

    for (NodeId v = 0; v < static_cast<NodeId>(nl.size()); ++v) {
        for (NodeId u : nl.gates[v].fanin) {
            os << "  n" << u << " -> n" << v;
            if (pathEdges.count(key(u, v))) os << " [color=red, penwidth=2]";
            os << ";\n";
        }
    }
    // The clocked connection D -> Q. Timing is cut here, so it does not constrain layout.
    for (const Flop& f : nl.flops) {
        os << "  n" << f.d << " -> n" << f.q << " [style=dashed, color=gray50, constraint=false, label=\"DFF\"];\n";
    }
    os << "}\n";
}

}  // namespace gt
