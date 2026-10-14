#include "gatetime/sim.hpp"

#include <algorithm>
#include <bitset>

namespace gt {

std::uint64_t SplitMix64::next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

Simulator::Simulator(const Netlist& nl, const std::vector<NodeId>& order)
    : n_(nl.size()), pos_(nl.size()), state_(nl.flops.size(), 0) {
    if (order.size() != n_) {
        throw std::invalid_argument("Simulator: topological order does not cover the netlist "
                                    "(combinational loop?)");
    }
    for (std::size_t p = 0; p < n_; ++p) pos_[order[p]] = static_cast<std::uint32_t>(p);

    std::vector<int> flopOf(n_, -1);
    for (std::size_t i = 0; i < nl.flops.size(); ++i) flopOf[nl.flops[i].q] = static_cast<int>(i);

    // Compile: one opcode and one fanin list per position, in evaluation order. The
    // evaluation loop trusts fanin counts, so they are validated here once; a hand-built
    // netlist cannot send it out of bounds.
    op_.resize(n_);
    faninBegin_.reserve(n_ + 1);
    faninBegin_.push_back(0);
    std::size_t edges = 0;
    for (const Gate& g : nl.gates) edges += g.fanin.size();
    faninPos_.reserve(edges);

    for (std::size_t p = 0; p < n_; ++p) {
        const NodeId u = order[p];
        const Gate& g = nl.gates[u];
        const std::size_t k = g.fanin.size();

        bool ok = true;
        Op op = Op::In;
        switch (g.type) {
            case GateType::Input: op = Op::In; ok = k == 0; break;
            case GateType::DffQ:  op = Op::Q; ok = k == 0 && flopOf[u] >= 0; break;  // Q needs its flop record
            case GateType::Buff:  op = Op::Buff; ok = k == 1; break;
            case GateType::Not:   op = Op::Not; ok = k == 1; break;
            case GateType::And:   op = Op::And; ok = k >= 1; break;
            case GateType::Nand:  op = Op::Nand; ok = k >= 1; break;
            case GateType::Or:    op = Op::Or; ok = k >= 1; break;
            case GateType::Nor:   op = Op::Nor; ok = k >= 1; break;
            case GateType::Xor:   op = Op::Xor; ok = k >= 1; break;
            case GateType::Xnor:  op = Op::Xnor; ok = k >= 1; break;
        }
        if (!ok) {
            throw std::invalid_argument("Simulator: node '" + g.name + "' (" + gateTypeName(g.type) +
                                        ") has an invalid fanin count (" + std::to_string(k) + ")");
        }
        op_[p] = op;
        for (NodeId f : g.fanin) {
            // A fanin must come earlier in the order, or the order is not topological.
            if (pos_[f] >= p) throw std::invalid_argument("Simulator: order is not topological");
            faninPos_.push_back(pos_[f]);
        }
        faninBegin_.push_back(static_cast<std::uint32_t>(faninPos_.size()));
    }

    inputPos_.reserve(nl.inputs.size());
    for (NodeId id : nl.inputs) inputPos_.push_back(pos_[id]);
    qPos_.reserve(nl.flops.size());
    dPos_.reserve(nl.flops.size());
    for (const Flop& f : nl.flops) {
        qPos_.push_back(pos_[f.q]);
        dPos_.push_back(pos_[f.d]);
    }
    val_.assign(n_, 0);
}

void Simulator::reset() { std::fill(state_.begin(), state_.end(), Word{0}); }

void Simulator::step(const std::vector<Word>& inputWords) {
    if (inputWords.size() != inputPos_.size()) {
        throw std::invalid_argument("Simulator::run: expected " + std::to_string(inputPos_.size()) +
                                    " input words, got " + std::to_string(inputWords.size()));
    }
    Word* const val = val_.data();
    for (std::size_t i = 0; i < inputPos_.size(); ++i) val[inputPos_[i]] = inputWords[i];
    for (std::size_t i = 0; i < qPos_.size(); ++i) val[qPos_[i]] = state_[i];

    const std::uint32_t* const fan = faninPos_.data();
    const std::uint32_t* const begin = faninBegin_.data();
    for (std::size_t p = 0; p < n_; ++p) {
        const std::uint32_t b = begin[p], e = begin[p + 1];
        Word v = 0;
        switch (op_[p]) {
            case Op::In:
            case Op::Q:
                continue;  // already loaded above
            case Op::Buff:
                v = val[fan[b]];
                break;
            case Op::Not:
                v = ~val[fan[b]];
                break;
            case Op::And:
            case Op::Nand:
                v = val[fan[b]];
                for (std::uint32_t k = b + 1; k < e; ++k) v &= val[fan[k]];
                if (op_[p] == Op::Nand) v = ~v;
                break;
            case Op::Or:
            case Op::Nor:
                v = val[fan[b]];
                for (std::uint32_t k = b + 1; k < e; ++k) v |= val[fan[k]];
                if (op_[p] == Op::Nor) v = ~v;
                break;
            case Op::Xor:
            case Op::Xnor:
                v = val[fan[b]];
                for (std::uint32_t k = b + 1; k < e; ++k) v ^= val[fan[k]];
                if (op_[p] == Op::Xnor) v = ~v;
                break;
        }
        val[p] = v;
    }
    // Clock edge: every flop latches its D net.
    for (std::size_t i = 0; i < dPos_.size(); ++i) state_[i] = val[dPos_[i]];
}

const std::vector<Word>& Simulator::run(const std::vector<Word>& inputWords) {
    step(inputWords);
    byNode_.resize(n_);
    for (std::size_t id = 0; id < n_; ++id) byNode_[id] = val_[pos_[id]];
    return byNode_;
}

std::uint64_t exhaustiveBatches(std::size_t numInputs) {
    if (numInputs <= 6) return 1;
    if (numInputs - 6 >= 63) throw std::invalid_argument("exhaustiveBatches: too many inputs");
    return std::uint64_t{1} << (numInputs - 6);
}

std::vector<Word> exhaustiveInputs(std::size_t numInputs, std::uint64_t batch) {
    static const Word kLow[6] = {
        0xAAAAAAAAAAAAAAAAULL, 0xCCCCCCCCCCCCCCCCULL, 0xF0F0F0F0F0F0F0F0ULL,
        0xFF00FF00FF00FF00ULL, 0xFFFF0000FFFF0000ULL, 0xFFFFFFFF00000000ULL,
    };
    std::vector<Word> words(numInputs);
    for (std::size_t k = 0; k < numInputs; ++k) {
        if (k < 6) {
            words[k] = kLow[k];
        } else {
            words[k] = ((batch >> (k - 6)) & 1) ? ~Word{0} : Word{0};
        }
    }
    return words;
}

std::vector<Word> randomInputs(std::size_t numInputs, SplitMix64& rng) {
    std::vector<Word> words(numInputs);
    for (Word& w : words) w = rng.next();
    return words;
}

namespace {

// '0'/'1' per node for one bit lane of the simulator's current values.
std::string bitsOf(const Simulator& sim, const std::vector<NodeId>& nodes, unsigned bit) {
    std::string s;
    s.reserve(nodes.size());
    for (NodeId id : nodes) s.push_back(((sim.value(id) >> bit) & 1) ? '1' : '0');
    return s;
}

std::string inputBits(const std::vector<Word>& in, unsigned bit) {
    std::string s;
    s.reserve(in.size());
    for (Word w : in) s.push_back(((w >> bit) & 1) ? '1' : '0');
    return s;
}

}  // namespace

RandomSimResult simulateRandom(const Netlist& nl, const std::vector<NodeId>& order, std::uint64_t count,
                               std::uint64_t seed, std::size_t sampleCount, std::uint64_t cycles) {
    if (cycles == 0) throw std::invalid_argument("simulateRandom: cycles must be >= 1");
    Simulator sim(nl, order);
    SplitMix64 rng(seed);
    RandomSimResult r;
    r.cycles = cycles;
    r.ones.assign(nl.outputs.size(), 0);

    for (std::uint64_t done = 0; done < count; done += 64) {
        const std::uint64_t remaining = count - done;
        const unsigned valid = remaining >= 64 ? 64u : static_cast<unsigned>(remaining);
        const Word mask = valid == 64 ? ~Word{0} : ((Word{1} << valid) - 1);
        sim.reset();  // every batch is 64 fresh sequences from the reset state

        for (std::uint64_t c = 0; c < cycles; ++c) {
            const std::vector<Word> in = randomInputs(nl.inputs.size(), rng);
            sim.step(in);

            for (std::size_t o = 0; o < nl.outputs.size(); ++o) {
                r.ones[o] += std::bitset<64>(sim.value(nl.outputs[o]) & mask).count();
            }

            if (r.sampleInputs.size() < sampleCount) {
                if (cycles == 1) {
                    for (unsigned b = 0; b < valid && r.sampleInputs.size() < sampleCount; ++b) {
                        r.sampleInputs.push_back(inputBits(in, b));
                        r.sampleOutputs.push_back(bitsOf(sim, nl.outputs, b));
                    }
                } else if (done == 0) {  // first sequence, successive cycles
                    r.sampleInputs.push_back(inputBits(in, 0));
                    r.sampleOutputs.push_back(bitsOf(sim, nl.outputs, 0));
                }
            }
        }
        r.vectors += valid;
    }
    return r;
}

std::vector<std::string> parseVectors(std::istream& in, std::size_t numInputs, const std::string& sourceName) {
    std::vector<std::string> vectors;
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        if (const auto hash = raw.find('#'); hash != std::string::npos) raw.resize(hash);
        const auto first = raw.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const auto last = raw.find_last_not_of(" \t\r\n");
        const std::string v = raw.substr(first, last - first + 1);

        auto fail = [&](const std::string& msg) {
            throw VectorFileError(sourceName + ":" + std::to_string(lineNo) + ": " + msg);
        };
        if (v.find_first_not_of("01") != std::string::npos) fail("vector '" + v + "' may only contain 0 and 1");
        if (v.size() != numInputs) {
            fail("vector has " + std::to_string(v.size()) + " bits but the netlist has " +
                 std::to_string(numInputs) + " inputs");
        }
        vectors.push_back(v);
    }
    return vectors;
}

std::vector<std::string> simulateVectors(const Netlist& nl, const std::vector<NodeId>& order,
                                         const std::vector<std::string>& vectors) {
    Simulator sim(nl, order);
    std::vector<std::string> out;
    out.reserve(vectors.size());

    auto check = [&](const std::string& v) {
        if (v.size() != nl.inputs.size()) {
            throw std::invalid_argument("simulateVectors: vector length does not match input count");
        }
    };

    if (!nl.flops.empty()) {
        // One sequence: each vector is the next clock cycle, state carried between them.
        std::vector<Word> in(nl.inputs.size());
        for (const std::string& v : vectors) {
            check(v);
            for (std::size_t i = 0; i < v.size(); ++i) in[i] = v[i] == '1' ? 1 : 0;
            sim.step(in);
            out.push_back(bitsOf(sim, nl.outputs, 0));
        }
        return out;
    }

    // Combinational: independent vectors, 64 per word.
    for (std::size_t base = 0; base < vectors.size(); base += 64) {
        const std::size_t n = std::min<std::size_t>(64, vectors.size() - base);
        std::vector<Word> in(nl.inputs.size(), 0);
        for (std::size_t b = 0; b < n; ++b) {
            const std::string& v = vectors[base + b];
            check(v);
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '1') in[i] |= Word{1} << b;
            }
        }
        sim.step(in);
        for (std::size_t b = 0; b < n; ++b) out.push_back(bitsOf(sim, nl.outputs, static_cast<unsigned>(b)));
    }
    return out;
}

}  // namespace gt
