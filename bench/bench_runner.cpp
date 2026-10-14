// Phase-by-phase benchmark. For each netlist it prints two Markdown tables:
//   1. parse, topological sort, timing (forward + backward), top-K extraction and a
//      4096-vector random simulation, each the median of N runs;
//   2. incremental timing: the cost of re-timing after changing one gate's delay,
//      against a full analysis of the same circuit.
//
//   gatetime_bench [--runs N] [--top K] [--edits E] file.bench...
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <random>
#include <string>
#include <vector>

#include "gatetime/delay_model.hpp"
#include "gatetime/incremental.hpp"
#include "gatetime/parser_bench.hpp"
#include "gatetime/paths.hpp"
#include "gatetime/sim.hpp"
#include "gatetime/timing.hpp"
#include "gatetime/topo.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct EcoRow {
    std::string name;
    double fullMs = 0, medianMs = 0, p90Ms = 0, meanEvals = 0;
    std::size_t nodes = 0;
    int edits = 0;
};

}  // namespace

int main(int argc, char** argv) {
    int runs = 10, edits = 50;
    std::size_t topK = 10;
    std::vector<std::string> files;
    const char* usage = "usage: gatetime_bench [--runs N] [--top K] [--edits E] file.bench...\n";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--runs") && i + 1 < argc) runs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--top") && i + 1 < argc) topK = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--edits") && i + 1 < argc) edits = std::atoi(argv[++i]);
        else if (argv[i][0] == '-') {
            std::fputs(usage, stderr);
            return 64;
        } else files.push_back(argv[i]);
    }
    if (runs < 1 || edits < 1 || files.empty()) {
        std::fputs(usage, stderr);
        return 64;
    }

    std::printf("Median of %d runs, times in ms (parse reads the file from disk each run).\n\n", runs);
    std::printf("| Circuit | Gates | Edges | Depth | Parse | Topo | Timing | Top-%zu | Sim 4096 vec | Total (no sim) |\n",
                topK);
    std::printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");

    gt::TableDelay model;
    std::vector<EcoRow> eco;

    for (const std::string& file : files) {
        try {
            std::vector<double> parse, topo, timing, paths, sim;
            std::size_t gates = 0, edges = 0;
            int depth = 0;
            double sink = 0;  // keeps the optimizer from discarding results

            for (int r = 0; r < runs; ++r) {
                auto t0 = Clock::now();
                const gt::Netlist nl = gt::parseBenchFile(file);
                parse.push_back(msSince(t0));

                t0 = Clock::now();
                const gt::TopoResult tp = gt::topoSort(nl);
                topo.push_back(msSince(t0));
                if (tp.hasCycle()) throw std::runtime_error("combinational loop");

                gt::TimingOptions opt;  // period defaults to the smallest feasible one
                t0 = Clock::now();
                const gt::TimingResult tr = gt::analyzeTiming(nl, tp.order, model, opt);
                timing.push_back(msSince(t0));

                t0 = Clock::now();
                const auto top = gt::topPaths(nl, tr, topK);
                paths.push_back(msSince(t0));

                t0 = Clock::now();
                const gt::RandomSimResult sr = gt::simulateRandom(nl, tp.order, 4096, 1, 0);
                sim.push_back(msSince(t0));

                sink += tr.worstSlack + static_cast<double>(top.size()) + static_cast<double>(sr.ones.size());
                if (r == 0) {
                    gates = nl.gateCount() - nl.flops.size();
                    depth = tp.depth;
                    for (const gt::Gate& g : nl.gates) edges += g.fanin.size();
                }
            }
            const double mp = median(parse), mt = median(topo), mm = median(timing), mk = median(paths),
                         ms = median(sim);
            std::printf("| %s | %zu | %zu | %d | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |\n",
                        baseName(file).c_str(), gates, edges, depth, mp, mt, mm, mk, ms, mp + mt + mm + mk);
            if (sink == -1.0) std::printf("\n");

            // Incremental timing on this circuit: random single-gate edits. Each edit
            // makes one gate 50% slower, is timed, then undone (untimed) so edits are
            // independent of each other.
            {
                const gt::Netlist nl = gt::parseBenchFile(file);
                const gt::TopoResult tp = gt::topoSort(nl);
                gt::TimingOptions opt;
                gt::IncrementalTimer timer(nl, tp.order, model, opt);

                std::vector<gt::NodeId> candidates;
                for (gt::NodeId id = 0; id < static_cast<gt::NodeId>(nl.size()); ++id) {
                    if (!nl.gates[id].fanin.empty()) candidates.push_back(id);
                }
                if (!candidates.empty()) {
                    std::mt19937 rng(42);
                    std::vector<double> times, evals;
                    for (int e = 0; e < edits; ++e) {
                        const gt::NodeId id = candidates[rng() % candidates.size()];
                        const double old = timer.result().delay[id];
                        timer.setDelay(id, old * 1.5 + 0.1);
                        const auto t0 = Clock::now();
                        const std::size_t visited = timer.update();
                        times.push_back(msSince(t0));
                        evals.push_back(static_cast<double>(visited));
                        timer.setDelay(id, old);
                        timer.update();
                    }
                    EcoRow row;
                    row.name = baseName(file);
                    row.fullMs = median(timing);
                    row.medianMs = median(times);
                    row.p90Ms = percentile(times, 0.9);
                    double sum = 0;
                    for (double v : evals) sum += v;
                    row.meanEvals = sum / static_cast<double>(evals.size());
                    row.nodes = nl.size();
                    row.edits = edits;
                    eco.push_back(row);
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), e.what());
            return 2;
        }
    }

    std::printf("\nIncremental timing: one gate made 50%% slower, %d random gates per circuit (ms).\n\n", edits);
    std::printf("| Circuit | Nodes | Full timing | Edit median | Edit p90 | Mean node evals | Of a full pass |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---:|\n");
    for (const EcoRow& r : eco) {
        std::printf("| %s | %zu | %.2f | %.4f | %.4f | %.0f | %.2f%% |\n", r.name.c_str(), r.nodes, r.fullMs,
                    r.medianMs, r.p90Ms, r.meanEvals, 100.0 * r.meanEvals / (2.0 * static_cast<double>(r.nodes)));
    }
    return 0;
}
