#include <sstream>
#include <string>

#include "gt_doctest.hpp"
#include "gatetime/delay_model.hpp"
#include "gatetime/dot_export.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/paths.hpp"
#include "gatetime/report.hpp"
#include "gatetime/timing.hpp"
#include "gatetime/topo.hpp"

using namespace gt;

namespace {

const char* kC17 =
    "INPUT(1)\nINPUT(2)\nINPUT(3)\nINPUT(6)\nINPUT(7)\nOUTPUT(22)\nOUTPUT(23)\n"
    "10 = NAND(1, 3)\n11 = NAND(3, 6)\n16 = NAND(2, 11)\n"
    "19 = NAND(11, 7)\n22 = NAND(10, 16)\n23 = NAND(16, 19)\n";

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

struct Fixture {
    Netlist nl = parseBenchString(kC17);
    TopoResult topo = topoSort(nl);
    TimingResult timing;
    ReportData data;

    explicit Fixture(std::optional<double> period, std::size_t topN = 5) {
        TimingOptions o;
        o.period = period;
        timing = analyzeTiming(nl, topo.order, UnitDelay{}, o);
        data.netlistName = "c17.bench";
        data.delayModelName = "unit";
        data.periodDefaulted = !period.has_value();
        data.netlist = &nl;
        data.topo = &topo;
        data.timing = &timing;
        data.critical = criticalPath(timing);
        data.top = topPaths(nl, timing, topN);
    }
};

}  // namespace

TEST_CASE("default period equals the critical delay") {
    Fixture f(std::nullopt);
    CHECK(f.timing.period == 3.0);
    CHECK(f.timing.worstSlack == 0.0);
}

TEST_CASE("report for c17 at T = 3") {
    Fixture f(3.0);
    std::ostringstream os;
    printReport(os, f.data);
    const std::string s = os.str();
    CHECK(has(s, "Netlist: c17.bench   gates=6 inputs=5 outputs=2 depth=3"));
    CHECK(has(s, "Clock period T = 3.000"));
    CHECK_FALSE(has(s, "defaulted"));
    CHECK(has(s, "Worst slack: 0.000"));
    CHECK(has(s, "Violating endpoints: 0 / 2"));
    CHECK(has(s, "Critical path (delay = 3.000):"));
    CHECK(has(s, "startpoint  3"));
    CHECK(has(s, "endpoint    22 (NAND)"));
    CHECK(has(s, "Top 5 paths:"));
    CHECK(has(s, "startpoint -> endpoint"));
}

TEST_CASE("report for c17 at T = 2 shows the violation") {
    Fixture f(2.0);
    std::ostringstream os;
    printReport(os, f.data);
    const std::string s = os.str();
    CHECK(has(s, "Worst slack: -1.000"));
    CHECK(has(s, "Violating endpoints: 2 / 2"));
    CHECK(has(s, "slack -1.000"));
}

TEST_CASE("defaulted period is called out; quiet summary is one line") {
    Fixture f(std::nullopt);
    std::ostringstream full, quiet;
    printReport(full, f.data);
    printSummary(quiet, f.data);
    CHECK(has(full.str(), "defaulted to the smallest period that meets timing"));
    CHECK(count(quiet.str(), "\n") == 1);
    CHECK(has(quiet.str(), "worst_slack=0.000"));
    CHECK(has(quiet.str(), "violating=0/2"));
}

TEST_CASE("top-N table respects N") {
    Fixture f(3.0, 3);
    std::ostringstream os;
    printReport(os, f.data);
    CHECK(has(os.str(), "Top 3 paths:"));
    // Rows are "  1  ...", "  2  ...", "  3  ..." and no fourth.
    CHECK(has(os.str(), "    3  "));
    CHECK_FALSE(has(os.str(), "    4  "));
}

TEST_CASE("unconstrained logic is reported, not hidden") {
    const Netlist nl = parseBenchString("INPUT(a)\nINPUT(b)\nOUTPUT(z)\nz = NOT(a)\nd = NOT(b)\n");
    const TopoResult topo = topoSort(nl);
    TimingOptions o;
    const TimingResult t = analyzeTiming(nl, topo.order, UnitDelay{}, o);
    ReportData d;
    d.netlistName = "x";
    d.delayModelName = "unit";
    d.netlist = &nl;
    d.topo = &topo;
    d.timing = &t;
    d.critical = criticalPath(t);
    d.top = topPaths(nl, t, 2);
    std::ostringstream os;
    printReport(os, d);
    CHECK(has(os.str(), "Unconstrained nodes: 2"));
}

TEST_CASE("long critical paths are elided in the middle") {
    const int n = 200;
    std::string text = "INPUT(s0)\nOUTPUT(s" + std::to_string(n) + ")\n";
    for (int i = 0; i < n; ++i) {
        text += "s" + std::to_string(i + 1) + " = BUFF(s" + std::to_string(i) + ")\n";
    }
    const Netlist nl = parseBenchString(text);
    const TopoResult topo = topoSort(nl);
    TimingOptions o;
    const TimingResult t = analyzeTiming(nl, topo.order, UnitDelay{}, o);
    ReportData d;
    d.netlistName = "chain";
    d.delayModelName = "unit";
    d.netlist = &nl;
    d.topo = &topo;
    d.timing = &t;
    d.critical = criticalPath(t);
    std::ostringstream os;
    printReport(os, d);
    CHECK(has(os.str(), "161 more nodes"));  // 201 nodes - 20 head - 20 tail
    CHECK(has(os.str(), "s0"));
    CHECK(has(os.str(), "endpoint    s200 (BUFF)"));
}

TEST_CASE("stats counts structure") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    std::ostringstream os;
    printStats(os, "c17.bench", nl, topo);
    const std::string s = os.str();
    CHECK(has(s, "nodes        11"));
    CHECK(has(s, "gates        6"));
    CHECK(has(s, "edges        12"));
    CHECK(has(s, "depth        3"));
    CHECK(has(s, "NAND=6"));
}

TEST_CASE("DOT export: structure and critical path highlight") {
    Fixture f(3.0);
    DotOptions o;
    o.timing = &f.timing;
    o.highlight = &f.data.critical;
    std::ostringstream os;
    writeDot(os, f.nl, o);
    const std::string s = os.str();
    CHECK(has(s, "digraph gatetime {"));
    CHECK(count(s, " -> ") == 12);                      // one per fanin pin
    CHECK(count(s, "[color=red, penwidth=2];") == 3);   // 3 edges on 3-11-16-22
    CHECK(count(s, "color=red, fontcolor=red") == 4);   // 4 nodes on the path
    CHECK(count(s, "peripheries=2") == 2);              // two outputs
    CHECK(has(s, "AT 3.00"));

    std::ostringstream plain;
    writeDot(plain, f.nl);
    CHECK_FALSE(has(plain.str(), "color=red"));
}
