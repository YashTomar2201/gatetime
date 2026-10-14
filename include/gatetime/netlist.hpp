#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gt {

// DffQ is the output pin of a flip-flop. It has no fanin: the timing graph is cut at
// the flop, so Q starts a new path and the flop's D net ends one.
enum class GateType { Input, Buff, Not, And, Nand, Or, Nor, Xor, Xnor, DffQ };
inline constexpr std::size_t kGateTypeCount = 10;

using NodeId = int;

struct Gate {
    std::string         name;
    GateType            type = GateType::Input;
    std::vector<NodeId> fanin;   // drivers of this gate's inputs
    std::vector<NodeId> fanout;  // gates driven by this gate's output
};

// A flip-flop `q = DFF(d)`: q is a startpoint, d is an endpoint.
struct Flop {
    NodeId q;
    NodeId d;
};

struct Netlist {
    std::vector<Gate>   gates;
    std::vector<NodeId> inputs;   // primary inputs: startpoints
    std::vector<NodeId> outputs;  // primary outputs: endpoints
    std::vector<Flop>   flops;    // sequential elements

    // Returns the node for `name`, creating an (undriven) Input node on first
    // mention. This is what makes forward references in .bench files work.
    // Create nodes only through this method: it maintains the name index.
    NodeId getOrCreate(std::string_view name);

    // Returns the node for `name`, or -1 if it does not exist.
    NodeId find(std::string_view name) const;

    // Preallocates room for `n` nodes (gates and the name index) so that parsing a large
    // file does not rehash or move anything.
    void reserveNodes(std::size_t n);

    // Every timing endpoint exactly once: primary outputs first, then the D net of
    // each flop that is not already listed.
    std::vector<NodeId> endpoints() const;

    std::size_t size() const { return gates.size(); }
    std::size_t gateCount() const { return gates.size() - inputs.size(); }  // includes flop Q nodes

private:
    // Open-addressing hash table from name to node id (linear probing, load <= 1/2).
    // It stores ids, not names: names live once, in gates[id].name. A slot also keeps
    // the 32-bit hash so a probe only compares strings on a likely hit, and so growing
    // the table never rehashes strings. This replaced std::unordered_map<string, int>,
    // which cost one heap allocation and two pointer hops per net.
    struct Slot {
        std::uint32_t hash = 0;
        std::uint32_t id1 = 0;  // node id + 1; 0 = empty
    };
    std::vector<Slot> slots_;
    std::size_t       used_ = 0;

    void growTo(std::size_t capacity);
};

const char* gateTypeName(GateType type);

// ASCII case-insensitive equality; `upperRef` must already be upper case.
inline bool equalsNoCase(std::string_view s, std::string_view upperRef) {
    if (s.size() != upperRef.size()) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != upperRef[i]) return false;
    }
    return true;
}

// Case-insensitive lookup of a combinational gate keyword (AND, NAND, OR, NOR,
// XOR, XNOR, NOT, BUFF/BUF). Returns false if `word` is not one of them.
// (DFF is handled by the parser and the delay-file reader separately.)
bool gateTypeFromString(std::string_view word, GateType& out);

}  // namespace gt
