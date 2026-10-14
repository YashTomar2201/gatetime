#include "gatetime/parser_bench.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace gt {

namespace {

using std::string;
using std::string_view;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

string_view trim(string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

// A name is any non-empty run without whitespace or the structural characters.
bool validName(string_view s) {
    static const std::array<bool, 256> bad = [] {
        std::array<bool, 256> t{};
        for (char c : string_view(" \t\r\n\v\f()=,#")) t[static_cast<unsigned char>(c)] = true;
        return t;
    }();
    if (s.empty()) return false;
    for (char c : s) {
        if (bad[static_cast<unsigned char>(c)]) return false;
    }
    return true;
}

struct Call {
    string_view              head;
    std::vector<string_view> args;
};

// Parses `HEAD ( a , b , ... )` into `call`, reusing its storage. On failure returns
// false and sets `err`.
bool parseCall(string_view text, Call& call, string& err) {
    const auto open = text.find('(');
    if (open == string_view::npos) {
        err = "expected '(' in '" + string(text) + "'";
        return false;
    }
    const auto close = text.rfind(')');
    if (close == string_view::npos || close < open) {
        err = "missing ')' in '" + string(text) + "'";
        return false;
    }
    if (!trim(text.substr(close + 1)).empty()) {
        err = "unexpected text after ')' in '" + string(text) + "'";
        return false;
    }
    call.head = trim(text.substr(0, open));
    if (!validName(call.head)) {
        err = "missing or malformed name before '(' in '" + string(text) + "'";
        return false;
    }

    string_view inner = text.substr(open + 1, close - open - 1);
    if (inner.find_first_of("()") != string_view::npos) {
        err = "unbalanced parentheses in '" + string(text) + "'";
        return false;
    }
    call.args.clear();
    if (trim(inner).empty()) return true;  // zero arguments; caller decides if that is legal

    size_t start = 0;
    while (true) {
        const auto comma = inner.find(',', start);
        const string_view piece =
            trim(inner.substr(start, comma == string_view::npos ? string_view::npos : comma - start));
        if (!validName(piece)) {
            err = "empty or malformed argument in '" + string(text) + "'";
            return false;
        }
        call.args.push_back(piece);
        if (comma == string_view::npos) break;
        start = comma + 1;
    }
    return true;
}

// Per-node bookkeeping the parser needs but Gate does not carry.
struct NodeInfo {
    int  firstUse = 0;      // line of the first mention
    bool defined = false;   // a driver (INPUT or gate output) has been seen
    bool isOutput = false;  // already listed in nl.outputs
};

}  // namespace

Netlist parseBenchText(std::string_view text) {
    Netlist nl;

    // One node per distinct net, at most one per line: reserving up front avoids
    // rehashing the name map and moving Gate objects as the vector grows.
    const std::size_t lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    nl.reserveNodes(lines);
    std::vector<NodeInfo> info;
    info.reserve(lines);

    // Resolves a name to a node and records bookkeeping on first sight.
    auto touch = [&](string_view name, int line) -> NodeId {
        const NodeId id = nl.getOrCreate(name);
        if (static_cast<size_t>(id) >= info.size()) info.push_back(NodeInfo{line, false, false});
        return id;
    };
    auto markDriven = [&](NodeId id, int line) {
        if (info[id].defined) {
            throw ParseError(line, "net '" + nl.gates[id].name + "' is driven more than once");
        }
        info[id].defined = true;
    };

    // Gates in the order their definitions appear. Fanout lists are built from this
    // after parsing, in one exactly-sized allocation per net, instead of growing
    // thousands of tiny vectors 1 -> 2 -> 4 while reading (that was ~40% of parse time).
    std::vector<NodeId> defOrder;
    defOrder.reserve(lines);

    Call call;
    string err;
    int lineNo = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == string_view::npos) end = text.size();
        string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;

        if (lineNo == 1 && line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
        if (const auto hash = line.find('#'); hash != string_view::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        const auto eq = line.find('=');

        if (eq == string_view::npos) {
            // INPUT(x) / OUTPUT(x)
            if (!parseCall(line, call, err)) throw ParseError(lineNo, err);
            const bool isInput = equalsNoCase(call.head, "INPUT");
            if (!isInput && !equalsNoCase(call.head, "OUTPUT")) {
                throw ParseError(lineNo, "unrecognised statement '" + string(line) +
                                             "' (expected INPUT(...), OUTPUT(...) or net = GATE(...))");
            }
            if (call.args.size() != 1) {
                throw ParseError(lineNo, string(isInput ? "INPUT" : "OUTPUT") + " takes exactly one net name");
            }
            const NodeId id = touch(call.args[0], lineNo);
            if (isInput) {
                markDriven(id, lineNo);
                nl.gates[id].type = GateType::Input;
                nl.inputs.push_back(id);
            } else if (!info[id].isOutput) {
                info[id].isOutput = true;
                nl.outputs.push_back(id);
            }
            continue;
        }

        // net = TYPE(a, b, ...)
        const string_view lhs = trim(line.substr(0, eq));
        const string_view rhs = trim(line.substr(eq + 1));
        if (!validName(lhs)) throw ParseError(lineNo, "missing or malformed net name before '='");
        if (!parseCall(rhs, call, err)) throw ParseError(lineNo, err);

        if (equalsNoCase(call.head, "DFF")) {
            // q = DFF(d): q becomes a startpoint with no fanin (the timing graph is cut at
            // the flop, which is what keeps sequential feedback from becoming a cycle) and
            // d is recorded as an endpoint. d must be driven somewhere; checked at the end.
            if (call.args.size() != 1) {
                throw ParseError(lineNo, "DFF takes exactly one input, got " + std::to_string(call.args.size()));
            }
            const NodeId q = touch(lhs, lineNo);
            markDriven(q, lineNo);
            nl.gates[q].type = GateType::DffQ;
            const NodeId d = touch(call.args[0], lineNo);
            nl.flops.push_back({q, d});
            continue;
        }
        GateType type;
        if (!gateTypeFromString(call.head, type)) {
            throw ParseError(lineNo, "unknown gate type '" + string(call.head) + "'");
        }
        if (call.args.empty()) {
            throw ParseError(lineNo, "gate '" + string(lhs) + "' has no inputs");
        }
        if ((type == GateType::Not || type == GateType::Buff) && call.args.size() != 1) {
            throw ParseError(lineNo, string(gateTypeName(type)) + " takes exactly one input, got " +
                                         std::to_string(call.args.size()));
        }

        const NodeId id = touch(lhs, lineNo);
        markDriven(id, lineNo);
        nl.gates[id].type = type;
        nl.gates[id].fanin.reserve(call.args.size());
        for (string_view arg : call.args) {
            const NodeId f = touch(arg, lineNo);  // never reallocates nl.gates (reserved), but index anyway
            nl.gates[id].fanin.push_back(f);
        }
        defOrder.push_back(id);
    }

    // Every referenced net must have a driver. Report the earliest offender.
    int worst = -1;
    for (std::size_t i = 0; i < info.size(); ++i) {
        if (!info[i].defined && (worst < 0 || info[i].firstUse < info[worst].firstUse)) {
            worst = static_cast<int>(i);
        }
    }
    if (worst >= 0) {
        throw ParseError(info[worst].firstUse, "net '" + nl.gates[worst].name +
                                                   "' is used but never driven (undefined net)");
    }

    // Fanout lists: count, reserve exactly, then fill in definition order.
    std::vector<unsigned> fanoutCount(nl.gates.size(), 0);
    for (NodeId v : defOrder) {
        for (NodeId f : nl.gates[v].fanin) ++fanoutCount[f];
    }
    for (std::size_t i = 0; i < fanoutCount.size(); ++i) {
        if (fanoutCount[i]) nl.gates[i].fanout.reserve(fanoutCount[i]);
    }
    for (NodeId v : defOrder) {
        for (NodeId f : nl.gates[v].fanin) nl.gates[f].fanout.push_back(v);
    }
    return nl;
}

Netlist parseBench(std::istream& in) {
    std::ostringstream buf;
    buf << in.rdbuf();
    return parseBenchText(buf.str());
}

Netlist parseBenchString(const std::string& text) { return parseBenchText(text); }

Netlist parseBenchFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::string text;
    if (std::fseek(f, 0, SEEK_END) == 0) {  // size the buffer once instead of growing it
        const long size = std::ftell(f);
        if (size > 0) text.reserve(static_cast<std::size_t>(size));
        std::rewind(f);
    }
    char chunk[1 << 16];
    std::size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) text.append(chunk, n);
    std::fclose(f);
    return parseBenchText(text);
}

}  // namespace gt
