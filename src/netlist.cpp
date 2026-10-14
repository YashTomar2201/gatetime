#include "gatetime/netlist.hpp"

#include <functional>
#include <stdexcept>

namespace gt {

namespace {

std::uint32_t hashName(std::string_view s) {
    const std::size_t h = std::hash<std::string_view>{}(s);
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

}  // namespace

void Netlist::growTo(std::size_t capacity) {
    std::size_t cap = 16;
    while (cap < capacity) cap <<= 1;
    if (cap <= slots_.size()) return;
    std::vector<Slot> next(cap);
    const std::size_t mask = cap - 1;
    for (const Slot& s : slots_) {
        if (s.id1 == 0) continue;
        std::size_t i = s.hash & mask;
        while (next[i].id1 != 0) i = (i + 1) & mask;
        next[i] = s;
    }
    slots_.swap(next);
}

void Netlist::reserveNodes(std::size_t n) {
    gates.reserve(n);
    growTo(2 * n);  // load factor <= 1/2
}

NodeId Netlist::getOrCreate(std::string_view name) {
    if (gates.size() >= 0x7FFFFFFFu) throw std::length_error("Netlist: too many nodes");
    if (2 * (used_ + 1) > slots_.size()) growTo(slots_.empty() ? 16 : 2 * slots_.size());

    const std::uint32_t h = hashName(name);
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = h & mask;
    while (slots_[i].id1 != 0) {
        if (slots_[i].hash == h && gates[slots_[i].id1 - 1].name == name) {
            return static_cast<NodeId>(slots_[i].id1 - 1);
        }
        i = (i + 1) & mask;
    }

    const NodeId id = static_cast<NodeId>(gates.size());
    gates.emplace_back();
    gates.back().name.assign(name.data(), name.size());
    slots_[i] = Slot{h, static_cast<std::uint32_t>(id) + 1};
    ++used_;
    return id;
}

NodeId Netlist::find(std::string_view name) const {
    if (slots_.empty()) return -1;
    const std::uint32_t h = hashName(name);
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t i = h & mask; slots_[i].id1 != 0; i = (i + 1) & mask) {
        if (slots_[i].hash == h && gates[slots_[i].id1 - 1].name == name) {
            return static_cast<NodeId>(slots_[i].id1 - 1);
        }
    }
    return -1;
}

std::vector<NodeId> Netlist::endpoints() const {
    std::vector<NodeId> result;
    result.reserve(outputs.size() + flops.size());
    std::vector<char> seen(gates.size(), 0);
    for (NodeId o : outputs) {
        if (!seen[o]) {
            seen[o] = 1;
            result.push_back(o);
        }
    }
    for (const Flop& f : flops) {
        if (!seen[f.d]) {
            seen[f.d] = 1;
            result.push_back(f.d);
        }
    }
    return result;
}

const char* gateTypeName(GateType type) {
    switch (type) {
        case GateType::Input: return "INPUT";
        case GateType::Buff:  return "BUFF";
        case GateType::Not:   return "NOT";
        case GateType::And:   return "AND";
        case GateType::Nand:  return "NAND";
        case GateType::Or:    return "OR";
        case GateType::Nor:   return "NOR";
        case GateType::Xor:   return "XOR";
        case GateType::Xnor:  return "XNOR";
        case GateType::DffQ:  return "DFF";
    }
    return "?";
}

bool gateTypeFromString(std::string_view word, GateType& out) {
    static const struct { const char* name; GateType type; } table[] = {
        {"AND", GateType::And},   {"NAND", GateType::Nand}, {"OR", GateType::Or},
        {"NOR", GateType::Nor},   {"XOR", GateType::Xor},   {"XNOR", GateType::Xnor},
        {"NOT", GateType::Not},   {"BUFF", GateType::Buff}, {"BUF", GateType::Buff},
    };
    for (const auto& e : table) {
        if (equalsNoCase(word, e.name)) {
            out = e.type;
            return true;
        }
    }
    return false;
}

}  // namespace gt
