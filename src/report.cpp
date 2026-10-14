#include "gatetime/report.hpp"

#include "gatetime/sim.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ostream>

namespace gt {

namespace {

std::string format(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

// `snap`: magnitudes below it print as 0, so rounding noise never shows as "-0.000".
std::string num(double v, double snap = kTimeEps, int precision = 3) {
    if (v == kInf) return "inf";
    if (v == -kInf) return "-inf";
    if (v > -snap && v < snap) v = 0.0;
    return format("%.*f", precision, v);
}

std::string pad(const std::string& s, std::size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}
std::string padLeft(const std::string& s, std::size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
}

// "name" for primary inputs, "name (TYPE)" for gates and flops.
std::string label(const Netlist& nl, NodeId id) {
    const Gate& g = nl.gates[id];
    if (g.type == GateType::Input) return g.name;
    return g.name + " (" + gateTypeName(g.type) + ")";
}

// "a > b > c"; long paths keep the first and last four nodes.
std::string pathString(const Netlist& nl, const Path& p) {
    const std::size_t keep = 4;
    std::string s;
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        if (p.nodes.size() > 2 * keep + 1 && i == keep) {
            s += " > ...";
            i = p.nodes.size() - keep - 1;
            continue;
        }
        if (!s.empty()) s += " > ";
        s += nl.gates[p.nodes[i]].name;
    }
    return s;
}

// Combinational gates on the path (the startpoint, a primary input or flop Q, is not one).
std::size_t gatesOnPath(const Netlist& nl, const Path& p) {
    return static_cast<std::size_t>(
        std::count_if(p.nodes.begin(), p.nodes.end(),
                      [&](NodeId id) { return !nl.gates[id].fanin.empty(); }));
}

}  // namespace

void printReport(std::ostream& os, const ReportData& d) {
    const Netlist& nl = *d.netlist;
    const TimingResult& t = *d.timing;

    const double tol = t.tolerance;
    os << "Netlist: " << d.netlistName << "   gates=" << nl.gateCount() - nl.flops.size()
       << " inputs=" << nl.inputs.size() << " outputs=" << nl.outputs.size();
    if (!nl.flops.empty()) os << " flops=" << nl.flops.size();
    os << " depth=" << d.topo->depth << "\n";
    os << "Delay model: " << d.delayModelName << "   Clock period T = " << num(t.period, tol)
       << (d.periodDefaulted ? " (defaulted to the smallest period that meets timing)" : "") << "\n";

    std::string phases;
    auto addPhase = [&](const char* nameStr, double ms) {
        if (ms >= 0) phases += format("%s: %.3f ms   ", nameStr, ms);
    };
    addPhase("Parse", d.parseMs);
    addPhase("Topo", d.topoMs);
    addPhase("Timing", d.timingMs);
    addPhase("Paths", d.pathsMs);
    if (!phases.empty()) os << phases.substr(0, phases.size() - 3) << "\n";
    os << "\n";

    if (t.endpoints.empty()) {
        os << "No OUTPUT(...) or DFF declared: there are no endpoints, so nothing is constrained.\n";
        return;
    }

    os << "Worst slack: " << num(t.worstSlack, tol) << "    Violating endpoints: " << t.violatingEndpoints
       << " / " << t.endpoints.size() << "\n";
    if (t.unconstrained > 0) {
        os << "Unconstrained nodes: " << t.unconstrained
           << " (no path to any endpoint; slack shown as inf)\n";
    }
    os << "\n";

    // Critical path.
    const Path& cp = d.critical;
    os << "Critical path (delay = " << num(cp.delay, tol) << "):\n";
    std::size_t width = 0;
    for (NodeId id : cp.nodes) width = std::max(width, label(nl, id).size());
    const std::size_t kHead = 20, kTail = 20;
    const bool elide = cp.nodes.size() > kHead + kTail + 1;
    for (std::size_t i = 0; i < cp.nodes.size(); ++i) {
        if (elide && i == kHead) {
            os << "              ... " << (cp.nodes.size() - kHead - kTail) << " more nodes ...\n";
            i = cp.nodes.size() - kTail - 1;
            continue;
        }
        const NodeId id = cp.nodes[i];
        const bool first = i == 0, last = i + 1 == cp.nodes.size();
        const char* role = first && last ? "start/end  " : first ? "startpoint  " : last ? "endpoint    " : "            ";
        os << "  " << role << pad(label(nl, id), width) << "  AT " << padLeft(num(t.arrival[id], tol), 9);
        if (last) os << "   slack " << num(t.endpointRequired[id] - t.arrival[id], tol);
        os << "\n";
    }
    os << "\n";

    // Top-N table.
    if (!d.top.empty()) {
        os << "Top " << d.top.size() << " path" << (d.top.size() == 1 ? "" : "s") << ":\n";
        std::vector<std::string> ends;
        std::size_t endsWidth = std::string("startpoint -> endpoint").size();
        for (const Path& p : d.top) {
            ends.push_back(nl.gates[p.nodes.front()].name + " -> " + nl.gates[p.nodes.back()].name);
            endsWidth = std::max(endsWidth, ends.back().size());
        }
        os << "  " << padLeft("#", 3) << "  " << padLeft("delay", 10) << "  " << padLeft("slack", 10)
           << "  " << pad("startpoint -> endpoint", endsWidth) << "  " << pad("gates", 5) << "  path\n";
        for (std::size_t i = 0; i < d.top.size(); ++i) {
            const Path& p = d.top[i];
            os << "  " << padLeft(std::to_string(i + 1), 3) << "  " << padLeft(num(p.delay, tol), 10) << "  "
               << padLeft(num(p.slack, tol), 10) << "  " << pad(ends[i], endsWidth) << "  "
               << pad(std::to_string(gatesOnPath(nl, p)), 5) << "  " << pathString(nl, p) << "\n";
        }
    }
}

void printSummary(std::ostream& os, const ReportData& d) {
    const TimingResult& t = *d.timing;
    os << d.netlistName << ": T=" << num(t.period, t.tolerance) << " critical=" << num(t.criticalDelay, t.tolerance)
       << " worst_slack=" << num(t.worstSlack, t.tolerance) << " violating=" << t.violatingEndpoints << "/"
       << t.endpoints.size() << "\n";
}

namespace {

std::string names(const Netlist& nl, const std::vector<NodeId>& ids) {
    std::string s;
    for (NodeId id : ids) s += (s.empty() ? "" : " ") + nl.gates[id].name;
    return s;
}

}  // namespace

void printRandomSim(std::ostream& os, const std::string& name, const Netlist& nl, const RandomSimResult& r,
                    std::uint64_t seed, double ms) {
    os << "Netlist: " << name << "   inputs=" << nl.inputs.size() << " outputs=" << nl.outputs.size();
    if (!nl.flops.empty()) os << " flops=" << nl.flops.size();
    os << "\n";
    const std::uint64_t evals = r.vectors * r.cycles;
    os << "Simulated " << r.vectors << " random " << (r.cycles > 1 ? "sequences" : "vectors");
    if (r.cycles > 1) os << " x " << r.cycles << " cycles";
    os << " (seed " << seed << ") in " << format("%.3f", ms) << " ms";
    if (ms > 0) os << "  (" << format("%.1f", static_cast<double>(evals) / ms / 1000.0) << " M evaluations/s)";
    os << "\n\n";

    os << "Output ones count (of " << evals << " evaluations):\n";
    for (std::size_t o = 0; o < nl.outputs.size(); ++o) {
        const double pct = evals ? 100.0 * static_cast<double>(r.ones[o]) / static_cast<double>(evals) : 0.0;
        os << "  " << pad(nl.gates[nl.outputs[o]].name, 12) << padLeft(std::to_string(r.ones[o]), 10) << " / "
           << evals << "  (" << format("%.1f", pct) << "%)\n";
    }

    if (!r.sampleInputs.empty()) {
        os << "\nFirst " << r.sampleInputs.size()
           << (r.cycles > 1 ? " cycles of sequence 0 (from the reset state):\n" : " vectors:\n");
        os << "  inputs (" << names(nl, nl.inputs) << ")  ->  outputs (" << names(nl, nl.outputs) << ")\n";
        for (std::size_t i = 0; i < r.sampleInputs.size(); ++i) {
            os << "  " << r.sampleInputs[i] << "  ->  " << r.sampleOutputs[i] << "\n";
        }
    }
}

void printVectorSim(std::ostream& os, const std::string& name, const Netlist& nl,
                    const std::vector<std::string>& inputs, const std::vector<std::string>& outputs) {
    os << "Netlist: " << name << "   " << inputs.size()
       << (nl.flops.empty() ? " vectors" : " clock cycles (one sequence from the reset state)") << "\n";
    os << "  inputs (" << names(nl, nl.inputs) << ")  ->  outputs (" << names(nl, nl.outputs) << ")\n";
    for (std::size_t i = 0; i < inputs.size(); ++i) os << "  " << inputs[i] << "  ->  " << outputs[i] << "\n";
}

void printStats(std::ostream& os, const std::string& name, const Netlist& nl, const TopoResult& topo) {
    std::size_t edges = 0, maxFanin = 0, maxFanout = 0, withFanout = 0;
    for (const Gate& g : nl.gates) {
        edges += g.fanin.size();
        maxFanin = std::max(maxFanin, g.fanin.size());
        maxFanout = std::max(maxFanout, g.fanout.size());
        if (!g.fanout.empty()) ++withFanout;
    }
    std::size_t typeCount[kGateTypeCount] = {};
    for (const Gate& g : nl.gates) ++typeCount[static_cast<std::size_t>(g.type)];

    os << "Netlist: " << name << "\n";
    os << "  nodes        " << nl.size() << "\n";
    os << "  inputs       " << nl.inputs.size() << "\n";
    os << "  outputs      " << nl.outputs.size() << "\n";
    os << "  gates        " << nl.gateCount() - nl.flops.size() << "\n";
    if (!nl.flops.empty()) os << "  flops        " << nl.flops.size() << "\n";
    os << "  edges        " << edges << "\n";
    os << "  depth        " << topo.depth << (topo.hasCycle() ? "  (loop: depth covers the resolved part)" : "") << "\n";
    os << "  avg fanout   " << format("%.2f", withFanout ? static_cast<double>(edges) / withFanout : 0.0)
       << "  (edges / nodes that drive something)\n";
    os << "  max fanin    " << maxFanin << "\n";
    os << "  max fanout   " << maxFanout << "\n";
    os << "  gate types  ";
    for (std::size_t i = 0; i < kGateTypeCount; ++i) {
        if (typeCount[i] && static_cast<GateType>(i) != GateType::Input) {
            os << " " << gateTypeName(static_cast<GateType>(i)) << "=" << typeCount[i];
        }
    }
    os << "\n";
    if (topo.hasCycle()) os << "  combinational loop: " << topo.unresolved.size() << " nodes unresolved\n";
}

}  // namespace gt
