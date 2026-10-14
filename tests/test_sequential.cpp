#include <cmath>
#include <functional>
#include <random>
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

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

const char* kToggle =
    "INPUT(a)\nOUTPUT(z)\n"
    "q = DFF(d)\n"
    "d = NAND(a, q)\n"
    "z = NOT(q)\n";

TimingResult run(const Netlist& nl, const DelayModel& m, std::optional<double> period = std::nullopt) {
    const TopoResult t = topoSort(nl);
    REQUIRE_FALSE(t.hasCycle());
    TimingOptions o;
    o.period = period;
    return analyzeTiming(nl, t.order, m, o);
}

class VectorDelay : public DelayModel {
public:
    VectorDelay(std::vector<double> d, double setup) : d_(std::move(d)), setup_(setup) {}
    double delay(const Netlist&, NodeId id) const override { return d_[id]; }
    double setupTime() const override { return setup_; }
    std::string name() const override { return "vector"; }

private:
    std::vector<double> d_;
    double setup_;
};

}  // namespace

TEST_CASE("a feedback loop through a flop is not a combinational loop") {
    const Netlist nl = parseBenchString(kToggle);
    const TopoResult t = topoSort(nl);
    CHECK_FALSE(t.hasCycle());
    CHECK(t.order.size() == nl.size());
    CHECK(t.level[nl.find("q")] == 0);  // Q starts a new path
    CHECK(t.level[nl.find("d")] == 1);
}

TEST_CASE("flop timing by hand: clock-to-Q launches, setup tightens the D endpoint") {
    const Netlist nl = parseBenchString(kToggle);
    TableDelay m;  // DFF 0.8 (clock-to-Q), NAND 1.0, NOT 0.5, SETUP 0.2
    const TimingResult r = run(nl, m);

    const NodeId q = nl.find("q"), d = nl.find("d"), z = nl.find("z");
    CHECK(near(r.arrival[q], 0.8));   // 0 + clock-to-Q
    CHECK(near(r.arrival[d], 1.8));   // max(a = 0, q = 0.8) + 1.0
    CHECK(near(r.arrival[z], 1.3));   // q + 0.5
    CHECK(r.pred[d] == q);

    // Smallest feasible period: D must arrive 0.2 early, so T = 1.8 + 0.2 = 2.0.
    CHECK(near(r.period, 2.0));
    CHECK(near(r.endpointRequired[d], 1.8));  // T - setup
    CHECK(near(r.endpointRequired[z], 2.0));  // T
    CHECK(near(r.worstSlack, 0.0));
    CHECK(r.criticalEndpoint == d);
    CHECK(near(r.slack[z], 0.7));
    CHECK(r.endpoints.size() == 2);
    CHECK(r.violatingEndpoints == 0);
}

TEST_CASE("flop timing: a period below the requirement violates at the D pin") {
    const Netlist nl = parseBenchString(kToggle);
    TableDelay m;
    const TimingResult r = run(nl, m, 1.5);
    CHECK(near(r.slack[nl.find("d")], -0.5));  // 1.5 - 0.2 - 1.8
    CHECK(near(r.slack[nl.find("z")], 0.2));   // 1.5 - 1.3
    CHECK(near(r.worstSlack, -0.5));
    CHECK(r.violatingEndpoints == 1);
}

TEST_CASE("unit delay: flop outputs launch at 0 and setup is 0") {
    const Netlist nl = parseBenchString(kToggle);
    const TimingResult r = run(nl, UnitDelay{});
    CHECK(r.arrival[nl.find("q")] == 0.0);
    CHECK(r.arrival[nl.find("d")] == 1.0);
    CHECK(r.arrival[nl.find("z")] == 1.0);
    CHECK(r.period == 1.0);
}

TEST_CASE("load-dependent delay: base + k * fanout") {
    const Netlist nl = parseBenchString(kToggle);
    TableDelay m;
    m.setLoadFactor(0.1);
    const TimingResult r = run(nl, m, 10.0);
    // q drives d and z (fanout 2): 0.8 + 0.2. d and z drive nothing but endpoints.
    CHECK(near(r.delay[nl.find("q")], 1.0));
    CHECK(near(r.delay[nl.find("d")], 1.0));
    CHECK(near(r.delay[nl.find("z")], 0.5));
    CHECK(near(r.arrival[nl.find("d")], 2.0));
    CHECK(near(r.arrival[nl.find("z")], 1.5));
    CHECK(m.name().find("per fanout") != std::string::npos);

    // Primary inputs stay free regardless of load.
    CHECK(r.delay[nl.find("a")] == 0.0);
}

TEST_CASE("delay file: DFF, SETUP and LOAD keys") {
    TableDelay m;
    m.loadString("DFF 0.4\nSETUP 0.1\nLOAD 0.05\nNAND 2\n", "inline");
    CHECK(m.get(GateType::DffQ) == 0.4);
    CHECK(m.setupTime() == 0.1);
    CHECK(m.loadFactor() == 0.05);
    CHECK(m.get(GateType::Nand) == 2.0);
}

TEST_CASE("setup time reorders paths: a shorter flop path can be the worst") {
    // Path to the output is 2 buffers; path to the flop is 1 buffer plus 1.5 of setup.
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(po)\n"
        "x = BUFF(a)\npo = BUFF(x)\n"
        "y = BUFF(a)\nq = DFF(y)\n");
    TableDelay m;
    m.set(GateType::Buff, 1.0);
    m.setSetupTime(1.5);
    const TimingResult r = run(nl, m, 5.0);

    const auto paths = topPaths(nl, r, 5);
    REQUIRE(paths.size() == 2);
    // Flop path: delay 1.0, slack 5 - 1.5 - 1.0 = 2.5.   Output path: delay 2.0, slack 3.0.
    CHECK(near(paths[0].delay, 1.0));
    CHECK(near(paths[0].slack, 2.5));
    CHECK(paths[0].nodes.back() == nl.find("y"));
    CHECK(near(paths[1].delay, 2.0));
    CHECK(near(paths[1].slack, 3.0));

    const Path crit = criticalPath(r);
    CHECK(crit.nodes.back() == nl.find("y"));  // smallest slack, not largest delay
    CHECK(near(crit.slack, 2.5));
}

TEST_CASE("paths launch at flop outputs and end at flop D pins") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(z)\n"
        "q1 = DFF(d1)\nq2 = DFF(d2)\n"
        "d1 = AND(a, q2)\n"
        "d2 = NOT(q1)\n"
        "z = OR(q1, q2)\n");
    TableDelay m;
    const TimingResult r = run(nl, m, 10.0);
    const auto paths = topPaths(nl, r, 100);
    // Endpoints: z, d1, d2. Paths: z <- q1, q2;  d1 <- a, q2;  d2 <- q1.
    CHECK(paths.size() == 5);
    for (const Path& p : paths) {
        const Gate& start = nl.gates[p.nodes.front()];
        CHECK((start.type == GateType::Input || start.type == GateType::DffQ));
        CHECK(start.fanin.empty());
    }
}

TEST_CASE("s27: counts, depth and unit-delay timing by hand") {
#ifdef GATETIME_SOURCE_DIR
    const Netlist nl = parseBenchFile(std::string(GATETIME_SOURCE_DIR) + "/bench/s27.bench");
    CHECK(nl.inputs.size() == 4);
    CHECK(nl.outputs.size() == 1);
    CHECK(nl.flops.size() == 3);
    CHECK(nl.gateCount() - nl.flops.size() == 10);

    const TopoResult topo = topoSort(nl);
    REQUIRE_FALSE(topo.hasCycle());
    CHECK(topo.depth == 6);

    // Unit delay, hand-propagated: G14=1 G12=1 G13=2 G8=2 G15=3 G16=3 G9=4 G11=5 G10=6 G17=6.
    const TimingResult r = run(nl, UnitDelay{});
    auto at = [&](const char* n) { return r.arrival[nl.find(n)]; };
    CHECK(at("G14") == 1.0);
    CHECK(at("G12") == 1.0);
    CHECK(at("G13") == 2.0);
    CHECK(at("G8") == 2.0);
    CHECK(at("G15") == 3.0);
    CHECK(at("G16") == 3.0);
    CHECK(at("G9") == 4.0);
    CHECK(at("G11") == 5.0);
    CHECK(at("G10") == 6.0);
    CHECK(at("G17") == 6.0);

    CHECK(r.endpoints.size() == 4);  // G17 (output) and the D nets G10, G11, G13
    CHECK(r.period == 6.0);
    CHECK(r.worstSlack == 0.0);

    const TimingResult tight = run(nl, UnitDelay{}, 5.0);
    CHECK(tight.violatingEndpoints == 2);  // G17 and G10 at 6 > 5
    CHECK(near(tight.worstSlack, -1.0));
#endif
}

TEST_CASE("property: flops, setup and clock-to-Q agree with independent references") {
    for (std::uint32_t seed = 1; seed <= 50; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 2 + static_cast<int>(seed % 3);
        o.flops = 1 + static_cast<int>(seed % 4);
        o.gates = 6 + static_cast<int>(seed % 30);
        const Netlist nl = gttest::randomDag(seed, o);
        CAPTURE(seed);

        std::mt19937 rng(seed * 31u);
        std::vector<double> d(nl.size());
        for (double& x : d) x = std::uniform_real_distribution<double>(0.1, 2.0)(rng);
        const double setup = 0.3;
        const VectorDelay model(d, setup);
        const TimingResult r = run(nl, model);

        // Reference arrival (memoized recursion); startpoints = 0 + their own delay.
        std::vector<double> at(nl.size(), std::nan(""));
        std::function<double(NodeId)> arrive = [&](NodeId u) -> double {
            if (!std::isnan(at[u])) return at[u];
            double v = 0.0;
            if (nl.gates[u].fanin.empty()) {
                v = d[u];
            } else {
                double best = -kInf;
                for (NodeId f : nl.gates[u].fanin) best = std::max(best, arrive(f));
                v = best + d[u];
            }
            return at[u] = v;
        };
        for (NodeId u = 0; u < static_cast<NodeId>(nl.size()); ++u) CHECK(near(r.arrival[u], arrive(u)));

        // Reference smallest period: every endpoint arrival (+ setup at flop D pins).
        std::vector<double> margin(nl.size(), 0.0);
        for (const Flop& f : nl.flops) margin[f.d] = setup;
        double need = 0.0;
        bool any = false;
        for (NodeId e : nl.endpoints()) {
            need = any ? std::max(need, arrive(e) + margin[e]) : arrive(e) + margin[e];
            any = true;
        }
        CHECK(near(r.period, need));
        CHECK(near(r.worstSlack, 0.0));
        CHECK(r.violatingEndpoints == 0);

        // Endpoint slacks against their own requirement.
        for (NodeId e : r.endpoints) {
            CHECK(near(r.endpointRequired[e] - r.arrival[e], r.period - margin[e] - arrive(e)));
        }
    }
}
