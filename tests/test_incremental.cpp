#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "gt_doctest.hpp"
#include "gatetime/delay_model.hpp"
#include "gatetime/incremental.hpp"
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
    VectorDelay(std::vector<double> d, double setup) : d_(std::move(d)), setup_(setup) {}
    double delay(const Netlist&, NodeId id) const override { return d_[id]; }
    double setupTime() const override { return setup_; }
    std::string name() const override { return "vector"; }

private:
    std::vector<double> d_;
    double setup_;
};

// Incremental and from-scratch results must be identical, not merely close.
void checkSame(const TimingResult& inc, const TimingResult& full) {
    CHECK(inc.period == full.period);
    CHECK(inc.delay == full.delay);
    CHECK(inc.arrival == full.arrival);
    CHECK(inc.required == full.required);
    CHECK(inc.slack == full.slack);
    CHECK(inc.pred == full.pred);
    CHECK(inc.endpoints == full.endpoints);
    CHECK(inc.endpointRequired == full.endpointRequired);
    CHECK(inc.criticalEndpoint == full.criticalEndpoint);
    CHECK(inc.criticalDelay == full.criticalDelay);
    CHECK(inc.worstSlack == full.worstSlack);
    CHECK(inc.violatingEndpoints == full.violatingEndpoints);
    CHECK(inc.unconstrained == full.unconstrained);
    CHECK(inc.tolerance == full.tolerance);
}

}  // namespace

TEST_CASE("incremental: construction equals a full analysis") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    TimingOptions o;
    o.period = 3.0;
    const UnitDelay unit;
    IncrementalTimer timer(nl, topo.order, unit, o);
    checkSame(timer.result(), analyzeTiming(nl, topo.order, unit, o));
    CHECK(timer.update() == 0);  // nothing pending
}

TEST_CASE("incremental: slowing a critical gate matches full recomputation") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    TimingOptions o;
    o.period = 3.0;
    IncrementalTimer timer(nl, topo.order, UnitDelay{}, o);

    const UnitDelay unit;  // OverrideDelay keeps a pointer to its base, so it must outlive it
    OverrideDelay model(unit);
    model.set(nl.find("16"), 2.5);

    timer.setDelay(nl.find("16"), 2.5);
    CHECK(timer.pendingChanges() == 1);
    const std::size_t visited = timer.update();
    CHECK(timer.pendingChanges() == 0);
    CHECK(visited > 0);

    TimingOptions fixed;
    fixed.period = 3.0;
    checkSame(timer.result(), analyzeTiming(nl, topo.order, model, fixed));

    // 16 was on the critical path: 11 (1) -> 16 (2.5) -> 22 (1) arrives at 4.5.
    CHECK(timer.result().arrival[nl.find("22")] == 4.5);
    CHECK(timer.result().worstSlack == -1.5);
    CHECK(timer.result().violatingEndpoints == 2);
}

TEST_CASE("incremental: an edit that cannot reach an endpoint's arrival stops early") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    const UnitDelay unit;
    TimingOptions o;
    o.period = 3.0;
    IncrementalTimer timer(nl, topo.order, unit, o);

    // Gate 10 has slack 1: slowing it by 0.2 changes AT(10) but not AT(22) (16 dominates).
    timer.setDelay(nl.find("10"), 1.2);
    const std::size_t visited = timer.update();
    CHECK(visited < 2 * nl.size());
    CHECK(visited <= 6);  // forward: 10, 22; backward: 1, 3 (RAT(3) is held by gate 11)

    OverrideDelay model(unit);
    model.set(nl.find("10"), 1.2);
    TimingOptions fixed;
    fixed.period = 3.0;
    checkSame(timer.result(), analyzeTiming(nl, topo.order, model, fixed));
    CHECK(timer.result().arrival[nl.find("10")] == 1.2);
    CHECK(timer.result().arrival[nl.find("22")] == 3.0);  // unchanged
}

TEST_CASE("incremental: setting a delay back restores the original result") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    const UnitDelay unit;
    TimingOptions o;
    o.period = 3.0;
    IncrementalTimer timer(nl, topo.order, unit, o);
    const TimingResult original = timer.result();

    timer.setDelay(nl.find("11"), 4.0);
    timer.update();
    CHECK(timer.result().worstSlack < 0);
    timer.setDelay(nl.find("11"), 1.0);
    timer.update();
    checkSame(timer.result(), original);
}

TEST_CASE("incremental: invalid edits are rejected, no-op edits cost nothing") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    TimingOptions o;
    IncrementalTimer timer(nl, topo.order, UnitDelay{}, o);
    CHECK_THROWS_AS(timer.setDelay(-1, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(timer.setDelay(1000, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(timer.setDelay(0, -0.5), std::invalid_argument);
    CHECK_THROWS_AS(timer.setDelay(0, INFINITY), std::invalid_argument);
    CHECK_THROWS_AS(timer.setDelay(0, NAN), std::invalid_argument);

    timer.setDelay(nl.find("16"), 1.0);  // same value as now
    CHECK(timer.pendingChanges() == 0);
    CHECK(timer.update() == 0);
}

TEST_CASE("incremental: the period is fixed at construction") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    TimingOptions o;  // no period: initial smallest feasible period (3.0)
    IncrementalTimer timer(nl, topo.order, UnitDelay{}, o);
    CHECK(timer.result().period == 3.0);
    timer.setDelay(nl.find("16"), 5.0);
    timer.update();
    CHECK(timer.result().period == 3.0);        // not re-derived
    CHECK(timer.result().worstSlack < 0.0);     // so the edit shows up as a violation
}

TEST_CASE("property: random edit sequences on random circuits (with flops) match full recomputation") {
    int updates = 0;
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 2 + static_cast<int>(seed % 4);
        o.flops = static_cast<int>(seed % 4);
        o.gates = 8 + static_cast<int>(seed % 50);
        o.distinctFanin = (seed % 3) != 0;
        const Netlist nl = gttest::randomDag(seed, o);
        const TopoResult topo = topoSort(nl);
        REQUIRE_FALSE(topo.hasCycle());
        CAPTURE(seed);

        std::mt19937 rng(seed * 7u);
        auto real = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };

        std::vector<double> d(nl.size());
        for (double& x : d) x = real(0.1, 2.0);
        const double setup = 0.25;

        TimingOptions opt;
        opt.period = seed % 2 ? std::optional<double>(real(2.0, 12.0)) : std::nullopt;
        opt.inputArrival = (seed % 3) * 0.5;
        IncrementalTimer timer(nl, topo.order, VectorDelay(d, setup), opt);

        TimingOptions fixed = opt;
        fixed.period = timer.result().period;  // full analyses use the timer's frozen period
        checkSame(timer.result(), analyzeTiming(nl, topo.order, VectorDelay(d, setup), fixed));

        for (int step = 0; step < 12; ++step) {
            const int edits = 1 + static_cast<int>(rng() % 3);  // batched edits
            for (int k = 0; k < edits; ++k) {
                const NodeId u = static_cast<NodeId>(rng() % nl.size());
                d[u] = (rng() % 4 == 0) ? 0.0 : real(0.0, 4.0);
                timer.setDelay(u, d[u]);
            }
            timer.update();
            checkSame(timer.result(), analyzeTiming(nl, topo.order, VectorDelay(d, setup), fixed));
            ++updates;
        }
    }
    CHECK(updates == 480);
}

TEST_CASE("incremental: work follows the affected cone, not the circuit size") {
    // A critical chain of 1000 buffers, plus 500 independent side gates hanging off the
    // same input (each an output of its own, with lots of slack).
    const int chain = 1000, side = 500;
    std::string text = "INPUT(a)\nOUTPUT(c" + std::to_string(chain - 1) + ")\n";
    for (int k = 0; k < side; ++k) text += "OUTPUT(s" + std::to_string(k) + ")\n";
    text += "c0 = BUFF(a)\n";
    for (int i = 1; i < chain; ++i) {
        text += "c" + std::to_string(i) + " = BUFF(c" + std::to_string(i - 1) + ")\n";
    }
    for (int k = 0; k < side; ++k) text += "s" + std::to_string(k) + " = NOT(a)\n";

    const Netlist nl = parseBenchString(text);
    const TopoResult topo = topoSort(nl);
    const TableDelay model;
    TimingOptions o;
    IncrementalTimer timer(nl, topo.order, model, o);
    const std::size_t fullWork = 2 * nl.size();  // forward + backward over every node

    // A side gate: itself forward (it is an endpoint), and its one fanin, `a`, backward.
    // `a`'s required time is held by the chain, so nothing else moves.
    timer.setDelay(nl.find("s17"), 0.9);
    const std::size_t sideWork = timer.update();
    CHECK(sideWork <= 2);
    CHECK(sideWork < fullWork / 500);

    // A buffer in the middle of the critical chain: the chain after it moves forward,
    // the chain before it moves backward.
    timer.setDelay(nl.find("c500"), 5.0);
    const std::size_t chainWork = timer.update();
    CHECK(chainWork >= 900);
    CHECK(chainWork < fullWork);

    // And both stayed exact.
    OverrideDelay model2(model);
    model2.set(nl.find("s17"), 0.9);
    model2.set(nl.find("c500"), 5.0);
    TimingOptions fixed;
    fixed.period = timer.result().period;
    checkSame(timer.result(), analyzeTiming(nl, topo.order, model2, fixed));
}
