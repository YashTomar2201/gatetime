#pragma once
// doctest plus a printer for GateType, so CHECK(a == b) can report values.
#include <ostream>

#include "doctest.h"
#include "gatetime/netlist.hpp"

namespace gt {
inline std::ostream& operator<<(std::ostream& os, GateType t) { return os << gateTypeName(t); }
}  // namespace gt
