# GateTime design notes

Decisions and trade-offs, one section per phase.

## Phase 1: netlist and `.bench` parser

- **Indices, not pointers.** `NodeId` is an `int` into `Netlist::gates`. Contiguous storage,
  trivially copyable, no dangling pointers when the vector grows.
- **Net = its driver.** In `.bench` every net has exactly one driver, so one node per gate
  output. Primary inputs are nodes with no fanin.
- **Forward references.** `Netlist::getOrCreate` makes a node on first mention. Whether a net
  was actually *driven* is tracked inside the parser (not in `Gate`), and checked at end of
  file so the error can point at the first line that used an undriven net.
- **Edges are kept per occurrence.** `z = NAND(a, a)` stores `a` twice in `z.fanin` and `z`
  twice in `a.fanout`, so in-degree counts in Kahn's algorithm stay consistent.
- **Errors are exceptions with a line number** (`ParseError::line()`); the CLI maps them to
  exit code 2.
- **Strictness.** Duplicate drivers, unknown gate types, bad arity (`NOT`/`BUFF` take one
  input), empty arguments and undefined nets are errors. A repeated `OUTPUT(x)` is ignored.
- **Sequential circuits.** `q = DFF(d)` makes `q` a `DffQ` node with *no fanin* and records the
  pair in `Netlist::flops`. Cutting the graph at the flop is what keeps sequential feedback
  from becoming a combinational cycle (see the review pass below). A combinational loop
  parses fine; it is the topological-sort pass that reports it.
- **Test naming gotcha.** A free function called `toString(GateType)` in namespace `gt` gets
  picked up by ADL inside doctest's own `toString` and breaks `CHECK(a == b)`. Hence
  `gateTypeName`.

## Phase 2: topological order and loops

- **Kahn's algorithm, iterative.** O(V + E). The output vector doubles as the FIFO queue
  (everything released so far, in release order), so there is no separate queue.
- **Levels come for free.** `level[v] = 1 + max level of fanin`, computed while edges are
  released. `depth` (max level) feeds `stats`, and nodes at the same level are independent,
  which is what a level-parallel pass would exploit.
- **What a loop looks like.** Nodes whose in-degree never reaches 0 are "unresolved": on a
  loop or downstream of one. Every unresolved node has an unresolved fanin, so walking
  backwards through unresolved fanins must revisit a node; that revisit closes a concrete loop,
  which is reported in signal-flow order.
- **No recursion anywhere.** A 200k-gate chain is in the tests; recursive DFS would overflow
  the stack on real circuits.

## Phase 3: delay models and timing

- **`DelayModel` is an interface** (`delay(netlist, node)`); `timing.cpp` only sees that.
  `UnitDelay` is for hand checks; `TableDelay` maps gate type to delay, starts from built-in
  defaults and lets a delay file override entries. A bad delay file leaves the model unchanged.
- **Ties are broken by lowest `NodeId`** (predecessor and critical endpoint), so output is
  deterministic.
- **Endpoints are `OUTPUT(...)` nets and flop D nets.** A node that is an endpoint and also has
  fanout takes `min(own requirement, RAT(fanout) - delay)`. Logic that reaches no endpoint has
  `RAT = slack = +inf` and is counted as "unconstrained" in the report.
- **`period` is optional.** If unset it is the smallest period that meets timing, so worst
  slack is 0 and the tool answers "what is the tightest clock this circuit meets".
- **Floating point.** The forward pass sums delays up the path and the backward pass subtracts
  them down it, so slack on a critical path can come out as 1e-16 instead of 0. See the
  review pass for the tolerance this uses.

## Phase 4: critical path and top-N

- **Critical path** is the predecessor chain from the worst endpoint (smallest slack against
  its own requirement; with one clock and no flops that is the largest arrival).
- **Top-N by best-first search.** A partial path is a suffix ending at an endpoint with head
  `h` and suffix cost `S` (delays of the nodes after `h`). Its key is `AT(h) + S`. Because
  `AT(h)` is the *longest* path to `h`, that key is exactly the delay of the best complete
  path through the suffix, i.e. an exact heuristic (A* with a perfect heuristic). It is also
  consistent: expanding to fanin `f` gives `AT(f) + S + d(h) <= AT(h) + S`, since
  `AT(h) >= AT(f) + d(h)`. Keys never increase along a search path, so the first complete path
  popped from the max-heap is the worst, the next is the second worst, and so on.
- **Arena of parent links**, not copied vectors: a push is O(1) and the completed entry's
  parent chain is the path, startpoint first.
- **Tie-break: newest entry first.** With equal keys (e.g. a ladder of identical parallel
  paths) FIFO order would expand every partial path of one length before any of the next,
  which is exponential. Preferring the newest entry dives to a complete path. A test with
  2^60 equal paths covers this.
- **Reported delay is re-summed forwards**, in the same order as the forward pass, so the
  worst path's delay equals `arrival[endpoint]` bit for bit.
- **Paths are per fanin pin**, so `NAND(a, a)` yields two paths with identical nodes. The
  brute-force reference counts the same way.
- **Tie ordering.** When several paths have the same delay their relative order is
  deterministic but unspecified, so top-1 can differ from the "critical path" printed above it
  (which follows predecessor pointers to the lowest-id endpoint) while having the same delay.

## Phase 5: CLI, report, DOT

- **Library vs CLI.** Everything except argument parsing and exit-code mapping lives in
  `gatetime_core`; `main.cpp` is a thin wrapper. Report formatting is in `report.cpp` so it can
  be unit tested.
- **Exit codes:** 0 ok, 1 timing violation, 2 parse/file error, 3 combinational loop,
  64 usage. The CLI test (`tests/cli_test.cmake`) runs the real executable and checks them.
- **Long critical paths** print the first and last 20 nodes with an elision line.
- **DOT:** node ids are `n<index>` and names only appear in escaped labels, so odd net names
  cannot break the file. The highlighted path is drawn red.

## Phase 6: simulator

- **One `uint64_t` per net = 64 vectors.** Gates are evaluated in topological order with
  bitwise operators; a gate with k inputs folds k words. Same algorithm as a scalar simulator,
  64x the throughput per operation.
- **Exhaustive vectors:** input `k < 6` gets the standard patterns (`0xAAAA...`, `0xCCCC...`,
  `0xF0F0...`, ...); inputs `>= 6` are constant per batch and come from the batch number, so
  any number of inputs is enumerated in `2^(n-6)` batches.
- **Reproducible random vectors.** `SplitMix64` is used instead of `std::mt19937` +
  distributions so a seed gives the same vectors on every platform and standard library.
- **Partial last word.** `--random 100` simulates 2 words but masks the unused 28 bits before
  counting ones.

## Phase 7: benchmarks and the parse optimization

Benchmark harness: `bench/bench_runner.cpp` (median of N runs per phase),
`bench/gen_random_dag.cpp` (layered DAG, seed 1), `bench/run_bench.sh`.

Baseline (before any tuning), median of 10, ms:

| Circuit | Parse | Topo | Timing | Top-10 |
|---|---:|---:|---:|---:|
| synthetic 10k | 31.33 | 0.95 | 1.13 | 0.19 |
| synthetic 100k | 434.57 | 15.64 | 19.47 | 1.20 |
| synthetic 1M | 5982.11 | 203.44 | 255.08 | 18.95 |

Parsing was 93% of the total and grew faster than linearly (14x for 10x the gates).

Profiling a copy of the parser with `rdtsc` around each section (cycles per line, 1M-gate
file) showed: fanin/fanout push 42%, name lookups and node creation 44%, tokenizing 14%.
The cost was allocation, not string scanning: every net got several tiny vector growths
(1 -> 2 -> 4) and a hash-map node.

Changes, each measured on the 1M-gate file (parse ms):

| Change | Parse (ms) |
|---|---:|
| baseline: `getline` per line, `std::string` per line, vectors grown by `push_back` | 5982 |
| read whole file once, parse with `string_view`, reserve gates/map/bookkeeping up front, no per-line allocations, case-insensitive compare without copying | 4205 |
| build fanout lists after parsing: count, reserve exactly, fill in definition order | 2582 |
| single-probe `try_emplace` in `getOrCreate` | 2481 |

Behavior is unchanged: the same 75 unit tests and CLI checks pass after every step.

What was left after that is covered by the review pass below (it replaced the hash map).

The real ISCAS'85/'89 circuits were benchmarked afterwards (see the section below).
Sanitizers: AddressSanitizer and UBSan runtimes are not shipped for this MinGW toolchain.
The test suite was instead built with `-D_GLIBCXX_DEBUG` (bounds-checked containers) and passed.

## Review pass: bugs fixed, sequential support, incremental timing, performance

A second pass over the whole code base. Everything below is covered by tests; the property
tests that found nothing are kept because they are what justifies the claims.

### Defects found and fixed

| # | Defect | Effect | Fix |
|---|---|---|---|
| 1 | Delay files accepted `inf` (`inf >= 0`) | One `inf` delay turns every slack into NaN and every comparison false | Delays must be finite and >= 0; same rule in `IncrementalTimer::setDelay` |
| 2 | A startpoint's own delay was ignored | A delay model that gives an input or flop output a delay (clock-to-Q) had no effect | Startpoint arrival = `inputArrival + delay` |
| 3 | Critical endpoint = largest arrival | Wrong once endpoints have different requirements (flop setup): the worst *slack* is not the longest path | Critical endpoint = smallest slack against its own requirement |
| 4 | Top-N ranked by delay | Same problem: a shorter flop path with large setup can be the worst | Ranked by slack; the search key becomes `AT(h) + S - required(endpoint)`, still exact and consistent |
| 5 | Slack tolerance was an absolute 1e-9 | At 1e8-scale arrivals one ulp is 1.5e-8, so rounding noise could be reported as a violation | Tolerance scales: `1e-9 * max(1, |T|, largest endpoint arrival)` |
| 6 | `Simulator` indexed `fanin[0]` unchecked | A hand-built `NOT` with no fanin was undefined behaviour | Fanin counts validated once at construction |
| 7 | Sequential circuits rejected | The most common real input (ISCAS'89) could not be analysed | See below |
| 8 | `--top` and `--quiet` | `--quiet` still extracted the top-N paths | Skipped in quiet mode |
| 9 | Endpoint slack used node slack | An endpoint with fanout would report the *downstream* path's slack as its own | Endpoint statistics use the endpoint's own requirement |
| 10 | Top-N tie-breaking trusted exact ties (found by running the real c6288) | Keys equal on paper differ by an ulp in floating point; the newest-first rule then degenerates to breadth-first and the search ran out of memory on a multiplier with ~10^20 near-equal paths | A child key within the slack tolerance of its parent's is treated as equal and a child never outranks its parent; a partial-path limit turns runaway requests into a clear error |

Fuzz tests (`tests/test_fuzz.cpp`) mutate valid files and feed random bytes to the parser,
delay-file and vector-file readers: the only allowed outcomes are success with a well-formed
result, or the documented exception type. They also run every parsed netlist through
timing and simulation.

### Sequential circuits (ISCAS'89 style)

- `q = DFF(d)`: `q` is a startpoint with no fanin, launching at `clock-to-Q`; `d` is an
  endpoint that must arrive `setup` before the clock edge. Cutting at the flop means a
  feedback loop through a register is not a combinational loop.
- The default period is the smallest feasible one: `max over endpoints of (arrival + margin)`,
  where margin is the setup time at flop D pins and 0 at primary outputs.
- Per-type delay table gained `DFF` (clock-to-Q), `SETUP` and `LOAD` keys.
- **Load-dependent delay**: `delay = table[type] + LOAD * fanout`. A gate driving more loads is
  slower; primary inputs stay free.
- The simulator holds flop state: each `step()` evaluates one clock cycle then latches
  every D into its Q. The 64 bit lanes are 64 independent machines.
- Not modelled: clock skew and hold checks, a single ideal clock only.

### Incremental timing (`IncrementalTimer`)

After changing one gate's delay only two cones can change: arrival times in its fanout cone and
required times in its fanin cone. Both waves run through a priority queue keyed by position in
the topological order (earliest first forwards, latest first backwards), so a node is
evaluated only once everything it depends on has settled, and a wave stops at any node whose
value comes out unchanged.

- The full analysis and the incremental one call the *same* `arrivalOf` / `requiredOf`
  functions, so results are bit-identical, not merely close. The tests compare every array with
  `==` after hundreds of random edit batches on random circuits with flops.
- The period is frozen at construction. If it were re-derived from the critical delay, every
  required time in the circuit would move on every edit and nothing would be incremental.
- Measured on the synthetic 1M-gate circuit, one gate made 50% slower: median 0.22 ms
  against 202 ms for a full analysis (about 900x), 0.30% of a full pass in node evaluations on
  average. The p90 is 4.8 ms: an edit on a critical path really does move a large cone.
- The `eco` command does this from the CLI and verifies against a full recomputation.

### Performance

| Change | Measured effect |
|---|---|
| Open-addressing name index instead of `unordered_map<string,int>` | 1M-gate parse 2.5 s -> 1.1-1.3 s. One 8-byte slot + hash per net, names stored once (in `gates[].name`), no heap allocation per net, no temporary `std::string` per lookup. Table growth reuses the stored hashes. |
| Compile the simulator into a flat topological program (opcodes + CSR fanin in position space) | 4096 random vectors on 1M gates: 3.8 s -> 1.5-1.8 s (100k gates: 285 -> 85 ms). It evaluates the same netlist many times, so the one-off compile amortises. |
| Size the file buffer once (`fseek`/`ftell`) | small, removes reallocation while reading |

Where parse time goes now (1M gates, cycles per line from `rdtsc`, ~1,950 cycles/line in
total): name lookups 49%, building fanout lists after parsing 31%, tokenizing 16%, file read
4%. What is left is dominated by one heap block per net for the fanin and fanout vectors and
by cache misses on the gate array. Going further means replacing `std::vector` inside `Gate`
with storage in one shared array, which changes the public data model; not done.

**An experiment that did not pay off.** A CSR (compressed sparse row), topologically
renumbered copy of the graph makes the forward and backward passes 2.6x faster on their own
(60 ms vs 156 ms for timing on 1M gates). But building it costs 60 ms (CSR) + 67 ms (renumber)
and Kahn's algorithm needs another 41 ms, so the one-shot pipeline (227 ms) is only ~17%
faster than topo + timing today (273 ms). It only wins when the compiled form is reused, which
is why the simulator uses it and the one-shot analysis does not. It would also win if the
parser produced CSR directly.

### Tooling

- `-DGATETIME_SANITIZE=ON` builds with AddressSanitizer + UBSan; `-DGATETIME_WERROR=ON` makes
  warnings errors (checked locally: the whole tree builds clean with it).
- `.github/workflows/ci.yml` runs gcc and clang, warnings-as-errors, and gcc/clang with
  sanitizers on Linux. **It has not been executed yet**: sanitizers do not exist for the
  MinGW toolchain this was developed on, so the first push is the first sanitizer run.
- Locally the suite also passes with `-D_GLIBCXX_DEBUG` (bounds-checked containers).

## Real circuits: ISCAS'85 and ISCAS'89

All 47 standard circuits that parse (plus c17 and s27) are in `bench/iscas/` and
`bench/` (source and exclusions in `bench/iscas/README.md`); the per-circuit numbers are in
`bench/results.md`.

- **Provenance check:** the official `s27.bench` is identical, after normalizing comments and
  whitespace, to the netlist that had been typed in from memory earlier; gate counts of every
  circuit match the published figures. `s400.bench` in the distributed archive references an
  undefined net and is rejected by the parser; it is left out and documented.
- **c6288 exposed defect 10** above. The synthetic tests had used unit delays, which are
  exactly representable, so ties were exact there; real circuits with delays like 1.1 and
  1.4 are not. The regression test uses the real file.
- **Speed:** every ISCAS circuit is analyzed end to end (parse, sort, timing, top-10) in about
  10 ms or less; the largest (s38417, 22,179 gates and 1,636 flops) takes 10 ms, of which
  parsing is 8.8 ms. The incremental timer re-times a single-gate edit in 0.6-8 microseconds
  (median), a handful of node evaluations on the larger circuits.
- **Why the parser dominates:** these files are small enough that the whole timing engine is
  faster than reading and tokenizing them. For a tool run once per netlist the parser is
  where the time is; for a tool inside an optimization loop the incremental timer is.
