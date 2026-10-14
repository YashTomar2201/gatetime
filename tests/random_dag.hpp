#pragma once
// Seeded random combinational netlists for property tests.
#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "gatetime/netlist.hpp"
#include "gatetime/parser_bench.hpp"

namespace gttest {

struct RandomDagOptions {
    int  inputs = 4;
    int  gates = 12;
    int  flops = 0;             // flip-flops: extra startpoints q<k> whose D is a random net
    int  maxFanin = 3;
    bool distinctFanin = true;  // false allows NAND(a, a)
    bool shuffle = true;        // shuffle gate lines so ids are NOT in topological order
};

// Returns .bench text. Outputs are every gate nobody reads, plus ~15% of the rest.
// Net ids: inputs i0.., then flop outputs q0.., then gates g0.. (a gate only reads
// nets with a lower index, so the combinational part is acyclic; flop D nets may be
// any net, including the flop's own Q or a later gate, which makes sequential loops).
inline std::string randomDagBench(std::uint32_t seed, const RandomDagOptions& o = {}) {
    std::mt19937 rng(seed);
    auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    static const char* kTypes[] = {"AND", "NAND", "OR", "NOR", "XOR", "XNOR", "NOT", "BUFF"};
    const int start = o.inputs + o.flops;  // first gate index
    const int total = start + o.gates;
    auto nameOf = [&](int i) {
        if (i < o.inputs) return "i" + std::to_string(i);
        if (i < start) return "q" + std::to_string(i - o.inputs);
        return "g" + std::to_string(i - start);
    };

    std::vector<int> readCount(total, 0);
    std::vector<std::string> gateLines;
    for (int g = 0; g < o.gates; ++g) {
        const int self = start + g;
        const char* type = kTypes[uni(0, 7)];
        const bool unary = std::string(type) == "NOT" || std::string(type) == "BUFF";
        int count = unary ? 1 : uni(2, std::max(2, o.maxFanin));
        if (o.distinctFanin) count = std::min(count, self);

        std::vector<int> picks;
        while (static_cast<int>(picks.size()) < count) {
            const int p = uni(0, self - 1);
            if (o.distinctFanin && std::find(picks.begin(), picks.end(), p) != picks.end()) continue;
            picks.push_back(p);
        }
        std::string line = nameOf(self) + " = " + type + "(";
        for (std::size_t k = 0; k < picks.size(); ++k) {
            line += (k ? ", " : "") + nameOf(picks[k]);
            ++readCount[picks[k]];
        }
        gateLines.push_back(line + ")");
    }

    // Flops: q<k> = DFF(<any net>). Reading a net as a flop D counts as being read.
    for (int k = 0; k < o.flops; ++k) {
        const int d = uni(0, total - 1);
        ++readCount[d];
        gateLines.push_back(nameOf(o.inputs + k) + " = DFF(" + nameOf(d) + ")");
    }

    std::string text;
    for (int i = 0; i < o.inputs; ++i) text += "INPUT(" + nameOf(i) + ")\n";
    for (int g = 0; g < o.gates; ++g) {
        const int id = start + g;
        if (readCount[id] == 0 || uni(0, 99) < 15) text += "OUTPUT(" + nameOf(id) + ")\n";
    }
    if (o.shuffle) std::shuffle(gateLines.begin(), gateLines.end(), rng);
    for (const auto& l : gateLines) text += l + "\n";
    return text;
}

inline gt::Netlist randomDag(std::uint32_t seed, const RandomDagOptions& o = {}) {
    return gt::parseBenchString(randomDagBench(seed, o));
}

}  // namespace gttest
