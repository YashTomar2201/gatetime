#include <algorithm>
#include <string>
#include <vector>

#include "gt_doctest.hpp"
#include "gatetime/parser_bench.hpp"

using namespace gt;

namespace {

const char* kC17 =
    "# c17\n"
    "INPUT(1)\nINPUT(2)\nINPUT(3)\nINPUT(6)\nINPUT(7)\n"
    "OUTPUT(22)\nOUTPUT(23)\n"
    "10 = NAND(1, 3)\n11 = NAND(3, 6)\n16 = NAND(2, 11)\n"
    "19 = NAND(11, 7)\n22 = NAND(10, 16)\n23 = NAND(16, 19)\n";

std::vector<std::string> names(const Netlist& nl, const std::vector<NodeId>& ids) {
    std::vector<std::string> out;
    for (NodeId id : ids) out.push_back(nl.gates[id].name);
    return out;
}
using V = std::vector<std::string>;

std::vector<std::string> faninOf(const Netlist& nl, const std::string& n) {
    return names(nl, nl.gates[nl.find(n)].fanin);
}
std::vector<std::string> fanoutOf(const Netlist& nl, const std::string& n) {
    return names(nl, nl.gates[nl.find(n)].fanout);
}

// Returns the line number reported for `text`, or -1 if it parsed fine.
int errorLine(const std::string& text, std::string* message = nullptr) {
    try {
        parseBenchString(text);
    } catch (const ParseError& e) {
        if (message) *message = e.what();
        return e.line();
    }
    return -1;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("c17 parses to the right counts") {
    const Netlist nl = parseBenchString(kC17);
    CHECK(nl.size() == 11);
    CHECK(nl.inputs.size() == 5);
    CHECK(nl.outputs.size() == 2);
    CHECK(nl.gateCount() == 6);
    CHECK(names(nl, nl.inputs) == V{"1", "2", "3", "6", "7"});
    CHECK(names(nl, nl.outputs) == V{"22", "23"});
}

TEST_CASE("c17 fanin and fanout lists are correct") {
    const Netlist nl = parseBenchString(kC17);
    CHECK(faninOf(nl, "10") == V{"1", "3"});
    CHECK(faninOf(nl, "11") == V{"3", "6"});
    CHECK(faninOf(nl, "16") == V{"2", "11"});
    CHECK(faninOf(nl, "19") == V{"11", "7"});
    CHECK(faninOf(nl, "22") == V{"10", "16"});
    CHECK(faninOf(nl, "23") == V{"16", "19"});
    CHECK(faninOf(nl, "1").empty());

    CHECK(fanoutOf(nl, "3") == V{"10", "11"});
    CHECK(fanoutOf(nl, "11") == V{"16", "19"});
    CHECK(fanoutOf(nl, "16") == V{"22", "23"});
    CHECK(fanoutOf(nl, "22").empty());
    CHECK(fanoutOf(nl, "23").empty());

    for (const Gate& g : nl.gates) {
        if (g.type != GateType::Input) CHECK(g.type == GateType::Nand);
    }
    for (NodeId id : nl.inputs) CHECK(nl.gates[id].type == GateType::Input);
}

TEST_CASE("fanin and fanout are mutually consistent") {
    const Netlist nl = parseBenchString(kC17);
    for (NodeId v = 0; v < static_cast<NodeId>(nl.size()); ++v) {
        for (NodeId u : nl.gates[v].fanin) {
            const auto& fo = nl.gates[u].fanout;
            CHECK(std::find(fo.begin(), fo.end(), v) != fo.end());
        }
    }
}

TEST_CASE("forward references are resolved") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nINPUT(b)\nOUTPUT(z)\n"
        "z = AND(y, a)\n"   // y is defined on the next line
        "y = NOT(b)\n");
    CHECK(nl.size() == 4);
    CHECK(faninOf(nl, "z") == V{"y", "a"});
    CHECK(nl.gates[nl.find("y")].type == GateType::Not);
    CHECK(fanoutOf(nl, "y") == V{"z"});
}

TEST_CASE("OUTPUT may be declared before the gate that drives it") {
    const Netlist nl = parseBenchString("OUTPUT(z)\nINPUT(a)\nz = BUFF(a)\n");
    CHECK(names(nl, nl.outputs) == V{"z"});
    CHECK(nl.gates[nl.find("z")].type == GateType::Buff);
}

TEST_CASE("multi-input gates keep every input") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nINPUT(b)\nINPUT(c)\nINPUT(d)\nOUTPUT(z)\n"
        "z = NAND(a, b, c, d)\n");
    CHECK(faninOf(nl, "z") == V{"a", "b", "c", "d"});
    CHECK(fanoutOf(nl, "d") == V{"z"});
}

TEST_CASE("comments, blank lines, odd whitespace, case and CRLF are tolerated") {
    const Netlist nl = parseBenchString(
        "# header comment\r\n"
        "\r\n"
        "   input( a )   # trailing comment\r\n"
        "INPUT(b)\r\n"
        "\tOUTPUT (z)\r\n"
        "z=nand(a,b)\r\n"
        "   \r\n");
    CHECK(nl.size() == 3);
    CHECK(names(nl, nl.inputs) == V{"a", "b"});
    CHECK(faninOf(nl, "z") == V{"a", "b"});
    CHECK(nl.gates[nl.find("z")].type == GateType::Nand);

    const Netlist spaced = parseBenchString("INPUT(a)\nINPUT(b)\nOUTPUT(z)\nz   =   NOR  (  a ,\tb  )\n");
    CHECK(spaced.gates[spaced.find("z")].type == GateType::Nor);
    CHECK(faninOf(spaced, "z") == V{"a", "b"});
}

TEST_CASE("alphanumeric net names and every gate keyword are accepted") {
    const Netlist nl = parseBenchString(
        "INPUT(G1)\nINPUT(G2)\nOUTPUT(G_out)\n"
        "n1 = AND(G1, G2)\nn2 = OR(G1, G2)\nn3 = XOR(n1, n2)\nn4 = XNOR(n1, n2)\n"
        "n5 = NOR(n3, n4)\nn6 = NOT(n5)\nG_out = BUFF(n6)\n");
    CHECK(nl.gates[nl.find("n1")].type == GateType::And);
    CHECK(nl.gates[nl.find("n2")].type == GateType::Or);
    CHECK(nl.gates[nl.find("n3")].type == GateType::Xor);
    CHECK(nl.gates[nl.find("n4")].type == GateType::Xnor);
    CHECK(nl.gates[nl.find("n5")].type == GateType::Nor);
    CHECK(nl.gates[nl.find("n6")].type == GateType::Not);
    CHECK(nl.gates[nl.find("G_out")].type == GateType::Buff);
}

TEST_CASE("duplicate fanin keeps both edges on both sides") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(z)\nz = NAND(a, a)\n");
    CHECK(faninOf(nl, "z") == V{"a", "a"});
    CHECK(fanoutOf(nl, "a") == V{"z", "z"});
}

TEST_CASE("a primary input can also be a primary output") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(a)\n");
    CHECK(nl.size() == 1);
    CHECK(names(nl, nl.inputs) == V{"a"});
    CHECK(names(nl, nl.outputs) == V{"a"});
}

TEST_CASE("an output net may also feed another gate") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(m)\nOUTPUT(z)\nm = NOT(a)\nz = BUFF(m)\n");
    CHECK(names(nl, nl.outputs) == V{"m", "z"});
    CHECK(fanoutOf(nl, "m") == V{"z"});
}

TEST_CASE("repeated OUTPUT declaration lists the net once") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(z)\nOUTPUT(z)\nz = NOT(a)\n");
    CHECK(nl.outputs.size() == 1);
}

TEST_CASE("empty input gives an empty netlist") {
    const Netlist nl = parseBenchString("# nothing here\n\n");
    CHECK(nl.size() == 0);
    CHECK(nl.inputs.empty());
    CHECK(nl.outputs.empty());
}

TEST_CASE("a self-loop parses (cycle detection is the topo pass's job)") {
    const Netlist nl = parseBenchString("INPUT(a)\nOUTPUT(x)\nx = AND(a, x)\n");
    CHECK(faninOf(nl, "x") == V{"a", "x"});
}

TEST_CASE("error: unknown gate type reports its line") {
    std::string msg;
    CHECK(errorLine("INPUT(a)\nOUTPUT(z)\nz = FROB(a)\n", &msg) == 3);
    CHECK(contains(msg, "unknown gate type 'FROB'"));
    CHECK(contains(msg, "line 3"));
}

TEST_CASE("error: missing parenthesis reports its line") {
    std::string msg;
    CHECK(errorLine("INPUT(a)\nz = NAND(a, a\n", &msg) == 2);
    CHECK(contains(msg, "missing ')'"));
    CHECK(errorLine("INPUT(a\n") == 1);
    CHECK(errorLine("INPUT(a)\nz = NAND a, a)\n") == 2);
    CHECK(errorLine("INPUT(a)\nz = NAND(a) extra\n") == 2);
}

TEST_CASE("error: undefined net reports the first line that used it") {
    std::string msg;
    CHECK(errorLine("INPUT(a)\nOUTPUT(z)\nz = AND(a, ghost)\ny = NOT(ghost)\n"
                    "w = BUFF(y)\n", &msg) == 3);
    CHECK(contains(msg, "'ghost'"));
    CHECK(contains(msg, "never driven"));
    // An OUTPUT of a net nobody drives is also undefined.
    CHECK(errorLine("INPUT(a)\nOUTPUT(nowhere)\n") == 2);
}

TEST_CASE("error: duplicate driver reports the second definition") {
    std::string msg;
    CHECK(errorLine("INPUT(a)\nOUTPUT(z)\nz = NOT(a)\nz = BUFF(a)\n", &msg) == 4);
    CHECK(contains(msg, "driven more than once"));
    CHECK(errorLine("INPUT(a)\nINPUT(a)\n") == 2);
    CHECK(errorLine("INPUT(a)\na = NOT(a)\n") == 2);
}

TEST_CASE("error: arity, empty args, bad statements and DFF") {
    CHECK(errorLine("INPUT(a)\nINPUT(b)\nz = NOT(a, b)\n") == 3);
    CHECK(errorLine("INPUT(a)\nz = NAND()\n") == 2);
    CHECK(errorLine("INPUT(a)\nz = NAND(a,)\n") == 2);
    CHECK(errorLine("INPUT(a)\nINPUT(b)\nTRISTATE(a)\n") == 3);
    CHECK(errorLine("INPUT(a, b)\n") == 1);
    CHECK(errorLine("= NOT(a)\n") == 1);
    CHECK(errorLine("INPUT(a)\nz = INPUT(a)\n") == 2);
    std::string msg;
    CHECK(errorLine("INPUT(a)\nINPUT(b)\nq = DFF(a, b)\n", &msg) == 3);
    CHECK(contains(msg, "DFF takes exactly one input"));
    CHECK(errorLine("INPUT(a)\nq = DFF()\n") == 2);
    CHECK(errorLine("INPUT(a)\nq = DFF(ghost)\n", &msg) == 2);  // D must be driven somewhere
    CHECK(contains(msg, "'ghost'"));
    CHECK(errorLine("INPUT(a)\nq = NOT(a)\nq = DFF(a)\n", &msg) == 3);  // Q driven twice
    CHECK(contains(msg, "driven more than once"));
}

TEST_CASE("DFF: Q is a startpoint, D is an endpoint, the graph stays acyclic") {
    // s27-like structure: a flop whose D depends on its own Q through logic.
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(z)\n"
        "q = DFF(d)\n"
        "d = NAND(a, q)\n"
        "z = NOT(q)\n");
    REQUIRE(nl.flops.size() == 1);
    const NodeId q = nl.find("q"), d = nl.find("d");
    CHECK(nl.flops[0].q == q);
    CHECK(nl.flops[0].d == d);
    CHECK(nl.gates[q].type == GateType::DffQ);
    CHECK(nl.gates[q].fanin.empty());              // cut: Q has no fanin
    CHECK(faninOf(nl, "d") == V{"a", "q"});
    CHECK(fanoutOf(nl, "q") == V{"d", "z"});
    CHECK(nl.inputs.size() == 1);                  // Q is not a primary input
    CHECK(nl.gateCount() == 3);                    // d, z, and the flop's Q
    CHECK(names(nl, nl.endpoints()) == V{"z", "d"});  // outputs first, then flop D nets
}

TEST_CASE("endpoints() lists each net once") {
    const Netlist nl = parseBenchString(
        "INPUT(a)\nOUTPUT(x)\nOUTPUT(y)\n"
        "x = NOT(a)\ny = BUFF(a)\n"
        "q1 = DFF(x)\nq2 = DFF(x)\nq3 = DFF(a)\n");  // x is both a PO and the D of two flops
    CHECK(names(nl, nl.endpoints()) == V{"x", "y", "a"});
}

TEST_CASE("name index: growth, repeated lookups, misses and odd names") {
    Netlist nl;
    CHECK(nl.find("anything") == -1);  // empty netlist

    const int n = 50000;  // forces many table growths
    for (int i = 0; i < n; ++i) {
        CHECK(nl.getOrCreate("net" + std::to_string(i)) == i);
    }
    CHECK(nl.size() == static_cast<std::size_t>(n));
    for (int i = 0; i < n; i += 7) {
        CHECK(nl.getOrCreate("net" + std::to_string(i)) == i);  // repeat: same id, no new node
        CHECK(nl.find("net" + std::to_string(i)) == i);
        CHECK(nl.gates[i].name == "net" + std::to_string(i));
    }
    CHECK(nl.size() == static_cast<std::size_t>(n));
    CHECK(nl.find("net") == -1);
    CHECK(nl.find("net50000") == -1);
    CHECK(nl.find("") == -1);

    // Names that are prefixes of each other, and long names beyond SSO.
    const std::string longName(200, 'x');
    const NodeId a = nl.getOrCreate("a"), aa = nl.getOrCreate("aa"), l = nl.getOrCreate(longName);
    CHECK(a != aa);
    CHECK(nl.find("a") == a);
    CHECK(nl.find("aa") == aa);
    CHECK(nl.find(longName) == l);
    CHECK(nl.find(longName.substr(1)) == -1);
    const NodeId empty = nl.getOrCreate("");  // the empty name is a legal key in the index itself
    CHECK(nl.find("") == empty);

    // A copied netlist has its own working index.
    Netlist copy = nl;
    CHECK(copy.find("aa") == aa);
    copy.getOrCreate("only-in-copy");
    CHECK(copy.find("only-in-copy") >= 0);
    CHECK(nl.find("only-in-copy") == -1);
}

TEST_CASE("reserveNodes does not change behaviour") {
    Netlist nl;
    nl.reserveNodes(1000);
    CHECK(nl.getOrCreate("x") == 0);
    CHECK(nl.getOrCreate("y") == 1);
    CHECK(nl.getOrCreate("x") == 0);
    CHECK(nl.find("y") == 1);
}

TEST_CASE("parseBenchFile reads bench/c17.bench") {
#ifdef GATETIME_SOURCE_DIR
    const Netlist nl = parseBenchFile(std::string(GATETIME_SOURCE_DIR) + "/bench/c17.bench");
    CHECK(nl.inputs.size() == 5);
    CHECK(nl.outputs.size() == 2);
    CHECK(nl.gateCount() == 6);
#endif
    CHECK_THROWS_AS(parseBenchFile("no/such/file.bench"), std::runtime_error);
}
