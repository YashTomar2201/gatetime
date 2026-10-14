// GateTime command-line front end. All real work lives in the gatetime_core library;
// this file only parses arguments, times the phases and maps failures to exit codes.
//
// Exit codes: 0 ok, 1 timing violation, 2 parse / file error, 3 combinational loop,
//             64 usage error.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gatetime/delay_model.hpp"
#include "gatetime/dot_export.hpp"
#include "gatetime/incremental.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/paths.hpp"
#include "gatetime/report.hpp"
#include "gatetime/sim.hpp"
#include "gatetime/timing.hpp"
#include "gatetime/topo.hpp"

namespace {

constexpr int kExitOk = 0, kExitViolation = 1, kExitError = 2, kExitLoop = 3, kExitUsage = 64;

const char* kUsage =
    "usage:\n"
    "  gatetime report <netlist.bench> [--period T] [--top N] [--dot OUT.dot] [--quiet]\n"
    "                                  [delay options]\n"
    "  gatetime eco    <netlist.bench> --set NET=DELAY [--set ...] [--period T] [delay options]\n"
    "  gatetime check  <netlist.bench>\n"
    "  gatetime stats  <netlist.bench>\n"
    "  gatetime sim    <netlist.bench> [--random COUNT] [--seed S] [--cycles C] | [--vectors FILE]\n"
    "                  (default: --random 64 --seed 1 --cycles 1)\n"
    "\n"
    "delay options (default: built-in per-gate-type table):\n"
    "  --unit-delay           every gate 1, inputs and flop outputs 0\n"
    "  --delay-file FILE      override table entries; lines are `<TYPE|DFF|SETUP|LOAD> <value>`\n"
    "  --load-factor K        add K per fanout to every gate delay\n"
    "  --setup S              flop setup time\n"
    "\n"
    "report: worst slack, critical path and top-N paths. If --period is omitted T is the\n"
    "smallest period that meets timing.\n"
    "eco: change gate delays and re-time incrementally; verifies against a full analysis.\n"
    "\n"
    "exit codes: 0 ok, 1 timing violation, 2 parse/file error, 3 combinational loop, 64 usage\n";

struct Args {
    std::string command;
    std::string file;
    std::optional<double> period;
    std::size_t top = 5;
    std::string delayFile;
    bool unitDelay = false;
    std::optional<double> loadFactor;
    std::optional<double> setup;
    std::string dotFile;
    bool quiet = false;
    std::optional<std::size_t> randomCount;
    std::uint64_t seed = 1;
    std::uint64_t cycles = 1;
    std::string vectorsFile;
    std::vector<std::pair<std::string, double>> sets;  // eco: NET=DELAY
};

int usageError(const std::string& msg) {
    std::fprintf(stderr, "gatetime: %s\n\n%s", msg.c_str(), kUsage);
    return kExitUsage;
}

bool parseDouble(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return !s.empty() && end == s.c_str() + s.size() && std::isfinite(out);
}

bool parseCount(const std::string& s, std::size_t& out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return false;
    out = static_cast<std::size_t>(std::strtoull(s.c_str(), nullptr, 10));
    return true;
}

// Returns -1 on success, otherwise the exit code to return.
int parseArgs(int argc, char** argv, Args& a) {
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        // Takes the next argument as this option's value.
        auto value = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        double d = 0;
        std::size_t n = 0;
        if (arg == "--period") {
            if (!value(v) || !parseDouble(v, d) || d < 0) return usageError("--period needs a non-negative number");
            a.period = d;
        } else if (arg == "--top") {
            if (!value(v) || !parseCount(v, a.top)) return usageError("--top needs a whole number");
        } else if (arg == "--delay-file") {
            if (!value(a.delayFile)) return usageError("--delay-file needs a path");
        } else if (arg == "--load-factor") {
            if (!value(v) || !parseDouble(v, d) || d < 0) return usageError("--load-factor needs a non-negative number");
            a.loadFactor = d;
        } else if (arg == "--setup") {
            if (!value(v) || !parseDouble(v, d) || d < 0) return usageError("--setup needs a non-negative number");
            a.setup = d;
        } else if (arg == "--dot") {
            if (!value(a.dotFile)) return usageError("--dot needs a path");
        } else if (arg == "--random") {
            if (!value(v) || !parseCount(v, n) || n == 0) return usageError("--random needs a whole number >= 1");
            a.randomCount = n;
        } else if (arg == "--seed") {
            if (!value(v) || !parseCount(v, n)) return usageError("--seed needs a whole number");
            a.seed = n;
        } else if (arg == "--cycles") {
            if (!value(v) || !parseCount(v, n) || n == 0) return usageError("--cycles needs a whole number >= 1");
            a.cycles = n;
        } else if (arg == "--vectors") {
            if (!value(a.vectorsFile)) return usageError("--vectors needs a path");
        } else if (arg == "--set") {
            const auto eq = value(v) ? v.find('=') : std::string::npos;
            if (eq == std::string::npos || eq == 0 || !parseDouble(v.substr(eq + 1), d) || d < 0) {
                return usageError("--set needs NET=DELAY with a non-negative number");
            }
            a.sets.emplace_back(v.substr(0, eq), d);
        } else if (arg == "--unit-delay") {
            a.unitDelay = true;
        } else if (arg == "--quiet") {
            a.quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            return usageError("unknown option '" + arg + "'");
        } else if (a.file.empty()) {
            a.file = arg;
        } else {
            return usageError("unexpected extra argument '" + arg + "'");
        }
    }
    if (a.file.empty()) return usageError("missing <netlist.bench>");
    if (a.unitDelay && (!a.delayFile.empty() || a.loadFactor || a.setup)) {
        return usageError("--unit-delay cannot be combined with --delay-file, --load-factor or --setup");
    }
    if (a.randomCount && !a.vectorsFile.empty()) return usageError("--random and --vectors are exclusive");
    if (a.command == "eco" && a.sets.empty()) return usageError("eco needs at least one --set NET=DELAY");
    return -1;
}

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int reportLoop(const std::string& file, const gt::Netlist& nl, const gt::TopoResult& topo) {
    std::fprintf(stderr, "%s: combinational loop detected (%zu nodes on or behind a loop)\n  loop:",
                 file.c_str(), topo.unresolved.size());
    for (gt::NodeId id : topo.cycle) std::fprintf(stderr, " %s ->", nl.gates[id].name.c_str());
    std::fprintf(stderr, " %s\n", nl.gates[topo.cycle.front()].name.c_str());
    return kExitLoop;
}

// Holds whichever delay model the options select.
struct ModelHolder {
    gt::UnitDelay unit;
    gt::TableDelay table;
    bool useUnit = false;
    const gt::DelayModel& get() const { return useUnit ? static_cast<const gt::DelayModel&>(unit) : table; }
};

void buildModel(const Args& a, ModelHolder& m) {
    m.useUnit = a.unitDelay;
    if (a.unitDelay) return;
    if (!a.delayFile.empty()) m.table.loadFile(a.delayFile);
    if (a.loadFactor) m.table.setLoadFactor(*a.loadFactor);
    if (a.setup) m.table.setSetupTime(*a.setup);
}

std::string pathLine(const gt::Netlist& nl, const gt::Path& p) {
    std::string s;
    for (gt::NodeId id : p.nodes) s += (s.empty() ? "" : " > ") + nl.gates[id].name;
    return s;
}

int runEco(const Args& a, const gt::Netlist& nl, const gt::TopoResult& topo, const gt::DelayModel& model) {
    gt::TimingOptions opt;
    opt.period = a.period;
    gt::IncrementalTimer timer(nl, topo.order, model, opt);
    const gt::TimingResult before = timer.result();

    gt::OverrideDelay overridden(model);
    std::printf("ECO on %s   T = %.3f   delay model: %s\n", a.file.c_str(), before.period, model.name().c_str());
    for (const auto& [name, delay] : a.sets) {
        const gt::NodeId id = nl.find(name);
        if (id < 0) throw std::runtime_error("--set: no net named '" + name + "'");
        std::printf("  set %-12s %.3f -> %.3f\n", name.c_str(), timer.result().delay[id], delay);
        timer.setDelay(id, delay);
        overridden.set(id, delay);
    }

    const auto t0 = Clock::now();
    const std::size_t visited = timer.update();
    const double ms = msSince(t0);
    const gt::TimingResult& after = timer.result();

    std::printf("Before: worst slack %.3f   violating %zu / %zu   critical delay %.3f\n", before.worstSlack,
                before.violatingEndpoints, before.endpoints.size(), before.criticalDelay);
    std::printf("After:  worst slack %.3f   violating %zu / %zu   critical delay %.3f\n", after.worstSlack,
                after.violatingEndpoints, after.endpoints.size(), after.criticalDelay);
    if (after.criticalEndpoint >= 0) {
        std::printf("Critical path now: %s\n", pathLine(nl, gt::criticalPath(after)).c_str());
    }
    std::printf("Incremental update: %zu node evaluations in %.3f ms (a full analysis does %zu)\n", visited, ms,
                2 * nl.size());

    // Cross-check against a from-scratch analysis with the same delays and period.
    gt::TimingOptions fullOpt;
    fullOpt.period = after.period;
    const gt::TimingResult full = gt::analyzeTiming(nl, topo.order, overridden, fullOpt);
    const bool same = full.arrival == after.arrival && full.required == after.required &&
                      full.slack == after.slack && full.pred == after.pred &&
                      full.worstSlack == after.worstSlack && full.criticalEndpoint == after.criticalEndpoint &&
                      full.violatingEndpoints == after.violatingEndpoints;
    std::printf("%s\n", same ? "Verified: identical to a full recomputation."
                             : "MISMATCH: incremental result differs from a full recomputation!");
    if (!same) return kExitError;
    return after.violatingEndpoints > 0 ? kExitViolation : kExitOk;
}

int runCommand(const Args& a) {
    const auto tParse = Clock::now();
    const gt::Netlist nl = gt::parseBenchFile(a.file);
    const double parseMs = msSince(tParse);

    const auto tTopo = Clock::now();
    const gt::TopoResult topo = gt::topoSort(nl);
    const double topoMs = msSince(tTopo);

    if (a.command == "stats") {
        gt::printStats(std::cout, a.file, nl, topo);
        return topo.hasCycle() ? kExitLoop : kExitOk;
    }
    if (topo.hasCycle()) return reportLoop(a.file, nl, topo);

    if (a.command == "check") {
        std::printf("%s: ok  inputs=%zu outputs=%zu gates=%zu", a.file.c_str(), nl.inputs.size(), nl.outputs.size(),
                    nl.gateCount() - nl.flops.size());
        if (!nl.flops.empty()) std::printf(" flops=%zu", nl.flops.size());
        std::printf(" depth=%d\n", topo.depth);
        return kExitOk;
    }

    if (a.command == "sim") {
        if (!a.vectorsFile.empty()) {
            std::ifstream in(a.vectorsFile);
            if (!in) throw std::runtime_error("cannot open vectors file: " + a.vectorsFile);
            const std::vector<std::string> vectors = gt::parseVectors(in, nl.inputs.size(), a.vectorsFile);
            const std::vector<std::string> outputs = gt::simulateVectors(nl, topo.order, vectors);
            gt::printVectorSim(std::cout, a.file, nl, vectors, outputs);
            return kExitOk;
        }
        const std::uint64_t count = a.randomCount.value_or(64);
        const auto tSim = Clock::now();
        const gt::RandomSimResult r = gt::simulateRandom(nl, topo.order, count, a.seed, 8, a.cycles);
        gt::printRandomSim(std::cout, a.file, nl, r, a.seed, msSince(tSim));
        return kExitOk;
    }

    ModelHolder holder;
    buildModel(a, holder);
    const gt::DelayModel& model = holder.get();

    if (a.command == "eco") return runEco(a, nl, topo, model);

    // report
    gt::TimingOptions opt;
    opt.period = a.period;
    const auto tTiming = Clock::now();
    const gt::TimingResult timing = gt::analyzeTiming(nl, topo.order, model, opt);
    const double timingMs = msSince(tTiming);

    const auto tPaths = Clock::now();
    gt::ReportData d;
    d.critical = gt::criticalPath(timing);
    if (!a.quiet) d.top = gt::topPaths(nl, timing, a.top);
    const double pathsMs = msSince(tPaths);

    d.netlistName = a.file;
    d.delayModelName = model.name();
    d.periodDefaulted = !a.period.has_value();
    d.netlist = &nl;
    d.topo = &topo;
    d.timing = &timing;
    d.parseMs = parseMs;
    d.topoMs = topoMs;
    d.timingMs = timingMs;
    d.pathsMs = pathsMs;

    if (a.quiet) {
        gt::printSummary(std::cout, d);
    } else {
        gt::printReport(std::cout, d);
    }

    if (!a.dotFile.empty()) {
        std::ofstream out(a.dotFile);
        if (!out) {
            std::fprintf(stderr, "cannot write DOT file: %s\n", a.dotFile.c_str());
            return kExitError;
        }
        gt::DotOptions dot;
        dot.timing = &timing;
        dot.highlight = d.critical.nodes.empty() ? nullptr : &d.critical;
        gt::writeDot(out, nl, dot);
    }
    return timing.violatingEndpoints > 0 ? kExitViolation : kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::fputs(kUsage, stdout);
        return kExitOk;
    }
    if (argc < 3) return usageError("expected a command and a netlist file");

    Args args;
    if (const int rc = parseArgs(argc, argv, args); rc >= 0) return rc;
    if (args.command != "report" && args.command != "check" && args.command != "stats" &&
        args.command != "sim" && args.command != "eco") {
        return usageError("unknown command '" + args.command + "'");
    }

    try {
        return runCommand(args);
    } catch (const gt::ParseError& e) {
        std::fprintf(stderr, "%s: parse error: %s\n", args.file.c_str(), e.what());
        return kExitError;
    } catch (const gt::DelayFileError& e) {
        std::fprintf(stderr, "delay file error: %s\n", e.what());
        return kExitError;
    } catch (const gt::VectorFileError& e) {
        std::fprintf(stderr, "vectors file error: %s\n", e.what());
        return kExitError;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return kExitError;
    }
}
