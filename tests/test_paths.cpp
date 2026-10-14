#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "gt_doctest.hpp"
#include "gatetime/delay_model.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/paths.hpp"
#include "gatetime/timing.hpp"
#include "gatetime/topo.hpp"
#include "random_dag.hpp"

using namespace gt;

namespace {

const char* kC17 =
    "INPUT(1)\nINPUT(2)\nINPUT(3)\nINPUT(6)\nINPUT(7)\nOUTPUT(22)\nOUTPUT(23)\n"
    "10 = NAND(1, 3)\n11 = NAND(3, 6)\n16 = NAND(2, 11)\n"
    "19 = NAND(11, 7)\n22 = NAND(10, 16)\n23 = NAND(16, 19)\n";

class VectorDelay : public DelayModel {
public:
    explicit VectorDelay(std::vector<double> d, double setup = 0.0) : d_(std::move(d)), setup_(setup) {}
    double delay(const Netlist&, NodeId id) const override { return d_[id]; }
    double setupTime() const override { return setup_; }
    std::string name() const override { return "vector"; }

private:
    std::vector<double> d_;
    double setup_;
};

TimingResult run(const Netlist& nl, const DelayModel& m, double period) {
    const TopoResult t = topoSort(nl);
    REQUIRE_FALSE(t.hasCycle());
    TimingOptions o;
    o.period = period;
    return analyzeTiming(nl, t.order, m, o);
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

std::string joined(const Netlist& nl, const Path& p) {
    std::string s;
    for (std::size_t i = 0; i < p.nodes.size(); ++i) s += (i ? "-" : "") + nl.gates[p.nodes[i]].name;
    return s;
}

// Brute force: enumerate every startpoint-to-endpoint path (per fanin pin), returning
// each path's delay. Exponential, so only for tiny circuits.
void enumerate(const Netlist& nl, const TimingResult& t, NodeId u, std::vector<NodeId>& stack,
               std::vector<double>& out) {
    stack.push_back(u);
    if (nl.gates[u].fanin.empty()) {
        double d = t.arrival[u];  // stack is endpoint..startpoint; startpoint contributes AT only
        for (std::size_t i = 0; i + 1 < stack.size(); ++i) d += t.delay[stack[i]];
        out.push_back(d);
    } else {
        for (NodeId f : nl.gates[u].fanin) enumerate(nl, t, f, stack, out);
    }
    stack.pop_back();
}

// Slack of every path (endpoint's own required time minus the path delay), ascending:
// the order topPaths() must produce.
std::vector<double> bruteForceSlacks(const Netlist& nl, const TimingResult& t) {
    std::vector<double> all;
    std::vector<NodeId> stack;
    for (NodeId e : t.endpoints) {
        std::vector<double> delays;
        enumerate(nl, t, e, stack, delays);
        for (double d : delays) all.push_back(t.endpointRequired[e] - d);
    }
    std::sort(all.begin(), all.end());
    return all;
}

// A returned path must be a real path with the delay it claims.
void checkPathIsValid(const Netlist& nl, const TimingResult& t, const Path& p) {
    REQUIRE_FALSE(p.nodes.empty());
    CHECK(nl.gates[p.nodes.front()].fanin.empty());  // starts at a startpoint
    CHECK(std::find(t.endpoints.begin(), t.endpoints.end(), p.nodes.back()) != t.endpoints.end());
    double d = t.arrival[p.nodes.front()];
    for (std::size_t i = 1; i < p.nodes.size(); ++i) {
        const auto& fin = nl.gates[p.nodes[i]].fanin;
        CHECK(std::find(fin.begin(), fin.end(), p.nodes[i - 1]) != fin.end());  // real edge
        d += t.delay[p.nodes[i]];
    }
    CHECK(near(p.delay, d));
    CHECK(near(p.slack, t.endpointRequired[p.nodes.back()] - d));
}

}  // namespace

TEST_CASE("c17 critical path: 3 -> 11 -> 16 -> 22, delay 3") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult t = run(nl, UnitDelay{}, 3.0);
    const Path p = criticalPath(t);
    CHECK(joined(nl, p) == "3-11-16-22");
    CHECK(p.delay == 3.0);
    CHECK(near(p.slack, 0.0));
    checkPathIsValid(nl, t, p);
}

TEST_CASE("c17 has 11 paths: six of delay 3, five of delay 2") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult t = run(nl, UnitDelay{}, 3.0);
    const auto paths = topPaths(nl, t, 100);
    REQUIRE(paths.size() == 11);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        CHECK(paths[i].delay == (i < 6 ? 3.0 : 2.0));
        checkPathIsValid(nl, t, paths[i]);
    }
    std::set<std::string> distinct;
    for (const Path& p : paths) distinct.insert(joined(nl, p));
    CHECK(distinct.size() == 11);

    // The six critical ones, as sets (their relative order among ties is unspecified).
    std::set<std::string> top6;
    for (std::size_t i = 0; i < 6; ++i) top6.insert(joined(nl, paths[i]));
    CHECK(top6 == std::set<std::string>{"3-11-16-22", "6-11-16-22", "3-11-16-23", "6-11-16-23",
                                        "3-11-19-23", "6-11-19-23"});
}

TEST_CASE("top-N edge cases: N = 0, N = 1, N larger than the path count") {
    const Netlist nl = parseBenchString(kC17);
    const TimingResult t = run(nl, UnitDelay{}, 3.0);
    CHECK(topPaths(nl, t, 0).empty());
    const auto one = topPaths(nl, t, 1);
    REQUIRE(one.size() == 1);
    CHECK(one[0].delay == 3.0);
    CHECK(topPaths(nl, t, 1000000).size() == 11);
}

TEST_CASE("top-N with table delays orders by delay") {
    // Two parallel routes of different cost into one output.
    const Netlist nl = parseBenchString(
        "INPUT(a)\nINPUT(b)\nOUTPUT(z)\n"
        "slow = XOR(a, a)\n"     // 2.0
        "fast = NOT(b)\n"        // 0.5
        "z = OR(slow, fast)\n");  // 1.5
    TableDelay m;
    const TimingResult t = run(nl, m, 10.0);
    const auto paths = topPaths(nl, t, 10);
    REQUIRE(paths.size() == 3);  // a-slow-z twice (two pins), b-fast-z
    CHECK(near(paths[0].delay, 3.5));
    CHECK(near(paths[1].delay, 3.5));
    CHECK(near(paths[2].delay, 2.0));
    CHECK(joined(nl, paths[2]) == "b-fast-z");
}

TEST_CASE("no outputs gives no paths") {
    const Netlist nl = parseBenchString("INPUT(a)\nx = NOT(a)\n");
    const TimingResult t = run(nl, UnitDelay{}, 1.0);
    CHECK(topPaths(nl, t, 5).empty());
    CHECK(criticalPath(t).nodes.empty());
}

TEST_CASE("primary input that is an output is a single-node path") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(a)\n");
    const TimingResult t = run(nl, UnitDelay{}, 1.0);
    const auto paths = topPaths(nl, t, 5);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0].nodes.size() == 1);
    CHECK(paths[0].delay == 0.0);
}

TEST_CASE("property: top-N equals brute-force enumeration on random small DAGs") {
    int checked = 0;
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 2 + static_cast<int>(seed % 3);
        o.gates = 4 + static_cast<int>(seed % 14);  // < 20 gates
        o.maxFanin = 3;
        o.flops = static_cast<int>(seed % 3);        // 0-2 flops: extra startpoints and D endpoints
        o.distinctFanin = (seed % 5) != 0;           // some circuits have NAND(a, a)
        const Netlist nl = gttest::randomDag(seed, o);
        CAPTURE(seed);

        std::mt19937 rng(seed * 104729u);
        std::vector<double> d(nl.size());
        for (NodeId i = 0; i < static_cast<NodeId>(nl.size()); ++i) {
            // Integer-ish delays create many exact ties, the hard case for ordering.
            // Startpoints may carry a delay too (a flop's clock-to-Q).
            const bool startpoint = nl.gates[i].fanin.empty();
            d[i] = static_cast<double>(std::uniform_int_distribution<int>(startpoint ? 0 : 1, 4)(rng));
            if (seed % 2) d[i] += 0.25;
        }
        // Flop D pins need to arrive `setup` early, so endpoints have different requirements
        // and ranking by slack differs from ranking by delay.
        const TimingResult t = run(nl, VectorDelay(d, /*setup=*/1.5), 20.0);
        const std::vector<double> brute = bruteForceSlacks(nl, t);
        REQUIRE_FALSE(brute.empty());

        for (std::size_t n : {std::size_t{1}, std::size_t{3}, std::size_t{10}, brute.size(), brute.size() + 5}) {
            const auto got = topPaths(nl, t, n);
            const std::size_t expect = std::min(n, brute.size());
            REQUIRE(got.size() == expect);
            for (std::size_t i = 0; i < expect; ++i) {
                CHECK(near(got[i].slack, brute[i]));
                if (i) CHECK(got[i].slack >= got[i - 1].slack - 1e-9);  // worst (smallest slack) first
                checkPathIsValid(nl, t, got[i]);
            }
        }

        // The critical path ends at the endpoint with the smallest slack.
        const Path crit = criticalPath(t);
        CHECK(near(crit.slack, brute.front()));
        CHECK(near(crit.slack, t.worstSlack));
        checkPathIsValid(nl, t, crit);
        ++checked;
    }
    CHECK(checked == 60);
}

TEST_CASE("fuzz: with distinct fanin every returned path is unique") {
    for (std::uint32_t seed = 100; seed < 130; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 3;
        o.gates = 15;
        const Netlist nl = gttest::randomDag(seed, o);
        const TimingResult t = run(nl, UnitDelay{}, 10.0);
        const auto paths = topPaths(nl, t, 1000);
        std::set<std::vector<NodeId>> distinct;
        for (const Path& p : paths) distinct.insert(p.nodes);
        CHECK(distinct.size() == paths.size());
        CHECK(paths.size() == bruteForceSlacks(nl, t).size());
    }
}

TEST_CASE("exponentially many equal paths: top-5 stays cheap") {
    // 60 stages, each a diamond of two parallel buffers: 2^60 paths in total.
    std::string text = "INPUT(s0)\nOUTPUT(s60)\n";
    for (int k = 0; k < 60; ++k) {
        const std::string in = "s" + std::to_string(k), out = "s" + std::to_string(k + 1);
        text += "x" + std::to_string(k) + " = BUFF(" + in + ")\n";
        text += "y" + std::to_string(k) + " = BUFF(" + in + ")\n";
        text += out + " = OR(x" + std::to_string(k) + ", y" + std::to_string(k) + ")\n";
    }
    const Netlist nl = parseBenchString(text);
    const TimingResult t = run(nl, UnitDelay{}, 200.0);
    const auto paths = topPaths(nl, t, 5);
    REQUIRE(paths.size() == 5);
    for (const Path& p : paths) {
        CHECK(p.delay == 120.0);  // 60 stages x (buff + or)
        CHECK(p.nodes.size() == 121);
    }
}

TEST_CASE("regression: paths that tie mathematically but not in floating point (c6288)") {
    // A lattice like an array multiplier: every cell reads two cells of the row above, so
    // there are ~2^rows paths, all of equal delay on paper. With a delay such as 1.1 the
    // search key of the best continuation differs from its parent's by an ulp (a + (b + c)
    // is not (a + b) + c), and an ordering that only trusts *exact* ties then expands every
    // pending partial path of the row above first: exponential. The real ISCAS'85 c6288
    // multiplier (~10^20 paths) ran out of memory exactly this way.
    const int rows = 60, width = 8;
    auto cell = [](int r, int c) { return "x" + std::to_string(r) + "_" + std::to_string(c); };
    std::string text;
    for (int c = 0; c < width; ++c) text += "INPUT(" + cell(0, c) + ")\n";
    for (int c = 0; c < width; ++c) text += "OUTPUT(" + cell(rows, c) + ")\n";
    for (int r = 1; r <= rows; ++r) {
        for (int c = 0; c < width; ++c) {
            text += cell(r, c) + " = NOR(" + cell(r - 1, c) + ", " + cell(r - 1, (c + 1) % width) + ")\n";
        }
    }
    const Netlist nl = parseBenchString(text);
    TableDelay model;
    model.set(GateType::Nor, 1.1);
    const TimingResult t = run(nl, model, 1000.0);

    const auto paths = topPaths(nl, t, 10);
    REQUIRE(paths.size() == 10);
    for (const Path& p : paths) {
        CHECK(std::fabs(p.delay - t.criticalDelay) < 1e-9);
        CHECK(std::fabs(p.slack - t.worstSlack) < 1e-9);
        CHECK(p.nodes.size() == static_cast<std::size_t>(rows) + 1);
        checkPathIsValid(nl, t, p);
    }
}

TEST_CASE("regression: the real ISCAS'85 c6288 multiplier") {
#ifdef GATETIME_SOURCE_DIR
    const Netlist nl = parseBenchFile(std::string(GATETIME_SOURCE_DIR) + "/bench/iscas/c6288.bench");
    CHECK(nl.inputs.size() == 32);
    CHECK(nl.outputs.size() == 32);
    CHECK(nl.gateCount() == 2416);
    const TimingResult t = run(nl, TableDelay{}, 1000.0);
    const auto paths = topPaths(nl, t, 25);
    REQUIRE(paths.size() == 25);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        CHECK(std::fabs(paths[i].delay - t.criticalDelay) < 1e-6);  // the multiplier has a huge plateau of critical paths
        checkPathIsValid(nl, t, paths[i]);
    }
#endif
}

TEST_CASE("an out-of-proportion request fails with a clear error, not bad_alloc") {
    // 2^30 paths; asking for 4 billion of them must hit the partial-path limit and say so.
    // (A small explicit limit keeps this fast, including under libstdc++ debug mode where
    // every push_heap re-validates the whole heap.)
    std::string text = "INPUT(s0)\nOUTPUT(s30)\n";
    for (int k = 0; k < 30; ++k) {
        const std::string in = "s" + std::to_string(k), out = "s" + std::to_string(k + 1);
        const std::string K = std::to_string(k);
        text += "a" + K + " = BUFF(" + in + ")\n";
        text += "b" + K + " = NOT(" + in + ")\n";
        text += out + " = OR(a" + K + ", b" + K + ")\n";
    }
    const Netlist nl = parseBenchString(text);
    const TimingResult t = run(nl, TableDelay{}, 100.0);
    CHECK_THROWS_AS(topPaths(nl, t, 4000000000ull, 5000), std::length_error);

    // The same circuit with a reasonable request is fine under the same limit.
    const auto some = topPaths(nl, t, 10, 5000);
    CHECK(some.size() == 10);
}

TEST_CASE("deep chain: iterative, no stack overflow") {
    const int n = 150000;
    Netlist nl;
    NodeId prev = nl.getOrCreate("in");
    nl.inputs.push_back(prev);
    for (int i = 0; i < n; ++i) {
        const NodeId g = nl.getOrCreate("g" + std::to_string(i));
        nl.gates[g].type = GateType::Buff;
        nl.gates[g].fanin.push_back(prev);
        nl.gates[prev].fanout.push_back(g);
        prev = g;
    }
    nl.outputs.push_back(prev);
    const TimingResult t = run(nl, UnitDelay{}, static_cast<double>(n));
    CHECK(t.criticalDelay == static_cast<double>(n));
    const auto paths = topPaths(nl, t, 3);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0].nodes.size() == static_cast<std::size_t>(n) + 1);
    CHECK(criticalPath(t).nodes.size() == static_cast<std::size_t>(n) + 1);
}
