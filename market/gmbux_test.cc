// Gmbux by example: how amounts are made, added up and shown.

#include "market/gmbux.h"

#include <benchmark/benchmark.h>

#include "gtest/gtest.h"

namespace {

using market::Gmbux;

// A whole gmbux is a hundred hundredths, and the two ways of making an
// amount agree.
TEST(GmbuxTest, IsCountedInHundredths) {
  EXPECT_EQ(Gmbux::Whole(12).hundredths(), 1200);
  EXPECT_EQ(Gmbux::Whole(12), Gmbux::Hundredths(1200));
  EXPECT_EQ(Gmbux(), Gmbux::Whole(0));
}

// Amounts add, subtract and compare as the numbers they are.
TEST(GmbuxTest, AddsSubtractsAndCompares) {
  Gmbux balance = Gmbux::Whole(10);
  balance -= Gmbux::Hundredths(631);
  balance += Gmbux::Hundredths(1);

  EXPECT_EQ(balance, Gmbux::Hundredths(370));
  EXPECT_EQ(Gmbux::Whole(1) + Gmbux::Whole(2), Gmbux::Whole(3));
  EXPECT_EQ(Gmbux::Whole(1) - Gmbux::Whole(2), Gmbux::Whole(-1));
  EXPECT_LT(Gmbux::Hundredths(99), Gmbux::Whole(1));
}

// A whole number of gmbux is shown without a fraction; anything else to
// two places.
TEST(GmbuxTest, DescribesItselfForPeople) {
  EXPECT_EQ(Gmbux::Whole(1000).Describe(), "1000");
  EXPECT_EQ(Gmbux().Describe(), "0");
  EXPECT_EQ(Gmbux::Hundredths(1250).Describe(), "12.50");
  EXPECT_EQ(Gmbux::Hundredths(5).Describe(), "0.05");
  EXPECT_EQ(Gmbux::Hundredths(-5).Describe(), "-0.05");
  EXPECT_EQ(Gmbux::Whole(-3).Describe(), "-3");
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //market:gmbux_test -- --benchmark_filter=all
//
// clang-format off
// Results. Written by perf/record_results.py; do not edit by hand.
//
//   Date      2026-10-06
//   CPU       Intel(R) Core(TM) i7-9700 CPU @ 3.00GHz, 8 cores, L3 12 MiB (1 instance)
//   Memory    31 GB
//   Disk      Samsung SSD 990 EVO Plus 4TB (ext4)
//   System    Linux 7.0.0-38-generic, CPU governor: powersave
//   Compiler  clang version 21.1.8
//   Build     bazel -c opt (-O2), C++20, no exceptions
//   I/O       io_uring, except where a benchmark's name says epoll
//
//   ------------------------------------------------------
//   Benchmark            Time             CPU   Iterations
//   ------------------------------------------------------
//   BM_Describe       92.2 ns         92.2 ns      4561535
// End of results.
// clang-format on

// Done for every amount in every answer: a whole number, and one with a
// fraction.
void BM_Describe(benchmark::State& state) {
  const Gmbux whole = Gmbux::Whole(1000);
  const Gmbux fractional = Gmbux::Hundredths(95012);
  for (auto _ : state) {
    benchmark::DoNotOptimize(whole.Describe());
    benchmark::DoNotOptimize(fractional.Describe());
  }
}
BENCHMARK(BM_Describe);

}  // namespace
