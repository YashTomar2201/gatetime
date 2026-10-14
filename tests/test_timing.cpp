#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "gt_doctest.hpp"
#include "gatetime/delay_model.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/timing.hpp"
#include "gatetime/topo.hpp"
#include "random_dag.hpp"

using namespace gt;

namespace {

const char* kC17 =
    "INPUT(1)\nINPUT(2)\nINPUT(3)\nINPUT(6)\nINPUT(7)\nOUTPUT(22)\nOUTPUT(23)\n"
    "10 = NAND(1, 3)\n11 = NAND(3, 6)\n16 = NAND(2, 11)\n"
    "19 = NAND(11, 7)\n22 = NAND(10, 16)\n23 = NAND(16, 19)\n";

// Explicit per-node delays, indexed by NodeId.
class VectorDelay : public DelayModel {
public:
    explicit VectorDelay(std::vector<double> d) : d_(std::move(d)) {}
    double delay(const Netlist&, NodeId id) const override { return d_[id]; }
    std::string name() const override { return "vector"; }

private:
    std::vector<double> d_;
};

TimingResult run(const Netlist& nl, const DelayModel& m, double period, double inputArrival = 0.0) {
    const TopoResult t = topoSort(nl);
    REQUIRE_FALSE(t.hasCycle());
    TimingOptions o;
    o.period = period;
    o.inputArrival = inputArrival;
    return analyzeTiming(nl, t.order, m, o);
}

double at(const Netlist& nl, const TimingResult& r, const char* name) { return r.arrival[nl.find(name)]; }
double sl(const Netlist& nl, const TimingResult& r, const char* name) { return r.slack[nl.find(name)]; }

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// Independent reference: memoized recursive longest path to u.
double refArrival(const Netlist& nl, const std::vector<double>& d, double inArr, NodeId u,
                  std::vector<double>& memo) {
    if (!std::isnan(memo[u])) return memo[u];
    double v;
    if (nl.gates[u].fanin.empty()) {
        v = inArr;
    } else {
        double best = -kInf;
        for (NodeId f : nl.gates[u].fanin) best = std::max(best, refArrival(nl, d, inArr, f, memo));
        v = best + d[u];
    }
    return memo[u] = v;
}

// Independent reference: memoized recursive required time.
double refRequired(const Netlist& nl, const std::vector<double>& d, const std::vector<char>& isOut,
                   double period, NodeId u, std::vector<double>& memo) {
    if (!std::isnan(memo[u])) return memo[u];
    double v = isOut[u] ? period : kInf;
    for (NodeId s : nl.gates[u].fanout) {
        v = std::min(v, refRequired(nl, d, isOut, period, s, memo) - d[s]);
    }
    return memo[u] = v;
}

}  // namespace

TEST_CASE("c17 unit delay: arrival times match the hand check") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult r = run(nl, UnitDelay{}, 3.0);
    for (const char* in : {"1", "2", "3", "6", "7"}) CHECK(at(nl, r, in) == 0.0);
    CHECK(at(nl, r, "10") == 1.0);
    CHECK(at(nl, r, "11") == 1.0);
    CHECK(at(nl, r, "16") == 2.0);
    CHECK(at(nl, r, "19") == 2.0);
    CHECK(at(nl, r, "22") == 3.0);
    CHECK(at(nl, r, "23") == 3.0);
    CHECK(r.criticalDelay == 3.0);
    CHECK(nl.gates[r.criticalEndpoint].name == "22");  // tie with 23 -> lowest id
}

TEST_CASE("c17, T = critical delay: zero slack on the critical nodes, 1 elsewhere") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult r = run(nl, UnitDelay{}, 3.0);
    for (const char* n : {"3", "6", "11", "16", "19", "22", "23"}) CHECK(near(sl(nl, r, n), 0.0));
    for (const char* n : {"1", "2", "7", "10"}) CHECK(near(sl(nl, r, n), 1.0));
    CHECK(near(r.worstSlack, 0.0));
    CHECK(r.violatingEndpoints == 0);
    CHECK(r.unconstrained == 0);

    CHECK(r.required[nl.find("22")] == 3.0);
    CHECK(r.required[nl.find("16")] == 2.0);
    CHECK(r.required[nl.find("11")] == 1.0);
    CHECK(r.required[nl.find("3")] == 0.0);
}

TEST_CASE("c17, T below the critical delay: negative slack on the critical path") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult r = run(nl, UnitDelay{}, 2.0);
    CHECK(near(sl(nl, r, "22"), -1.0));
    CHECK(near(sl(nl, r, "23"), -1.0));
    CHECK(near(sl(nl, r, "16"), -1.0));
    CHECK(near(sl(nl, r, "3"), -1.0));
    CHECK(near(sl(nl, r, "10"), 0.0));
    CHECK(near(r.worstSlack, -1.0));
    CHECK(r.violatingEndpoints == 2);
}

TEST_CASE("c17, loose period gives positive slack everywhere") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult r = run(nl, UnitDelay{}, 10.0);
    CHECK(near(r.worstSlack, 7.0));
    CHECK(r.violatingEndpoints == 0);
}

TEST_CASE("predecessor pointers follow the worst path, ties to the lowest id") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult r = run(nl, UnitDelay{}, 3.0);
    auto predName = [&](const char* n) {
        const NodeId p = r.pred[nl.find(n)];
        return p < 0 ? std::string("-") : nl.gates[p].name;
    };
    CHECK(predName("22") == "16");  // 16 (AT 2) beats 10 (AT 1)
    CHECK(predName("16") == "11");  // 11 (AT 1) beats 2 (AT 0)
    CHECK(predName("11") == "3");   // tie 3 vs 6 -> lowest id (3 was declared first)
    CHECK(predName("23") == "16");  // tie 16 vs 19 (both AT 2) -> lowest id
    CHECK(predName("3") == "-");
}

TEST_CASE("table delays: a two-gate chain adds up") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(y)\nx = NAND(a)\ny = NOR(x)\n");
    TableDelay m;  // defaults: NAND 1.0, NOR 1.1
    const TimingResult r = run(nl, m, 5.0);
    CHECK(near(at(nl, r, "x"), 1.0));
    CHECK(near(at(nl, r, "y"), 2.1));
    CHECK(near(r.worstSlack, 2.9));
    CHECK(near(sl(nl, r, "a"), 2.9));  // slack is constant along a single path
}

TEST_CASE("arrival time at primary inputs shifts everything") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult r = run(nl, UnitDelay{}, 5.0, 2.0);
    CHECK(at(nl, r, "1") == 2.0);
    CHECK(at(nl, r, "22") == 5.0);
    CHECK(near(r.worstSlack, 0.0));
}

TEST_CASE("a node that is both an endpoint and has fanout takes the tighter requirement") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(m)\nOUTPUT(z)\nm = NOT(a)\nz = BUFF(m)\n");
    const TimingResult r = run(nl, UnitDelay{}, 10.0);
    CHECK(r.required[nl.find("z")] == 10.0);
    CHECK(r.required[nl.find("m")] == 9.0);  // min(T = 10, RAT(z) - 1 = 9)
    CHECK(near(sl(nl, r, "m"), 8.0));
    CHECK(near(sl(nl, r, "z"), 8.0));
}

TEST_CASE("a primary input that is also an output is a zero-delay path") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(a)\n");
    const TimingResult r = run(nl, UnitDelay{}, 4.0);
    CHECK(r.criticalDelay == 0.0);
    CHECK(near(r.worstSlack, 4.0));
}

TEST_CASE("logic that reaches no output is unconstrained (infinite slack)") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nINPUT(b)\nOUTPUT(z)\nz = NOT(a)\ndangling = NOT(b)\n");
    const TimingResult r = run(nl, UnitDelay{}, 3.0);
    CHECK(r.unconstrained == 2);  // b and dangling
    CHECK(r.slack[nl.find("dangling")] == kInf);
    CHECK(r.slack[nl.find("b")] == kInf);
    CHECK(at(nl, r, "dangling") == 1.0);  // arrival is still computed
}

TEST_CASE("no outputs: nothing is constrained and nothing crashes") {
    const Netlist nl = parseBenchString("INPUT(a)\nx = NOT(a)\n");
    const TimingResult r = run(nl, UnitDelay{}, 3.0);
    CHECK(r.criticalEndpoint == -1);
    CHECK(r.worstSlack == kInf);
    CHECK(r.violatingEndpoints == 0);
}

TEST_CASE("a combinational loop is rejected by analyzeTiming") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(x)\nx = AND(a, x)\n");
    const TopoResult t = topoSort(nl);
    TimingOptions o;
    CHECK_THROWS_AS(analyzeTiming(nl, t.order, UnitDelay{}, o), std::invalid_argument);
}

TEST_CASE("TableDelay: defaults, overrides, comments") {
    TableDelay m;
    CHECK(m.get(GateType::Nand) == 1.0);
    CHECK(m.get(GateType::Xor) == 2.0);
    CHECK(m.get(GateType::Input) == 0.0);

    m.loadString("# custom\nnand 2.5  # slow\n\nXOR 3\nbuf 0.25\n", "inline");
    CHECK(m.get(GateType::Nand) == 2.5);
    CHECK(m.get(GateType::Xor) == 3.0);
    CHECK(m.get(GateType::Buff) == 0.25);
    CHECK(m.get(GateType::Nor) == 1.1);  // untouched
    CHECK(m.name() == "table (inline)");
}

TEST_CASE("TableDelay: bad files are rejected with a line number and change nothing") {
    TableDelay m;
    auto message = [&](const std::string& text) {
        try {
            m.loadString(text, "d.txt");
        } catch (const DelayFileError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(message("NAND 1.0\nFROB 2\n") == "d.txt:2: unknown gate type 'FROB'");
    CHECK(message("NAND\n") == "d.txt:1: expected '<KEY> <delay>', got 'NAND'");
    CHECK(message("NAND abc\n") == "d.txt:1: 'abc' is not a number");
    CHECK(message("NAND -1\n") == "d.txt:1: delay must be a finite number >= 0, got '-1'");
    // inf and nan used to slip through (inf >= 0) and turned every slack into NaN.
    CHECK(message("NAND inf\n") == "d.txt:1: delay must be a finite number >= 0, got 'inf'");
    CHECK(message("NAND nan\n") == "d.txt:1: delay must be a finite number >= 0, got 'nan'");
    CHECK(message("NAND 1e999\n").find("is not a number") != std::string::npos);  // out of range
    CHECK(message("NAND 1 2\n") == "d.txt:1: unexpected text '2' after the delay value");
    CHECK(m.get(GateType::Nand) == 1.0);  // the valid first line was not applied
    CHECK_THROWS_AS(m.loadFile("no/such/delay.txt"), std::runtime_error);
}

TEST_CASE("TableDelay file under tests/data loads") {
#ifdef GATETIME_SOURCE_DIR
    TableDelay m;
    m.loadFile(std::string(GATETIME_SOURCE_DIR) + "/bench/delays.txt");
    CHECK(m.get(GateType::Nand) == 1.0);
    CHECK(m.get(GateType::Xor) == 2.0);
#endif
}

TEST_CASE("property: arrival and required times equal memoized recursive references") {
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 2 + static_cast<int>(seed % 4);
        o.gates = 6 + static_cast<int>(seed % 40);
        o.distinctFanin = (seed % 4) != 0;
        const Netlist nl = gttest::randomDag(seed, o);
        CAPTURE(seed);

        std::mt19937 rng(seed * 7919u);
        std::vector<double> d(nl.size());
        for (NodeId i = 0; i < static_cast<NodeId>(nl.size()); ++i) {
            d[i] = nl.gates[i].fanin.empty() ? 0.0 : std::uniform_real_distribution<double>(0.1, 3.0)(rng);
        }
        const double period = 1.0 + (seed % 7) * 3.0;
        const double inArr = (seed % 3) * 0.5;
        const TimingResult r = run(nl, VectorDelay(d), period, inArr);

        std::vector<double> memoAT(nl.size(), std::nan(""));
        std::vector<double> memoRAT(nl.size(), std::nan(""));
        std::vector<char> isOut(nl.size(), 0);
        for (NodeId e : nl.outputs) isOut[e] = 1;

        double maxAT = -kInf;
        for (NodeId u = 0; u < static_cast<NodeId>(nl.size()); ++u) {
            CHECK(near(r.arrival[u], refArrival(nl, d, inArr, u, memoAT)));
            const double rr = refRequired(nl, d, isOut, period, u, memoRAT);
            if (rr == kInf) {
                CHECK(r.required[u] == kInf);
            } else {
                CHECK(near(r.required[u], rr));
            }
        }
        for (NodeId e : nl.outputs) maxAT = std::max(maxAT, r.arrival[e]);

        // Critical delay is the largest endpoint arrival; worst slack = T - that.
        CHECK(near(r.criticalDelay, maxAT));
        CHECK(near(r.worstSlack, period - maxAT));
        // No constrained node can be worse than the worst endpoint.
        for (NodeId u = 0; u < static_cast<NodeId>(nl.size()); ++u) {
            if (r.slack[u] != kInf) CHECK(r.slack[u] >= r.worstSlack - 1e-9);
        }
        // The pred chain from the critical endpoint reproduces the critical delay.
        double sum = inArr;
        NodeId cur = r.criticalEndpoint;
        while (cur >= 0) {
            if (r.pred[cur] >= 0) sum += d[cur];
            cur = r.pred[cur];
        }
        CHECK(near(sum, r.criticalDelay));
    }
}
