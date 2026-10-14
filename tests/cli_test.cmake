# End-to-end checks of the gatetime executable's output and exit codes.
# Run as: cmake -DGATETIME=<exe> -DSRC=<repo root> -DOUT=<scratch dir> -P cli_test.cmake

set(C17 "${SRC}/bench/c17.bench")
set(LOOP "${SRC}/tests/data/loop.bench")
set(TMP "${OUT}")

set(failures 0)

function(run_gatetime expected_code expected_text)
  execute_process(
    COMMAND "${GATETIME}" ${ARGN}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  set(all "${out}${err}")
  set(ok TRUE)
  if(NOT rc EQUAL expected_code)
    set(ok FALSE)
  endif()
  if(NOT expected_text STREQUAL "")
    string(FIND "${all}" "${expected_text}" pos)
    if(pos EQUAL -1)
      set(ok FALSE)
    endif()
  endif()
  if(ok)
    message(STATUS "ok   gatetime ${ARGN} -> ${rc}")
  else()
    message(SEND_ERROR "FAIL gatetime ${ARGN}\n  expected exit ${expected_code} and text '${expected_text}'\n  got exit ${rc}\n  output: ${all}")
  endif()
endfunction()

# Timing met / violated.
run_gatetime(0 "Worst slack: 0.000" report "${C17}" --period 3)
run_gatetime(1 "Violating endpoints: 2 / 2" report "${C17}" --period 2)
run_gatetime(0 "defaulted to the smallest period that meets timing" report "${C17}")
run_gatetime(0 "violating=0/2" report "${C17}" --period 3 --quiet)
run_gatetime(0 "Top 2 paths" report "${C17}" --period 3 --top 2)
run_gatetime(0 "Delay model: unit" report "${C17}" --unit-delay)

# Other commands.
run_gatetime(0 "gates=6" check "${C17}")
run_gatetime(0 "edges        12" stats "${C17}")

# Simulator.
run_gatetime(0 "Simulated 64 random vectors (seed 1)" sim "${C17}")
run_gatetime(0 "Simulated 1000 random vectors (seed 42)" sim "${C17}" --random 1000 --seed 42)
run_gatetime(0 "11111  ->  10" sim "${C17}" --vectors "${SRC}/tests/data/c17_vectors.txt")
run_gatetime(0 "01001  ->  11" sim "${C17}" --vectors "${SRC}/tests/data/c17_vectors.txt")
run_gatetime(2 "bad_vectors.txt:3: vector has 3 bits" sim "${C17}" --vectors "${SRC}/tests/data/bad_vectors.txt")
run_gatetime(3 "combinational loop detected" sim "${LOOP}" --random 8)
run_gatetime(64 "exclusive" sim "${C17}" --random 8 --vectors "${SRC}/tests/data/c17_vectors.txt")
run_gatetime(64 "--random needs" sim "${C17}" --random 0)

# Sequential circuits (flops cut the timing graph; setup tightens the D pins).
set(S27 "${SRC}/bench/s27.bench")
run_gatetime(0 "flops=3" report "${S27}")
run_gatetime(0 "Violating endpoints: 0 / 4" report "${S27}" --unit-delay --period 6)
run_gatetime(1 "Violating endpoints: 2 / 4" report "${S27}" --unit-delay --period 5)
run_gatetime(0 "startpoint  G6 (DFF)" report "${S27}")
run_gatetime(0 "flops        3" stats "${S27}")
run_gatetime(0 "flops=3" check "${S27}")
run_gatetime(0 "+ 0.1 per fanout" report "${C17}" --load-factor 0.1)
run_gatetime(0 "Delay model: table" report "${S27}" --setup 0.5 --load-factor 0.05 --period 20)
run_gatetime(64 "cannot be combined" report "${C17}" --unit-delay --load-factor 0.1)
run_gatetime(0 "x 4 cycles" sim "${S27}" --random 128 --cycles 4 --seed 5)
run_gatetime(0 "clock cycles (one sequence" sim "${S27}" --vectors "${SRC}/tests/data/s27_vectors.txt")

# Incremental timing (ECO): re-time after changing gate delays, verified against a full run.
run_gatetime(1 "Verified: identical to a full recomputation" eco "${C17}" --period 3 --set 16=2.5)
run_gatetime(0 "Verified: identical to a full recomputation" eco "${C17}" --period 3 --set 10=1.2)
run_gatetime(0 "Verified: identical to a full recomputation" eco "${S27}" --set G8=0.1 --set G9=3 --period 20)
run_gatetime(2 "no net named 'nosuch'" eco "${C17}" --set nosuch=1)
run_gatetime(64 "eco needs at least one --set" eco "${C17}")
run_gatetime(64 "--set needs NET=DELAY" eco "${C17}" --set 16)
run_gatetime(64 "--set needs NET=DELAY" eco "${C17}" --set 16=-1)

# Combinational loop.
run_gatetime(3 "combinational loop detected" check "${LOOP}")
run_gatetime(3 "combinational loop detected" report "${LOOP}")

# Errors.
run_gatetime(2 "cannot open file" check "${SRC}/no/such/file.bench")
run_gatetime(2 "cannot open delay file" report "${C17}" --delay-file "${SRC}/no/such/delays.txt")
run_gatetime(64 "unknown option" report "${C17}" --bogus)
run_gatetime(64 "expected a command and a netlist file" report)
run_gatetime(64 "missing <netlist.bench>" report --quiet)
run_gatetime(64 "unknown command" frobnicate "${C17}")
run_gatetime(64 "--period needs" report "${C17}" --period abc)
run_gatetime(0 "" report "${C17}" --delay-file "${SRC}/bench/delays.txt" --period 3)

# DOT export.
set(DOT "${TMP}/cli_test_out.dot")
file(REMOVE "${DOT}")
run_gatetime(0 "" report "${C17}" --period 3 --dot "${DOT}")
if(EXISTS "${DOT}")
  file(READ "${DOT}" dot_text)
  string(FIND "${dot_text}" "digraph gatetime" p)
  if(p EQUAL -1)
    message(SEND_ERROR "FAIL DOT file is not a digraph")
  else()
    message(STATUS "ok   DOT file written")
  endif()
  file(REMOVE "${DOT}")
else()
  message(SEND_ERROR "FAIL DOT file was not created")
endif()

# DOT export of a sequential circuit draws the clocked D -> Q connection dashed.
run_gatetime(0 "" report "${S27}" --dot "${DOT}")
file(READ "${DOT}" dot_text)
string(FIND "${dot_text}" "label=\"DFF\"" p)
if(p EQUAL -1)
  message(SEND_ERROR "FAIL sequential DOT has no dashed DFF edges")
else()
  message(STATUS "ok   sequential DOT has DFF edges")
endif()
file(REMOVE "${DOT}")
