#include "gatetime/delay_model.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace gt {

double UnitDelay::delay(const Netlist& nl, NodeId id) const {
    return nl.gates[id].fanin.empty() ? 0.0 : 1.0;
}

double OverrideDelay::delay(const Netlist& nl, NodeId id) const {
    const auto it = overrides_.find(id);
    return it != overrides_.end() ? it->second : base_->delay(nl, id);
}

TableDelay::TableDelay() {
    table_.fill(0.0);
    set(GateType::Not, 0.5);
    set(GateType::Buff, 0.6);
    set(GateType::Nand, 1.0);
    set(GateType::Nor, 1.1);
    set(GateType::And, 1.4);
    set(GateType::Or, 1.5);
    set(GateType::Xor, 2.0);
    set(GateType::Xnor, 2.0);
    set(GateType::DffQ, 0.8);  // clock-to-Q
}

double TableDelay::delay(const Netlist& nl, NodeId id) const {
    const Gate& g = nl.gates[id];
    if (g.type == GateType::Input) return 0.0;  // primary inputs (and anything undriven)
    return get(g.type) + load_ * static_cast<double>(g.fanout.size());
}

std::string TableDelay::name() const {
    if (load_ == 0.0) return source_;
    char buf[64];
    std::snprintf(buf, sizeof buf, " + %g per fanout", load_);
    return source_ + buf;
}

void TableDelay::loadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open delay file: " + path);
    std::ostringstream buf;
    buf << in.rdbuf();
    loadString(buf.str(), path);
}

void TableDelay::loadString(const std::string& text, const std::string& sourceName) {
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    auto fail = [&](const std::string& msg) {
        throw DelayFileError(sourceName + ":" + std::to_string(lineNo) + ": " + msg);
    };

    // Parse into copies so a bad file leaves this model untouched.
    auto table = table_;
    double setup = setup_, load = load_;

    while (std::getline(in, raw)) {
        ++lineNo;
        if (const auto hash = raw.find('#'); hash != std::string::npos) raw.resize(hash);
        std::istringstream ls(raw);
        std::string key, valueWord, extra;
        if (!(ls >> key)) continue;  // blank line
        if (!(ls >> valueWord)) fail("expected '<KEY> <delay>', got '" + key + "'");
        if (ls >> extra) fail("unexpected text '" + extra + "' after the delay value");

        std::size_t used = 0;
        double value = 0.0;
        try {
            value = std::stod(valueWord, &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used != valueWord.size()) fail("'" + valueWord + "' is not a number");
        if (!std::isfinite(value) || value < 0.0) {
            fail("delay must be a finite number >= 0, got '" + valueWord + "'");
        }

        GateType type;
        if (equalsNoCase(key, "SETUP")) {
            setup = value;
        } else if (equalsNoCase(key, "LOAD")) {
            load = value;
        } else if (equalsNoCase(key, "DFF")) {
            table[static_cast<std::size_t>(GateType::DffQ)] = value;
        } else if (gateTypeFromString(key, type)) {
            table[static_cast<std::size_t>(type)] = value;
        } else {
            fail("unknown gate type '" + key + "'");
        }
    }
    table_ = table;
    setup_ = setup;
    load_ = load;
    source_ = "table (" + sourceName + ")";
}

}  // namespace gt
