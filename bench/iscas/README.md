# ISCAS benchmark circuits

Files in this directory are the standard ISCAS'85 (combinational) and ISCAS'89 (sequential)
benchmark netlists in `.bench` format, used by `bench/run_bench.sh` and the benchmarks in
`bench/results.md`.

**Source:** `ISCAS.7z` (6.9 MB) from the benchmark archive of the Digital Design and
Dependability group at CTU in Prague, https://ddd.fit.cvut.cz/www/prj/Benchmarks/ ,
downloaded 2026-10-13; the `bench/` directory of the archive. The files are unmodified,
including their original comment headers (some use lower-case gate names such as `nand`,
which the parser accepts).

**Included:** the ten ISCAS'85 circuits (c432, c499, c880, c1355, c1908, c2670, c3540, c5315,
c6288, c7552) and the 37 ISCAS'89 circuits named `s<number>.bench` that parse cleanly
(`s27` is `bench/s27.bench` one level up). `c17` is kept one level up in
`bench/c17.bench` with simplified net names.

**Left out:**

- `s400.bench`: the file in this archive references the net `Phi1H` on line 97 and never
  defines it, so GateTime rejects it with `net 'Phi1H' is used but never driven`. The
  parser is right; the distributed file is incomplete.
- The `.1` variants (`s208.1` ...), `s526n`, `prolog.bench` and the `iscas89_scan`
  directory (scan-chain-inserted versions): not part of the standard set.
- Other formats in the archive (BLIF, Verilog, EDIF, ISC).

The benchmarks are distributed for research use; keep this note when redistributing.
