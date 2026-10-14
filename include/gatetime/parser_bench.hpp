#pragma once
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "gatetime/netlist.hpp"

namespace gt {

// Thrown for any malformed .bench content. what() is "line N: <message>".
class ParseError : public std::runtime_error {
public:
    ParseError(int line, const std::string& message)
        : std::runtime_error("line " + std::to_string(line) + ": " + message), line_(line) {}
    int line() const { return line_; }

private:
    int line_;
};

// ISCAS .bench: INPUT(x), OUTPUT(x), `net = TYPE(a, b, ...)` and, for sequential
// circuits (ISCAS'89), `q = DFF(d)`. Forward references are allowed; every
// referenced net must be driven by the end of the file.
Netlist parseBench(std::istream& in);
Netlist parseBenchString(const std::string& text);
Netlist parseBenchText(std::string_view text);  // the core: no copies, no streams

// Throws std::runtime_error if the file cannot be opened, ParseError on bad content.
Netlist parseBenchFile(const std::string& path);

}  // namespace gt
