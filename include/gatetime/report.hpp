#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "gatetime/netlist.hpp"
#include "gatetime/paths.hpp"
#include "gatetime/timing.hpp"
#include "gatetime/topo.hpp"

namespace gt {

struct ReportData {
    std::string netlistName;
    std::string delayModelName;
    bool periodDefaulted = false;  // T was not given: it equals the critical delay

    const Netlist*      netlist = nullptr;
    const TopoResult*   topo = nullptr;
    const TimingResult* timing = nullptr;
    Path                critical;
    std::vector<Path>   top;

    // Wall-clock phases in milliseconds; negative = not measured (omitted).
    double parseMs = -1, topoMs = -1, timingMs = -1, pathsMs = -1;
};

// Full human-readable report: header, worst slack, critical path, top-N table.
void printReport(std::ostream& os, const ReportData& d);

// One line: worst slack and violation count (for --quiet and scripts).
void printSummary(std::ostream& os, const ReportData& d);

// Structure statistics for the `stats` command.
void printStats(std::ostream& os, const std::string& name, const Netlist& nl, const TopoResult& topo);

struct RandomSimResult;  // sim.hpp

// `sim --random`: per-output ones count plus the first few vectors.
void printRandomSim(std::ostream& os, const std::string& name, const Netlist& nl, const RandomSimResult& r,
                    std::uint64_t seed, double ms);

// `sim --vectors`: one input -> output line per vector.
void printVectorSim(std::ostream& os, const std::string& name, const Netlist& nl,
                    const std::vector<std::string>& inputs, const std::vector<std::string>& outputs);

}  // namespace gt
