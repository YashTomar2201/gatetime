#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "gt_doctest.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/sim.hpp"
#include "gatetime/topo.hpp"
#include "random_dag.hpp"

using namespace gt;

namespace {

const char* kC17 =
    "INPUT(1)\nINPUT(2)\nINPUT(3)\nINPUT(6)\nINPUT(7)\nOUTPUT(22)\nOUTPUT(23)\n"
    "10 = NAND(1, 3)\n11 = NAND(3, 6)\n16 = NAND(2, 11)\n"
    "19 = NAND(11, 7)\n22 = NAND(10, 16)\n23 = NAND(16, 19)\n";

// Reference evaluator: one bit at a time, no word tricks. `in[i]` is nl.inputs[i];
// `state[i]` is the current Q of nl.flops[i] (all 0 if omitted).
std::vector<bool> scalarEval(const Netlist& nl, const std::vector<NodeId>& order, const std::vector<bool>& in,
                             const std::vector<bool>& state = {}) {
    std::vector<int> slot(nl.size(), -1), flopSlot(nl.size(), -1);
    for (std::size_t i = 0; i < nl.inputs.size(); ++i) slot[nl.inputs[i]] = static_cast<int>(i);
    for (std::size_t i = 0; i < nl.flops.size(); ++i) flopSlot[nl.flops[i].q] = static_cast<int>(i);
    std::vector<bool> val(nl.size(), false);
    for (NodeId u : order) {
        const Gate& g = nl.gates[u];
        auto ones = [&] {
            int c = 0;
            for (NodeId f : g.fanin) c += val[f] ? 1 : 0;
            return c;
        };
        const int n = static_cast<int>(g.fanin.size());
        bool v = false;
        switch (g.type) {
            case GateType::Input: v = slot[u] >= 0 && in[slot[u]]; break;
            case GateType::Buff:  v = val[g.fanin[0]]; break;
            case GateType::Not:   v = !val[g.fanin[0]]; break;
            case GateType::And:   v = ones() == n; break;
            case GateType::Nand:  v = ones() != n; break;
            case GateType::Or:    v = ones() > 0; break;
            case GateType::Nor:   v = ones() == 0; break;
            case GateType::Xor:   v = ones() % 2 == 1; break;
            case GateType::Xnor:  v = ones() % 2 == 0; break;
            case GateType::DffQ:  v = flopSlot[u] >= 0 && static_cast<std::size_t>(flopSlot[u]) < state.size() &&
                                      state[flopSlot[u]];
                                  break;
        }
        val[u] = v;
    }
    return val;
}

// Next flop state after a clock edge, given the net values of this cycle.
std::vector<bool> scalarNextState(const Netlist& nl, const std::vector<bool>& val) {
    std::vector<bool> next(nl.flops.size());
    for (std::size_t i = 0; i < nl.flops.size(); ++i) next[i] = val[nl.flops[i].d];
    return next;
}

}  // namespace

TEST_CASE("exhaustive input words are the standard bit patterns") {
    const auto w = exhaustiveInputs(6, 0);
    REQUIRE(w.size() == 6);
    CHECK(w[0] == 0xAAAAAAAAAAAAAAAAULL);
    CHECK(w[1] == 0xCCCCCCCCCCCCCCCCULL);
    CHECK(w[2] == 0xF0F0F0F0F0F0F0F0ULL);
    CHECK(w[3] == 0xFF00FF00FF00FF00ULL);
    CHECK(w[4] == 0xFFFF0000FFFF0000ULL);
    CHECK(w[5] == 0xFFFFFFFF00000000ULL);
    CHECK(exhaustiveBatches(5) == 1);
    CHECK(exhaustiveBatches(6) == 1);
    CHECK(exhaustiveBatches(9) == 8);
    // Input 6 is constant within a batch and flips with the batch number.
    CHECK(exhaustiveInputs(7, 0)[6] == 0);
    CHECK(exhaustiveInputs(7, 1)[6] == ~std::uint64_t{0});
}

TEST_CASE("c17: all 32 input combinations match the closed-form truth table") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    Simulator sim(nl, topo.order);
    const auto& val = sim.run(exhaustiveInputs(5, 0));

    const NodeId n22 = nl.find("22"), n23 = nl.find("23");
    for (unsigned v = 0; v < 32; ++v) {
        // Inputs in declaration order 1, 2, 3, 6, 7 are bits 0..4 of v.
        const bool i1 = v & 1, i2 = v & 2, i3 = v & 4, i6 = v & 8, i7 = v & 16;
        // c17 simplifies to: 22 = (1.3) + 2.!(3.6)   and   23 = !(3.6).(2 + 7)
        const bool e22 = (i1 && i3) || (i2 && !(i3 && i6));
        const bool e23 = !(i3 && i6) && (i2 || i7);
        CAPTURE(v);
        CHECK(((val[n22] >> v) & 1) == (e22 ? 1u : 0u));
        CHECK(((val[n23] >> v) & 1) == (e23 ? 1u : 0u));
    }
}

TEST_CASE("c17: hand-computed rows") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    // inputs 1,2,3,6,7
    const auto out = simulateVectors(nl, topo.order,
                                     {"00000", "11111", "10100", "01001", "00110", "11000"});
    REQUIRE(out.size() == 6);
    CHECK(out[0] == "00");  // all zeros: 10=1 11=1 16=1 19=1 -> 22=0 23=0
    CHECK(out[1] == "10");  // all ones:  10=0 11=0 16=1 19=1 -> 22=1 23=0
    CHECK(out[2] == "10");  // 10=0 11=1 16=1 19=1 -> 22=1 23=0
    CHECK(out[3] == "11");  // 10=1 11=1 16=0 19=0 -> 22=1 23=1
    CHECK(out[4] == "00");  // 10=1 11=0 16=1 19=1 -> 22=0 23=0
    CHECK(out[5] == "11");  // 10=1 11=1 16=0 19=1 -> 22=1 23=1
}

TEST_CASE("bit-parallel equals scalar on random netlists of every gate type") {
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 2 + static_cast<int>(seed % 6);
        o.gates = 5 + static_cast<int>(seed % 60);
        o.maxFanin = 4;
        o.distinctFanin = (seed % 4) != 0;
        const Netlist nl = gttest::randomDag(seed, o);
        const TopoResult topo = topoSort(nl);
        Simulator sim(nl, topo.order);
        CAPTURE(seed);

        SplitMix64 rng(seed);
        const std::vector<Word> in = randomInputs(nl.inputs.size(), rng);
        const std::vector<Word>& val = sim.run(in);

        for (unsigned bit = 0; bit < 64; ++bit) {
            std::vector<bool> scalarIn(nl.inputs.size());
            for (std::size_t i = 0; i < in.size(); ++i) scalarIn[i] = (in[i] >> bit) & 1;
            const std::vector<bool> ref = scalarEval(nl, topo.order, scalarIn);
            for (NodeId u = 0; u < static_cast<NodeId>(nl.size()); ++u) {
                REQUIRE(static_cast<bool>((val[u] >> bit) & 1) == ref[u]);
            }
        }
    }
}

TEST_CASE("exhaustive over 8 inputs (4 batches) matches scalar") {
    gttest::RandomDagOptions o;
    o.inputs = 8;
    o.gates = 30;
    o.maxFanin = 4;
    const Netlist nl = gttest::randomDag(77, o);
    const TopoResult topo = topoSort(nl);
    Simulator sim(nl, topo.order);

    for (std::uint64_t batch = 0; batch < exhaustiveBatches(8); ++batch) {
        const std::vector<Word> in = exhaustiveInputs(8, batch);
        const std::vector<Word>& val = sim.run(in);
        for (unsigned bit = 0; bit < 64; ++bit) {
            const std::uint64_t vec = batch * 64 + bit;  // global vector number
            std::vector<bool> scalarIn(8);
            for (std::size_t k = 0; k < 8; ++k) scalarIn[k] = (vec >> k) & 1;
            const std::vector<bool> ref = scalarEval(nl, topo.order, scalarIn);
            for (NodeId out : nl.outputs) REQUIRE(static_cast<bool>((val[out] >> bit) & 1) == ref[out]);
        }
    }
}

TEST_CASE("wide gates: 4-input NAND, parity XOR/XNOR") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nINPUT(b)\nINPUT(c)\nINPUT(d)\nOUTPUT(n)\nOUTPUT(x)\nOUTPUT(xn)\n"
        "n = NAND(a, b, c, d)\nx = XOR(a, b, c)\nxn = XNOR(a, b, c)\n");
    const TopoResult topo = topoSort(nl);
    const auto out = simulateVectors(nl, topo.order, {"1111", "1110", "1100", "1000", "0000", "0111"});
    //                                    n x xn
    CHECK(out[0] == "010");  // NAND(1,1,1,1)=0, parity(1,1,1)=1
    CHECK(out[1] == "110");  // NAND=1, parity(1,1,1)=1
    CHECK(out[2] == "101");  // parity(1,1,0)=0
    CHECK(out[3] == "110");  // parity(1,0,0)=1
    CHECK(out[4] == "101");  // parity(0,0,0)=0
    CHECK(out[5] == "101");  // 0,1,1 -> parity 0
}

TEST_CASE("random simulation counts only the requested vectors") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);

    for (std::uint64_t count : {1ull, 63ull, 64ull, 65ull, 100ull, 1024ull}) {
        const RandomSimResult r = simulateRandom(nl, topo.order, count, 42, 4);
        CAPTURE(count);
        CHECK(r.vectors == count);
        REQUIRE(r.ones.size() == 2);
        for (auto ones : r.ones) CHECK(ones <= count);

        // Recompute with the scalar reference from the same random stream.
        SplitMix64 rng(42);
        std::vector<std::uint64_t> expect(2, 0);
        std::uint64_t seen = 0;
        while (seen < count) {
            const std::vector<Word> in = randomInputs(5, rng);
            for (unsigned bit = 0; bit < 64 && seen < count; ++bit, ++seen) {
                std::vector<bool> scalarIn(5);
                for (std::size_t i = 0; i < 5; ++i) scalarIn[i] = (in[i] >> bit) & 1;
                const auto ref = scalarEval(nl, topo.order, scalarIn);
                for (std::size_t o = 0; o < 2; ++o) expect[o] += ref[nl.outputs[o]] ? 1 : 0;
            }
        }
        CHECK(r.ones == expect);
        CHECK(r.sampleInputs.size() == std::min<std::uint64_t>(count, 4));
        CHECK(r.sampleOutputs.size() == r.sampleInputs.size());
    }
}

TEST_CASE("random simulation is reproducible per seed and differs across seeds") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    const auto a = simulateRandom(nl, topo.order, 4096, 7);
    const auto b = simulateRandom(nl, topo.order, 4096, 7);
    const auto c = simulateRandom(nl, topo.order, 4096, 8);
    CHECK(a.ones == b.ones);
    CHECK(a.sampleInputs == b.sampleInputs);
    CHECK(a.sampleInputs != c.sampleInputs);
}

TEST_CASE("vectors file: parsing and errors") {
    std::istringstream good("# comment\n\n10100\n 01110  # trailing\r\n11111\n");
    const auto v = parseVectors(good, 5, "v.txt");
    CHECK(v == std::vector<std::string>{"10100", "01110", "11111"});

    auto message = [](const std::string& text, std::size_t n) {
        std::istringstream in(text);
        try {
            parseVectors(in, n, "v.txt");
        } catch (const VectorFileError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(message("101\n10\n", 3) == "v.txt:2: vector has 2 bits but the netlist has 3 inputs");
    CHECK(message("10x\n", 3) == "v.txt:1: vector '10x' may only contain 0 and 1");
}

TEST_CASE("a netlist with a loop cannot be simulated") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(x)\nx = AND(a, x)\n");
    const TopoResult topo = topoSort(nl);
    CHECK_THROWS_AS(Simulator(nl, topo.order), std::invalid_argument);
}

TEST_CASE("sequential: a toggle flop alternates, state starts at 0") {
    const Netlist nl = parseBenchString("OUTPUT(q)\nq = DFF(d)\nd = NOT(q)\n");
    const TopoResult topo = topoSort(nl);
    const auto out = simulateVectors(nl, topo.order, {"", "", "", "", "", ""});
    CHECK(out == std::vector<std::string>{"0", "1", "0", "1", "0", "1"});
}

TEST_CASE("sequential: a 2-stage shift register delays its input by two cycles") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(q2)\nq1 = DFF(a)\nq2 = DFF(q1)\n");
    const TopoResult topo = topoSort(nl);
    // Output shows q2 before the edge: out[t] = a[t - 2], and 0 for the first two cycles.
    const auto out = simulateVectors(nl, topo.order, {"1", "0", "0", "0", "1", "1"});
    CHECK(out == std::vector<std::string>{"0", "0", "1", "0", "0", "0"});
}

TEST_CASE("sequential: reset() returns every flop to 0") {
    const Netlist nl = parseBenchString("OUTPUT(q)\nq = DFF(d)\nd = NOT(q)\n");
    const TopoResult topo = topoSort(nl);
    Simulator sim(nl, topo.order);
    const NodeId q = nl.find("q");
    CHECK((sim.run({})[q] & 1) == 0);  // cycle 0: q = 0
    CHECK((sim.run({})[q] & 1) == 1);  // cycle 1: q = 1
    sim.reset();
    CHECK((sim.run({})[q] & 1) == 0);  // back to the reset state
}

TEST_CASE("sequential: bit-parallel equals scalar over many cycles on random circuits") {
    for (std::uint32_t seed = 1; seed <= 25; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 2 + static_cast<int>(seed % 4);
        o.flops = 1 + static_cast<int>(seed % 4);
        o.gates = 6 + static_cast<int>(seed % 30);
        o.maxFanin = 4;
        const Netlist nl = gttest::randomDag(seed, o);
        const TopoResult topo = topoSort(nl);
        REQUIRE_FALSE(topo.hasCycle());
        Simulator sim(nl, topo.order);
        CAPTURE(seed);

        SplitMix64 rng(seed);
        std::vector<std::vector<bool>> state(64, std::vector<bool>(nl.flops.size(), false));
        for (int cycle = 0; cycle < 10; ++cycle) {
            const std::vector<Word> in = randomInputs(nl.inputs.size(), rng);
            const std::vector<Word>& val = sim.run(in);
            for (unsigned lane = 0; lane < 64; ++lane) {
                std::vector<bool> scalarIn(nl.inputs.size());
                for (std::size_t i = 0; i < in.size(); ++i) scalarIn[i] = (in[i] >> lane) & 1;
                const std::vector<bool> ref = scalarEval(nl, topo.order, scalarIn, state[lane]);
                for (NodeId u = 0; u < static_cast<NodeId>(nl.size()); ++u) {
                    REQUIRE(static_cast<bool>((val[u] >> lane) & 1) == ref[u]);
                }
                state[lane] = scalarNextState(nl, ref);
            }
        }
    }
}

TEST_CASE("sequential: simulateRandom with cycles counts every cycle of every sequence") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nINPUT(b)\nOUTPUT(z)\nOUTPUT(q2)\n"
        "q1 = DFF(d1)\nq2 = DFF(d2)\n"
        "d1 = XOR(a, q2)\nd2 = AND(b, q1)\nz = OR(q1, q2)\n");
    const TopoResult topo = topoSort(nl);

    for (std::uint64_t count : {1ull, 64ull, 100ull}) {
        const std::uint64_t cycles = 7;
        const RandomSimResult r = simulateRandom(nl, topo.order, count, 9, 8, cycles);
        CAPTURE(count);
        CHECK(r.vectors == count);
        CHECK(r.cycles == cycles);
        CHECK(r.sampleInputs.size() == 7);  // sequence 0, up to 8 cycles but only 7 exist

        // Scalar replay of the same random stream.
        SplitMix64 rng(9);
        std::vector<std::uint64_t> expect(2, 0);
        for (std::uint64_t done = 0; done < count; done += 64) {
            const unsigned valid = static_cast<unsigned>(std::min<std::uint64_t>(64, count - done));
            std::vector<std::vector<bool>> state(64, std::vector<bool>(2, false));
            for (std::uint64_t c = 0; c < cycles; ++c) {
                const std::vector<Word> in = randomInputs(2, rng);
                for (unsigned lane = 0; lane < valid; ++lane) {
                    std::vector<bool> scalarIn = {static_cast<bool>((in[0] >> lane) & 1),
                                                  static_cast<bool>((in[1] >> lane) & 1)};
                    const auto ref = scalarEval(nl, topo.order, scalarIn, state[lane]);
                    for (std::size_t o = 0; o < 2; ++o) expect[o] += ref[nl.outputs[o]] ? 1 : 0;
                    state[lane] = scalarNextState(nl, ref);
                }
            }
        }
        CHECK(r.ones == expect);
    }
    CHECK_THROWS_AS(simulateRandom(nl, topo.order, 8, 1, 8, 0), std::invalid_argument);
}

TEST_CASE("simulator validates fanin counts of hand-built netlists") {
    auto build = [](GateType type, int fanins) {
        Netlist nl;
        const NodeId a = nl.getOrCreate("a");
        nl.inputs.push_back(a);
        const NodeId g = nl.getOrCreate("g");
        nl.gates[g].type = type;
        for (int i = 0; i < fanins; ++i) {
            nl.gates[g].fanin.push_back(a);
            nl.gates[a].fanout.push_back(g);
        }
        return nl;
    };
    auto order = [](const Netlist& nl) { return topoSort(nl).order; };

    for (auto [type, fanins, ok] : std::vector<std::tuple<GateType, int, bool>>{
             {GateType::Not, 1, true},  {GateType::Not, 0, false}, {GateType::Not, 2, false},
             {GateType::Buff, 0, false}, {GateType::And, 0, false}, {GateType::And, 3, true},
             {GateType::Xor, 1, true}}) {
        const Netlist nl = build(type, fanins);
        CAPTURE(gateTypeName(type));
        CAPTURE(fanins);
        if (ok) {
            CHECK_NOTHROW(Simulator(nl, order(nl)));
        } else {
            CHECK_THROWS_AS(Simulator(nl, order(nl)), std::invalid_argument);
        }
    }

    // A DffQ node with no matching flop record is rejected too.
    Netlist nl = build(GateType::Input, 0);
    nl.gates[nl.find("g")].type = GateType::DffQ;
    CHECK_THROWS_AS(Simulator(nl, order(nl)), std::invalid_argument);
}

TEST_CASE("run() checks the input word count") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult topo = topoSort(nl);
    Simulator sim(nl, topo.order);
    CHECK_THROWS_AS(sim.run({0, 0}), std::invalid_argument);
}
