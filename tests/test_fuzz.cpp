// Robustness: malformed input must be rejected with the documented exception types,
// never crash, hang or throw something else. Mutations are seeded, so failures reproduce.
#include <cmath>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "gt_doctest.hpp"
#include "gatetime/delay_model.hpp"
#include "gatetime/incremental.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/sim.hpp"
#include "gatetime/timing.hpp"
#include "gatetime/topo.hpp"
#include "random_dag.hpp"

using namespace gt;

namespace {

// One random edit: delete, insert, duplicate or swap a span, or truncate.
std::string mutate(std::string s, std::mt19937& rng) {
    static const std::string kAlphabet = "()=,# \t\r\n01xXaAzZ_.-$";
    auto pick = [&](std::size_t n) { return n == 0 ? std::size_t{0} : static_cast<std::size_t>(rng() % n); };
    switch (rng() % 6) {
        case 0:  // delete a character
            if (!s.empty()) s.erase(pick(s.size()), 1);
            break;
        case 1:  // insert a structural / odd character
            s.insert(pick(s.size() + 1), 1, kAlphabet[pick(kAlphabet.size())]);
            break;
        case 2: {  // duplicate a span
            if (s.empty()) break;
            const std::size_t a = pick(s.size()), len = 1 + pick(std::min<std::size_t>(20, s.size() - a));
            s.insert(a, s.substr(a, len));
            break;
        }
        case 3:  // truncate
            s.resize(pick(s.size() + 1));
            break;
        case 4:  // replace a byte with an arbitrary one (including NUL and high bytes)
            if (!s.empty()) s[pick(s.size())] = static_cast<char>(rng() % 256);
            break;
        default: {  // swap two characters
            if (s.size() < 2) break;
            std::swap(s[pick(s.size())], s[pick(s.size())]);
            break;
        }
    }
    return s;
}

// Everything a successfully parsed netlist must satisfy.
void checkWellFormed(const Netlist& nl) {
    const NodeId n = static_cast<NodeId>(nl.size());
    for (NodeId v = 0; v < n; ++v) {
        for (NodeId u : nl.gates[v].fanin) {
            REQUIRE(u >= 0);
            REQUIRE(u < n);
        }
        for (NodeId u : nl.gates[v].fanout) {
            REQUIRE(u >= 0);
            REQUIRE(u < n);
        }
        REQUIRE(nl.find(nl.gates[v].name) == v);
    }
    std::size_t faninEdges = 0, fanoutEdges = 0;
    for (const Gate& g : nl.gates) {
        faninEdges += g.fanin.size();
        fanoutEdges += g.fanout.size();
    }
    REQUIRE(faninEdges == fanoutEdges);
    for (NodeId id : nl.inputs) REQUIRE(nl.gates[id].type == GateType::Input);
    for (NodeId id : nl.outputs) REQUIRE((id >= 0 && id < n));
    for (const Flop& f : nl.flops) {
        REQUIRE(nl.gates[f.q].type == GateType::DffQ);
        REQUIRE(nl.gates[f.q].fanin.empty());
        REQUIRE((f.d >= 0 && f.d < n));
    }
}

}  // namespace

TEST_CASE("fuzz: mutated .bench text parses or throws ParseError, nothing else") {
    int parsed = 0, rejected = 0;
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 3;
        o.flops = static_cast<int>(seed % 3);
        o.gates = 10;
        const std::string base = gttest::randomDagBench(seed, o);
        std::mt19937 rng(seed * 2654435761u);

        for (int round = 0; round < 40; ++round) {
            std::string text = base;
            const int edits = 1 + static_cast<int>(rng() % 4);
            for (int e = 0; e < edits; ++e) text = mutate(std::move(text), rng);
            CAPTURE(seed);
            CAPTURE(round);
            try {
                const Netlist nl = parseBenchString(text);
                checkWellFormed(nl);
                // A parsed netlist must also survive the analyses without throwing,
                // unless the mutation introduced a loop.
                const TopoResult topo = topoSort(nl);
                if (!topo.hasCycle()) {
                    TimingOptions opt;
                    const TimingResult r = analyzeTiming(nl, topo.order, TableDelay{}, opt);
                    CHECK(r.arrival.size() == nl.size());
                    Simulator sim(nl, topo.order);
                    sim.run(std::vector<Word>(nl.inputs.size(), 0x5555));
                }
                ++parsed;
            } catch (const ParseError& e) {
                CHECK(e.line() >= 1);
                ++rejected;
            }
        }
    }
    // Both outcomes must actually occur, or the fuzzer is not testing anything.
    CHECK(parsed > 50);
    CHECK(rejected > 50);
}

TEST_CASE("fuzz: arbitrary bytes never crash the parser") {
    std::mt19937 rng(12345);
    for (int i = 0; i < 400; ++i) {
        std::string junk(rng() % 200, '\0');
        for (char& c : junk) c = static_cast<char>(rng() % 256);
        try {
            const Netlist nl = parseBenchString(junk);
            checkWellFormed(nl);
        } catch (const ParseError&) {
        }
    }
}

TEST_CASE("fuzz: line-oriented near misses") {
    // Hand-picked shapes that exercise the tokenizer's edges.
    const std::vector<std::string> inputs = {
        "", "\n\n\n", "#", "# only comment", "INPUT", "INPUT(", "INPUT()", "INPUT(a", "INPUT(a))",
        "INPUT(a,b)", "OUTPUT", "OUTPUT(a)", "= NAND(a)", "a =", "a = (b)", "a = NAND", "a = NAND(", "a = NAND()",
        "a = NAND(,)", "a = NAND(b,)", "a = NAND(,b)", "a = NAND(b c)", "a = NAND((b))", "a = = NAND(b)",
        "a = NAND(b)(c)", "a = NAND(b) # c", "a=nand(b)", "INPUT(a)\na = NOT(a)", "INPUT(a)\nINPUT(a)",
        "q = DFF(q)", "q = DFF(q)\nOUTPUT(q)", "\xEF\xBB\xBFINPUT(a)", "INPUT(a)\r\nOUTPUT(a)\r\n",
        std::string("INPUT(a)\0INPUT(b)", 17), std::string(5000, '('),
        std::string(5000, 'a') + " = NOT(" + std::string(5000, 'b') + ")",
    };
    for (const std::string& text : inputs) {
        try {
            const Netlist nl = parseBenchString(text);
            checkWellFormed(nl);
        } catch (const ParseError& e) {
            CHECK(e.line() >= 1);
        }
    }
    // Spot checks on expected outcomes.
    CHECK_NOTHROW(parseBenchString("q = DFF(q)\nOUTPUT(q)\n"));  // a flop feeding itself is fine
    CHECK_THROWS_AS(parseBenchString("a = NAND(b c)\n"), ParseError);
    CHECK_THROWS_AS(parseBenchString("a = NAND(b,)\n"), ParseError);
}

TEST_CASE("fuzz: mutated delay files load or throw DelayFileError") {
    const std::string base = "# t\nNOT 0.5\nBUFF 0.6\nNAND 1.0\nNOR 1.1\nAND 1.4\nOR 1.5\nXOR 2.0\nDFF 0.8\nSETUP 0.2\nLOAD 0.05\n";
    std::mt19937 rng(99);
    int ok = 0, bad = 0;
    for (int i = 0; i < 600; ++i) {
        std::string text = base;
        for (int e = 0, n = 1 + static_cast<int>(rng() % 3); e < n; ++e) text = mutate(std::move(text), rng);
        TableDelay m;
        try {
            m.loadString(text, "fuzz");
            ++ok;
            // Whatever loaded must be usable: finite, non-negative.
            for (std::size_t t = 0; t < kGateTypeCount; ++t) {
                const double v = m.get(static_cast<GateType>(t));
                REQUIRE(std::isfinite(v));
                REQUIRE(v >= 0.0);
            }
            REQUIRE(std::isfinite(m.setupTime()));
            REQUIRE(m.setupTime() >= 0.0);
            REQUIRE(std::isfinite(m.loadFactor()));
            REQUIRE(m.loadFactor() >= 0.0);
        } catch (const DelayFileError&) {
            ++bad;
            // A rejected file must leave the model exactly as it was.
            TableDelay fresh;
            CHECK(m.get(GateType::Nand) == fresh.get(GateType::Nand));
            CHECK(m.setupTime() == fresh.setupTime());
        }
    }
    CHECK(ok > 20);
    CHECK(bad > 20);
}

TEST_CASE("fuzz: mutated vector files load or throw VectorFileError") {
    const std::string base = "# vectors\n00000\n11111\n10101 # trailing\n\n01010\n";
    std::mt19937 rng(7);
    int ok = 0, bad = 0;
    for (int i = 0; i < 400; ++i) {
        std::string text = base;
        for (int e = 0, n = 1 + static_cast<int>(rng() % 3); e < n; ++e) text = mutate(std::move(text), rng);
        std::istringstream in(text);
        try {
            const auto v = parseVectors(in, 5, "fuzz");
            for (const std::string& s : v) {
                REQUIRE(s.size() == 5);
                REQUIRE(s.find_first_not_of("01") == std::string::npos);
            }
            ++ok;
        } catch (const VectorFileError&) {
            ++bad;
        }
    }
    CHECK(ok > 20);
    CHECK(bad > 20);
}

TEST_CASE("relative tolerance: rounding noise at large magnitudes is not a violation") {
    // A chain of 1000 gates with awkward delays around 1e5 puts arrivals near 1e8, where
    // one ulp (1.5e-8) is bigger than the old absolute 1e-9. Summing the same numbers in
    // the opposite order differs by a few ulps; a period set to the smaller sum must still
    // count as meeting timing.
    int differed = 0;
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        std::mt19937 rng(seed);
        const int n = 1000;
        std::vector<double> d(n);
        for (double& x : d) x = std::uniform_real_distribution<double>(0.1, 2e5)(rng);

        Netlist nl;
        NodeId prev = nl.getOrCreate("in");
        nl.inputs.push_back(prev);
        std::vector<double> delay(1, 0.0);
        for (int i = 0; i < n; ++i) {
            const NodeId g = nl.getOrCreate("g" + std::to_string(i));
            nl.gates[g].type = GateType::Buff;
            nl.gates[g].fanin.push_back(prev);
            nl.gates[prev].fanout.push_back(g);
            delay.push_back(d[i]);
            prev = g;
        }
        nl.outputs.push_back(prev);

        double forward = 0.0, backward = 0.0;
        for (int i = 0; i < n; ++i) forward += d[i];
        for (int i = n - 1; i >= 0; --i) backward += d[i];
        if (forward != backward) ++differed;

        class Chain : public DelayModel {
        public:
            explicit Chain(std::vector<double> v) : v_(std::move(v)) {}
            double delay(const Netlist&, NodeId id) const override { return v_[id]; }
            std::string name() const override { return "chain"; }

        private:
            std::vector<double> v_;
        };

        const TopoResult topo = topoSort(nl);
        TimingOptions opt;
        opt.period = std::min(forward, backward);
        const TimingResult r = analyzeTiming(nl, topo.order, Chain(delay), opt);
        CAPTURE(seed);
        CHECK(r.violatingEndpoints == 0);
        CHECK(r.tolerance > 1e-3);  // scaled to ~1e8 magnitudes
        for (double s : r.slack) CHECK(std::fabs(s) <= r.tolerance);  // rounding noise only
    }
    CHECK(differed > 0);  // the two summation orders really did disagree somewhere
}
