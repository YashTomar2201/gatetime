#pragma once
#include <cstddef>
#include <cstdint>
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gatetime/netlist.hpp"

namespace gt {

// One bit per test vector: a Word holds the value of one net across 64 vectors.
using Word = std::uint64_t;

// Small deterministic generator, identical on every platform.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next();

private:
    std::uint64_t state_;
};

// Bit-parallel logic simulator: evaluates the whole netlist for 64 input vectors
// per call, one machine word per net, in topological order.
//
// Sequential circuits: every flop's Q starts at 0 after construction or reset(). Each
// run() is one clock cycle: the combinational logic is evaluated from the current
// state and inputs, then the clock edge latches every flop's D into its Q. The 64 bit
// lanes are 64 independent machines, so one run() advances 64 sequences by one cycle.
//
// The constructor compiles the netlist once into a flat program in topological
// order (opcode array + compressed fanin lists in position space), so evaluating it
// walks memory sequentially instead of chasing a pointer per gate. That matters
// because one Simulator evaluates the same netlist many times.
class Simulator {
public:
    // `order` must be a topological order covering every node (acyclic netlist).
    // Throws std::invalid_argument for a loop, or a gate whose fanin count does not fit
    // its type (NOT/BUFF need exactly one input, other gates at least one).
    Simulator(const Netlist& nl, const std::vector<NodeId>& order);

    // Evaluates one clock cycle. inputWords[i] is the value of nl.inputs[i] for 64
    // vectors (bit j = vector j). Read results with value() afterwards; they are the
    // values *before* the clock edge. Cheapest way to advance the simulation.
    void step(const std::vector<Word>& inputWords);

    // Value of a net after the last step()/run().
    Word value(NodeId id) const { return val_[pos_[id]]; }

    // Same as step(), then returns one Word per node indexed by NodeId (a copy in
    // netlist order; valid until the next run()). Convenient for tests and small circuits.
    const std::vector<Word>& run(const std::vector<Word>& inputWords);

    // Puts every flop back to 0.
    void reset();

private:
    enum class Op : std::uint8_t { In, Q, Buff, Not, And, Nand, Or, Nor, Xor, Xnor };

    std::size_t                n_;
    std::vector<std::uint32_t> pos_;       // NodeId -> position in topological order
    std::vector<Op>            op_;        // per position
    std::vector<std::uint32_t> faninBegin_;  // per position, n + 1 entries
    std::vector<std::uint32_t> faninPos_;  // fanin positions, concatenated
    std::vector<std::uint32_t> inputPos_;  // per primary input: its position
    std::vector<std::uint32_t> qPos_, dPos_;  // per flop: position of Q and of D
    std::vector<Word>          val_;       // per position
    std::vector<Word>          state_;     // one Word per flop
    std::vector<Word>          byNode_;    // run()'s NodeId-indexed copy
};

// Input words that enumerate input assignments in binary: across vector number
// v = batch * 64 + bit, input k takes the value of bit k of v. With <= 6 inputs a
// single batch (batch 0) covers all 2^n assignments in its low 2^n bits.
std::vector<Word> exhaustiveInputs(std::size_t numInputs, std::uint64_t batch);
std::uint64_t exhaustiveBatches(std::size_t numInputs);  // 1 for <= 6 inputs, else 2^(n-6)

// Input words filled from the generator, input 0 first. The stream is part of the
// contract: same seed, same netlist input count -> same vectors.
std::vector<Word> randomInputs(std::size_t numInputs, SplitMix64& rng);

struct RandomSimResult {
    std::uint64_t              vectors = 0;  // independent sequences simulated
    std::uint64_t              cycles = 1;   // clock cycles per sequence (1 for combinational use)
    std::vector<std::uint64_t> ones;         // per output (nl.outputs order): evaluations where it was 1,
                                             // out of vectors * cycles
    std::vector<std::string>   sampleInputs;   // first few evaluations as '0'/'1' strings (nl.inputs order)
    std::vector<std::string>   sampleOutputs;  // matching output strings (nl.outputs order)
};

// Simulates `count` random sequences of `cycles` cycles each from the reset state
// (rounded up internally to whole words, the extra bits are ignored). With cycles == 1
// the samples are the first `sampleCount` vectors; with more, they are the first
// sequence's first `sampleCount` cycles, so state evolution is visible.
RandomSimResult simulateRandom(const Netlist& nl, const std::vector<NodeId>& order, std::uint64_t count,
                               std::uint64_t seed, std::size_t sampleCount = 8, std::uint64_t cycles = 1);

// Thrown for a malformed vectors file; what() is "<source>:<line>: <message>".
class VectorFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One '0'/'1' string of exactly `numInputs` characters per line (nl.inputs order).
// `#` starts a comment; blank lines are skipped.
std::vector<std::string> parseVectors(std::istream& in, std::size_t numInputs,
                                      const std::string& sourceName = "<stream>");

// Output string ('0'/'1' per nl.outputs entry) for each input vector string. For a
// combinational netlist the vectors are independent. For a sequential one they are
// consecutive clock cycles of a single sequence starting from the reset state.
std::vector<std::string> simulateVectors(const Netlist& nl, const std::vector<NodeId>& order,
                                         const std::vector<std::string>& vectors);

}  // namespace gt
