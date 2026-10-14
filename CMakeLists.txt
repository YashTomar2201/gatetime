cmake_minimum_required(VERSION 3.16)
project(gatetime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic)

option(GATETIME_WERROR "Treat compiler warnings as errors" OFF)
option(GATETIME_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(GATETIME_WERROR)
  add_compile_options(-Werror)
endif()
if(GATETIME_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# Library: everything except main.cpp. Tests and the CLI both link this.
add_library(gatetime_core
  src/netlist.cpp
  src/parser_bench.cpp
  src/topo.cpp
  src/delay_model.cpp
  src/timing.cpp
  src/incremental.cpp
  src/paths.cpp
  src/report.cpp
  src/sim.cpp
  src/dot_export.cpp)
target_include_directories(gatetime_core PUBLIC include)

add_executable(gatetime src/main.cpp)
target_link_libraries(gatetime PRIVATE gatetime_core)
install(TARGETS gatetime RUNTIME DESTINATION bin)

# Benchmark tooling (not needed for the library or tests).
add_executable(gen_random_dag bench/gen_random_dag.cpp)
add_executable(gatetime_bench bench/bench_runner.cpp)
target_link_libraries(gatetime_bench PRIVATE gatetime_core)

enable_testing()
add_executable(gatetime_tests
  tests/test_main.cpp
  tests/test_parser.cpp
  tests/test_topo.cpp
  tests/test_timing.cpp
  tests/test_paths.cpp
  tests/test_report.cpp
  tests/test_sim.cpp
  tests/test_sequential.cpp
  tests/test_incremental.cpp
  tests/test_fuzz.cpp)
target_include_directories(gatetime_tests PRIVATE tests)
target_compile_definitions(gatetime_tests PRIVATE GATETIME_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(gatetime_tests PRIVATE gatetime_core)
add_test(NAME unit COMMAND gatetime_tests)

# End-to-end: run the real executable and check output and exit codes.
add_test(NAME cli
  COMMAND ${CMAKE_COMMAND}
    -DGATETIME=$<TARGET_FILE:gatetime>
    -DSRC=${CMAKE_SOURCE_DIR}
    -DOUT=${CMAKE_BINARY_DIR}
    -P ${CMAKE_SOURCE_DIR}/tests/cli_test.cmake)
