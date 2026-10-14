#include <algorithm>
#include <string>
#include <vector>

#include "gt_doctest.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/topo.hpp"
#include "random_dag.hpp"

using namespace gt;

namespace {

const char* kC17 =
    "INPUT(1)\nINPUT(2)\nINPUT(3)\nINPUT(6)\nINPUT(7)\nOUTPUT(22)\nOUTPUT(23)\n"
    "10 = NAND(1, 3)\n11 = NAND(3, 6)\n16 = NAND(2, 11)\n"
    "19 = NAND(11, 7)\n22 = NAND(10, 16)\n23 = NAND(16, 19)\n";

// Every edge u -> v must have pos[u] < pos[v], and order must be a permutation.
void checkValidOrder(const Netlist& nl, const TopoResult& t) {
    REQUIRE(t.order.size() == nl.size());
    std::vector<int> pos(nl.size(), -1);
    for (std::size_t i = 0; i < t.order.size(); ++i) {
        REQUIRE(pos[t.order[i]] == -1);  // no repeats
        pos[t.order[i]] = static_cast<int>(i);
    }
    for (NodeId v = 0; v < static_cast<NodeId>(nl.size()); ++v) {
        for (NodeId u : nl.gates[v].fanin) CHECK(pos[u] < pos[v]);
    }
}

// Sorted, so tests do not depend on how ids were assigned.
std::vector<std::string> names(const Netlist& nl, const std::vector<NodeId>& ids) {
    std::vector<std::string> out;
    for (NodeId id : ids) out.push_back(nl.gates[id].name);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("c17: valid order, no cycle, levels and depth") {
    const Netlist nl = parseBenchString(kC17);
    const TopoResult t = topoSort(nl);
    CHECK_FALSE(t.hasCycle());
    CHECK(t.unresolved.empty());
    CHECK(t.cycle.empty());
    checkValidOrder(nl, t);

    for (const char* in : {"1", "2", "3", "6", "7"}) CHECK(t.level[nl.find(in)] == 0);
    CHECK(t.level[nl.find("10")] == 1);
    CHECK(t.level[nl.find("11")] == 1);
    CHECK(t.level[nl.find("16")] == 2);
    CHECK(t.level[nl.find("19")] == 2);
    CHECK(t.level[nl.find("22")] == 3);
    CHECK(t.level[nl.find("23")] == 3);
    CHECK(t.depth == 3);
}

TEST_CASE("three-gate loop is detected and reported") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(z)\n"
        "x = AND(a, z)\n"
        "y = NOT(x)\n"
        "z = BUFF(y)\n");
    const TopoResult t = topoSort(nl);
    REQUIRE(t.hasCycle());
    CHECK(names(nl, t.unresolved) == std::vector<std::string>{"x", "y", "z"});
    CHECK(t.order.size() == 1);  // only the input "a" was released
    CHECK(t.level[nl.find("x")] == -1);

    REQUIRE(t.cycle.size() == 3);
    for (std::size_t i = 0; i < t.cycle.size(); ++i) {
        const NodeId from = t.cycle[i];
        const NodeId to = t.cycle[(i + 1) % t.cycle.size()];
        const auto& fo = nl.gates[from].fanout;
        CHECK(std::find(fo.begin(), fo.end(), to) != fo.end());  // from feeds to
    }
}

TEST_CASE("nodes downstream of a loop are unresolved but not on the cycle") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(w)\n"
        "x = AND(a, y)\ny = NOT(x)\n"   // 2-node loop
        "w = BUFF(y)\n");               // hangs off the loop
    const TopoResult t = topoSort(nl);
    REQUIRE(t.hasCycle());
    CHECK(names(nl, t.unresolved) == std::vector<std::string>{"w", "x", "y"});
    CHECK(t.cycle.size() == 2);
    CHECK(std::find(t.cycle.begin(), t.cycle.end(), nl.find("w")) == t.cycle.end());
}

TEST_CASE("self loop is a one-node cycle") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(x)\nx = AND(a, x)\n");
    const TopoResult t = topoSort(nl);
    REQUIRE(t.hasCycle());
    CHECK(t.cycle == std::vector<NodeId>{nl.find("x")});
}

TEST_CASE("isolated inputs and empty netlists") {
    const Netlist two = parseBenchString("INPUT(a)\nINPUT(b)\n");
    const TopoResult t = topoSort(two);
    CHECK_FALSE(t.hasCycle());
    CHECK(t.order.size() == 2);
    CHECK(t.depth == 0);

    const Netlist none = parseBenchString("");
    const TopoResult e = topoSort(none);
    CHECK_FALSE(e.hasCycle());
    CHECK(e.order.empty());
    CHECK(e.depth == 0);
}

TEST_CASE("duplicate fanin does not confuse in-degree counting") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(z)\nz = NAND(a, a)\n");
    const TopoResult t = topoSort(nl);
    CHECK_FALSE(t.hasCycle());
    CHECK(t.order.size() == 2);
    CHECK(t.level[nl.find("z")] == 1);
}

TEST_CASE("a very deep chain does not overflow the stack") {
    // 200k gates in a line: any recursive DFS would blow the stack.
    const int n = 200000;
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
    const TopoResult t = topoSort(nl);
    CHECK_FALSE(t.hasCycle());
    CHECK(t.order.size() == static_cast<std::size_t>(n) + 1);
    CHECK(t.depth == n);
}

TEST_CASE("random DAGs (ids not in topological order) always sort validly") {
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        gttest::RandomDagOptions o;
        o.inputs = 2 + static_cast<int>(seed % 5);
        o.gates = 5 + static_cast<int>(seed % 40);
        o.distinctFanin = (seed % 3) != 0;  // also exercise NAND(a, a)
        const Netlist nl = gttest::randomDag(seed, o);
        const TopoResult t = topoSort(nl);
        CAPTURE(seed);
        CHECK_FALSE(t.hasCycle());
        checkValidOrder(nl, t);
        // level[v] = 1 + max level of fanin, 0 for startpoints.
        for (NodeId v = 0; v < static_cast<NodeId>(nl.size()); ++v) {
            int expect = 0;
            for (NodeId u : nl.gates[v].fanin) expect = std::max(expect, t.level[u] + 1);
            CHECK(t.level[v] == expect);
        }
    }
}

TEST_CASE("bench/loop file is rejected as a loop") {
#ifdef GATETIME_SOURCE_DIR
    const Netlist nl = parseBenchFile(std::string(GATETIME_SOURCE_DIR) + "/tests/data/loop.bench");
    const TopoResult t = topoSort(nl);
    CHECK(t.hasCycle());
    CHECK_FALSE(t.cycle.empty());
#endif
}
