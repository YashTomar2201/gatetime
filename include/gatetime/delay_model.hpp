#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "gatetime/netlist.hpp"

namespace gt {

// Anything that can say how long a node's gate takes. timing.cpp only sees this
// interface, so models can be swapped without touching the timing engine.
class DelayModel {
public:
    virtual ~DelayModel() = default;

    // Propagation delay through `id`. For a flop's Q node this is the clock-to-Q delay;
    // primary inputs have delay 0.
    virtual double delay(const Netlist& nl, NodeId id) const = 0;

    // Setup time every flop's D pin needs before the clock edge. 0 by default.
    virtual double setupTime() const { return 0.0; }

    virtual std::string name() const = 0;
};

// Every combinational gate takes 1; primary inputs and flop outputs take 0, as does
// flop setup. Used for hand-checks.
class UnitDelay : public DelayModel {
public:
    double delay(const Netlist& nl, NodeId id) const override;
    std::string name() const override { return "unit"; }
};

// Decorator: another model's delays, except for nodes whose delay has been set
// explicitly. Used for what-if (ECO) experiments and to cross-check incremental timing.
class OverrideDelay : public DelayModel {
public:
    explicit OverrideDelay(const DelayModel& base) : base_(&base) {}
    void set(NodeId id, double delay) { overrides_[id] = delay; }

    double delay(const Netlist& nl, NodeId id) const override;
    double setupTime() const override { return base_->setupTime(); }
    std::string name() const override { return base_->name() + " + overrides"; }

private:
    const DelayModel*                  base_;
    std::unordered_map<NodeId, double> overrides_;
};

// Thrown for a malformed delay file. what() is "<source>:<line>: <message>".
class DelayFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// delay(n) = table[type(n)] + loadFactor * fanout(n).
//
// Starts from built-in defaults; a delay file overrides entries. The load term models
// that a gate driving more loads is slower; it is 0 unless set, which gives the plain
// per-type table.
class TableDelay : public DelayModel {
public:
    TableDelay();  // built-in defaults

    // File format: one `<KEY> <value>` per line, `#` starts a comment. KEY is a gate type
    // (AND NAND OR NOR XOR XNOR NOT BUFF), DFF (clock-to-Q), SETUP (flop setup time) or
    // LOAD (extra delay per fanout). Values must be finite and >= 0.
    // Throws DelayFileError / std::runtime_error (cannot open).
    void loadFile(const std::string& path);
    void loadString(const std::string& text, const std::string& sourceName = "<string>");

    void set(GateType type, double value) { table_[static_cast<std::size_t>(type)] = value; }
    double get(GateType type) const { return table_[static_cast<std::size_t>(type)]; }

    void setSetupTime(double v) { setup_ = v; }
    void setLoadFactor(double v) { load_ = v; }
    double loadFactor() const { return load_; }

    double delay(const Netlist& nl, NodeId id) const override;
    double setupTime() const override { return setup_; }
    std::string name() const override;

private:
    std::array<double, kGateTypeCount> table_{};  // indexed by GateType
    double setup_ = 0.2;
    double load_ = 0.0;
    std::string source_ = "table (built-in defaults)";
};

}  // namespace gt
