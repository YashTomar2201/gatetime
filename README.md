# GateTime

A static timing analyzer in C++17. It reads gate-level netlists in ISCAS `.bench` format
(combinational ISCAS'85 and sequential ISCAS'89), builds the circuit as a DAG, and reports
arrival times, slack, the critical path and the N worst paths. It can re-time a circuit
incrementally after a gate delay changes, and includes a bit-parallel logic simulator that
evaluates 64 input vectors per machine word.

GateTime is a **simplified** STA engine written to demonstrate the core graph algorithms
(see [Limitations](#limitations)). It is not a signoff tool.

```
$ gatetime report bench/s27.bench --top 3
Netlist: bench/s27.bench   gates=10 inputs=4 outputs=1 flops=3 depth=6
Delay model: table (built-in defaults)   Clock period T = 7.100 (defaulted to the smallest period that meets timing)
Parse: 0.231 ms   Topo: 0.001 ms   Timing: 0.006 ms   Paths: 0.015 ms

Worst slack: 0.000    Violating endpoints: 0 / 4

Critical path (delay = 6.900):
  startpoint  G6 (DFF)   AT     0.800
              G8 (AND)   AT     2.200
              G15 (OR)   AT     3.700
              G9 (NAND)  AT     4.700
              G11 (NOR)  AT     5.800
  endpoint    G10 (NOR)  AT     6.900   slack 0.000

Top 3 paths:
    #       delay       slack  startpoint -> endpoint  gates  path
    1       6.900       0.000  G6 -> G10               5      G6 > G8 > G16 > G9 > G11 > G10
    2       6.900       0.000  G6 -> G10               5      G6 > G8 > G15 > G9 > G11 > G10
    3       6.600       0.300  G7 -> G10               5      G7 > G12 > G15 > G9 > G11 > G10
```

## Features

- **Parser:** `.bench` with forward references, multi-input gates, `DFF`, comments, any
  whitespace, CRLF, case-insensitive keywords. Errors carry a line number (unknown gate,
  undefined net, duplicate driver, bad arity, malformed line). Fuzz-tested.
- **Graph:** index-based netlist, fanin and fanout lists, open-addressing name index.
- **Topology:** Kahn's algorithm with levelization and combinational-loop detection (reports
  one concrete loop). Flops cut the graph, so sequential feedback is not a loop.
- **Timing:** arrival time, required time, slack; setup time at flop D pins and clock-to-Q at
  flop outputs; pluggable delay model: unit, per-gate-type table with a delay file,
  load-dependent (`base + k x fanout`), or your own `DelayModel`.
- **Paths:** critical path and top-N worst paths by best-first search (no path enumeration),
  ranked by slack.
- **Incremental timing:** change gate delays and re-time only the affected cones; results are
  bit-identical to a full analysis.
- **Simulator:** bit-parallel (64 vectors per word), exhaustive / random / file vectors, and
  clocked simulation of sequential circuits (64 independent sequences per word).
- **Tooling:** Graphviz export with the critical path in red, random layered-DAG generator,
  benchmark harness, CI with sanitizers (see [Testing](#testing)).

## Build and run

Requires a C++17 compiler (developed with g++ 13.1 from MSYS2 UCRT64), CMake and Ninja.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `-DGATETIME_WERROR=ON` (warnings are errors), `-DGATETIME_SANITIZE=ON`
(AddressSanitizer + UBSan on GCC/Clang). Without CMake:
`g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude src/*.cpp -o gatetime`.

```
gatetime report <netlist.bench> [--period T] [--top N] [--dot OUT.dot] [--quiet] [delay options]
gatetime eco    <netlist.bench> --set NET=DELAY [--set ...] [--period T] [delay options]
gatetime check  <netlist.bench>
gatetime stats  <netlist.bench>
gatetime sim    <netlist.bench> [--random COUNT] [--seed S] [--cycles C] | [--vectors FILE]

delay options (default: built-in per-gate-type table):
  --unit-delay       every gate 1, inputs and flop outputs 0
  --delay-file FILE  override table entries (`<TYPE|DFF|SETUP|LOAD> <value>`, see bench/delays.txt)
  --load-factor K    add K per fanout to every gate delay
  --setup S          flop setup time
```

If `--period` is omitted, T is the smallest period that meets timing (worst slack 0).
Exit codes: `0` ok, `1` timing violation, `2` parse or file error, `3` combinational loop,
`64` usage error.

Incremental re-timing (`eco`) changes gate delays and checks the incremental result against a
from-scratch analysis:

```
$ gatetime eco bench/c17.bench --period 3 --set 16=2.5
ECO on bench/c17.bench   T = 3.000   delay model: table (built-in defaults)
  set 16           1.000 -> 2.500
Before: worst slack 0.000   violating 0 / 2   critical delay 3.000
After:  worst slack -1.500   violating 2 / 2   critical delay 4.500
Critical path now: 3 > 11 > 16 > 22
Incremental update: 7 node evaluations in 0.003 ms (a full analysis does 22)
Verified: identical to a full recomputation.
```

Other examples:

```
$ gatetime report bench/c17.bench --period 2 --quiet ; echo $?
bench/c17.bench: T=2.000 critical=3.000 worst_slack=-1.000 violating=2/2
1

$ gatetime check tests/data/loop.bench ; echo $?
tests/data/loop.bench: combinational loop detected (3 nodes on or behind a loop)
  loop: x -> y -> z -> x
3

$ gatetime sim bench/s27.bench --random 256 --cycles 6 --seed 3
$ gatetime report bench/c17.bench --dot c17.dot && dot -Tpng c17.dot -o c17.png
```

## How it works

Every net has one driver, so a gate and its output net are one node. **Arrival time** AT(n)
is the latest time a change can reach n: the max AT over its fanin plus its own delay,
computed in one pass in topological order. **Required time** RAT(n) is the latest it may
arrive without missing the clock period T: at a primary output it is T, at a flop D pin
T minus the setup time, elsewhere the min over its fanout of RAT minus that gate's delay,
computed in one pass in reverse order. **Slack** is RAT minus AT; negative slack means the
path misses T. Flop outputs start new paths (launching at clock-to-Q) and flop inputs end
them, which is what keeps register feedback from being a cycle. The critical path is found by
walking predecessor pointers back from the endpoint with the smallest slack.

Design decisions, trade-offs, the defects found in review and the experiments that did not
pay off are in [docs/design.md](docs/design.md).

## Algorithms and complexity

| Step | Algorithm | Complexity |
|---|---|---|
| Parse | single pass over the file, open-addressing name index | O(file size) |
| Levelize, loop detection | Kahn's algorithm (iterative) | O(V + E) |
| Arrival, required, slack | forward and backward pass in topological order | O(V + E) |
| Critical path | predecessor pointers | O(path length) |
| Top-N paths | best-first search backwards from endpoints, exact AT heuristic, parent-pointer arena | O(N x depth x fanin x log heap) |
| Incremental update | two priority-queue waves with early exit | O(affected cone x log) |
| Simulation | compiled flat program, one bitwise op per fanin, 64 vectors per word | O((V + E) x vectors / 64) |

## Benchmarks

Measured on an AMD Ryzen 5 5500U (6 cores / 12 threads), 7.3 GB RAM, Windows 11,
g++ 13.1.0 (MSYS2 UCRT64), `-O2 -DNDEBUG -std=c++17`. Each phase is the **median of 10 runs**
(`gatetime_bench`, `std::chrono::steady_clock`); parse includes reading the file from disk
(page cache warm after the first run). Delay model: the built-in table; T = smallest feasible.
Run-to-run variation on this laptop is around 10-15%.

**ISCAS'85 and ISCAS'89** (the real circuits; all 47 in `bench/iscas/` plus c17 and s27 are in
[bench/results.md](bench/results.md), a representative subset is shown here):

| Circuit | Gates | Flops | Depth | Parse (ms) | Topo (ms) | Timing (ms) | Top-10 (ms) | Sim 4096 vec (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| c17 | 6 | | 3 | 0.05 | 0.00 | 0.00 | 0.00 | 0.01 |
| c432 | 160 | | 17 | 0.21 | 0.00 | 0.01 | 0.02 | 0.07 |
| c880 | 383 | | 24 | 0.23 | 0.00 | 0.01 | 0.02 | 0.11 |
| c1908 | 880 | | 40 | 0.46 | 0.01 | 0.03 | 0.05 | 0.26 |
| c3540 | 1,669 | | 47 | 0.71 | 0.03 | 0.06 | 0.03 | 0.49 |
| c6288 (16x16 multiplier) | 2,416 | | 124 | 0.99 | 0.03 | 0.07 | 0.04 | 0.34 |
| c7552 | 3,512 | | 43 | 1.36 | 0.06 | 0.11 | 0.05 | 1.16 |
| s27 | 10 | 3 | 6 | 0.06 | 0.00 | 0.00 | 0.01 | 0.01 |
| s298 | 119 | 14 | 9 | 0.10 | 0.00 | 0.00 | 0.01 | 0.03 |
| s1423 | 657 | 74 | 59 | 0.38 | 0.01 | 0.03 | 0.03 | 0.19 |
| s5378 | 2,779 | 179 | 25 | 1.21 | 0.04 | 0.09 | 0.03 | 0.73 |
| s9234 | 5,597 | 228 | 58 | 1.92 | 0.07 | 0.15 | 0.04 | 1.44 |
| s15850 | 9,772 | 597 | 82 | 3.36 | 0.12 | 0.29 | 0.10 | 2.67 |
| s35932 | 16,065 | 1,728 | 29 | 5.96 | 0.16 | 0.43 | 0.11 | 2.43 |
| s38417 | 22,179 | 1,636 | 47 | 8.78 | 0.27 | 0.89 | 0.14 | 5.37 |
| s38584 | 19,253 | 1,452 | 56 | 7.26 | 0.26 | 0.76 | 0.12 | 5.74 |

Every ISCAS circuit is analyzed end to end (parse, sort, timing, top-10) in about 10 ms or
less, and on the largest, s38417, the parser is most of that. Incremental re-timing after a
single gate edit (median of 100 random edits per circuit) takes 0.6-8 microseconds on the
ISCAS'85 circuits and 0.7-7 microseconds on ISCAS'89, re-evaluating on average 0.1-22% of
the nodes a full analysis would (excluding the 6- and 10-gate toys c17 and s27; the share
falls as circuits grow: on s38584 the mean edit evaluates 38 nodes where a full pass
evaluates about 41,000).

**Synthetic circuits** (layered random DAGs, to see how it scales past ISCAS sizes):

| Circuit | Gates | Edges | Depth | Parse (ms) | Topo (ms) | Timing (ms) | Top-10 (ms) | Sim 4096 vec (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| synthetic 10k | 10,000 | 30,025 | 100 | 5.61 | 0.32 | 0.62 | 0.09 | 7.84 |
| synthetic 100k | 100,000 | 299,784 | 200 | 81.23 | 10.55 | 14.79 | 0.66 | 93.71 |
| synthetic 1M | 1,000,000 | 3,000,424 | 1000 | 1104.46 | 144.69 | 201.67 | 11.18 | 1813.61 |

Incremental timing: one random gate made 50% slower, 100 random gates per circuit, each edit
undone before the next.

| Circuit | Nodes | Full timing (ms) | Edit median (ms) | Edit p90 (ms) | Mean node evaluations | Of a full pass |
|---|---:|---:|---:|---:|---:|---:|
| synthetic 10k | 10,100 | 0.62 | 0.0062 | 0.21 | 903 | 4.47% |
| synthetic 100k | 100,500 | 14.79 | 0.0406 | 0.89 | 4,906 | 2.44% |
| synthetic 1M | 1,001,000 | 201.67 | 0.2217 | 4.77 | 6,001 | 0.30% |

Synthetic circuits come from `bench/gen_random_dag.cpp` (layered DAG, fanin 2-4, seed 1).
Reproduce with `sh bench/run_bench.sh`.

Findings:

- **Parsing dominates** (about 75% of the one-shot analysis at 1M gates). The first version
  took 5,982 ms to parse 1M gates; profiling with cycle counters showed the cost was
  allocation (growing tiny fanout vectors, one hash-map node per net) rather than string
  scanning. Reading the file at once, reserving storage, building fanout lists in a counted
  second pass and an open-addressing name index brought it to about 1.1 s (5x faster).
- **Topo, timing and top-N scale roughly linearly** in gates, with a bump between 100k and
  1M (about 13x for 10x the gates) consistent with the working set leaving cache.
- **Incremental update is ~900x cheaper than a full pass at 1M gates** for a typical edit;
  the p90 shows that an edit on a critical path really does move a large cone.
- The simulator is compiled into a flat program once per run: 4096 vectors on 1M gates is
  3.8 s -> 1.8 s compared with walking the netlist's per-gate vectors.
- A CSR / renumbered copy of the graph speeds the timing passes 2.6x but costs more to build
  than it saves for one-shot analysis (details in [docs/design.md](docs/design.md)).
- **The real circuits found a bug the synthetic tests missed.** On c6288, a multiplier with
  about 10^20 paths of near-identical delay, top-N path extraction ran out of memory: keys
  that are equal on paper differed by one ulp, so the tie-breaking that worked on exactly
  representable unit delays fell back to breadth-first search. The fix treats keys within the
  rounding tolerance as equal and never lets a child outrank its parent; the same request now
  takes 0.04 ms, and c6288 is a regression test. A guard also turns an out-of-proportion
  `--top` into a clear error instead of `bad_alloc`.

## Testing

112 doctest cases (about 650,000 assertions) plus 46 end-to-end CLI checks (output and exit
codes), all run by `ctest`. The suite builds with zero warnings under
`-Wall -Wextra -Wpedantic`, and with `-Werror`.

- **Parser:** c17 counts and fanin/fanout, forward references, multi-input gates, comments and
  whitespace, CRLF, duplicate fanin, PI that is also a PO, `DFF`, each error with its line
  number, and the name index (growth, misses, copies).
- **Fuzzing:** mutated and random-byte inputs to the parser, delay-file and vector-file
  readers: only success with a well-formed result or the documented exception.
- **Topology:** every edge goes forward on c17 and on random DAGs whose ids are not in
  topological order; loops (3-gate, self, with downstream logic); a 200k-gate chain.
- **Timing:** c17 against the hand table (T = 3 and T = 2); flop launch, setup and
  load-dependent delay by hand; s27 by hand; random circuits (with flops) against independent
  memoized-recursion references for AT, RAT and the default period; rounding-noise tolerance
  at 1e8 magnitudes.
- **Paths:** best-first top-N equals brute-force enumeration on 60 random circuits (with
  flops, setup and exact ties, ranked by slack); a circuit with 2^60 paths returns its top 5
  immediately; the real c6288 multiplier (regression, see Findings); a 150k-gate chain.
- **Incremental:** hundreds of random edit batches on random circuits with flops, compared with
  `==` against a from-scratch analysis; work stays proportional to the affected cone.
- **Simulator:** c17 truth table over all 32 vectors; bit-parallel vs a scalar evaluator on
  random netlists, combinational and clocked; shift register and toggle flop by hand; fanin
  validation.
- **Sanitizers:** AddressSanitizer and UBSan are not available for the MinGW toolchain this was
  developed on, so locally the suite is also run under libstdc++ debug containers
  (`-D_GLIBCXX_DEBUG`), which bounds-check every `vector` access. The CI workflow in
  `.github/workflows/ci.yml` runs gcc and clang with ASan + UBSan on Linux; **it has not run
  yet**, the first push will be its first execution.

## Limitations

GateTime does not model slew or transition time, wire parasitics, hold checks (only the
setup-style longest path is analyzed), clock skew or multiple clocks, multiple corners, SDC
constraints, or false and multicycle paths. Delays are per gate type (optionally plus a load
term per fanout), not from a library with slew-dependent tables. Paths are counted per fanin
pin, so `NAND(a, a)` yields two paths with the same nodes. The ISCAS circuits come from the CTU
Prague benchmark archive (`bench/iscas/README.md`); one of them, `s400`, is incomplete in that
archive and is excluded. Delays are not from a real cell library, so absolute numbers
(arbitrary units) say nothing about real silicon; only the engine's speed and structure are
being measured.

## Roadmap

Structural-Verilog subset parser, hold-time check (needs an earliest-arrival pass), level-
parallel forward pass, packing the netlist into one shared array to speed up parsing further,
wider simulator lanes (SIMD), and the larger ITC'99 benchmarks.
