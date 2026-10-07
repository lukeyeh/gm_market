// The load tests, run small: they are measuring instruments, and this
// checks that they measure what they say. For numbers, run //perf:bench.

#include "perf/load.h"

#include <benchmark/benchmark.h>

#include <chrono>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"

namespace {

// The bot deals with every event of an evening's trading, and each one it
// answers has a latency.
TEST(BotLoadTest, DealsWithAnEvening) {
  const absl::StatusOr<LoadResult> result = RunBotLoad(LoadOptions{
      .scene = Scene::kEvening,
      .events = 300,
      .members = 20,
      .history_days = 2,
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->handled, 300);
  // Most of an evening is commands, each of which is answered.
  EXPECT_GT(result->latency.count(), 150);
  EXPECT_LE(result->latency.count(), 300);
  EXPECT_GT(result->events_per_second(), 0);
}

// And with every event of the morning after, most of which are GMs that
// want no answer.
TEST(BotLoadTest, DealsWithAMorning) {
  const absl::StatusOr<LoadResult> result = RunBotLoad(LoadOptions{
      .scene = Scene::kMorning,
      .events = 300,
      .members = 20,
      .history_days = 2,
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->handled, 300);
  EXPECT_GT(result->latency.count(), 0);
  EXPECT_LT(result->latency.count(), 150);
}

// Pacing is honoured: events sent a millisecond apart take at least that
// long in all.
TEST(BotLoadTest, PacesEventsWhenAsked) {
  const absl::StatusOr<LoadResult> result = RunBotLoad(LoadOptions{
      .events = 20,
      .interval = std::chrono::milliseconds(1),
      .members = 5,
      .history_days = 0,
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->handled, 20);
  EXPECT_GE(result->elapsed, std::chrono::milliseconds(19));
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //perf:load_test -- --benchmark_filter=all
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
//   -----------------------------------------------------
//   Benchmark           Time             CPU   Iterations
//   -----------------------------------------------------
//   BM_Evening        112 ms         67.9 ms            7
//   BM_Morning        105 ms         55.3 ms            6
// End of results.
// clang-format on

// A short evening and a short morning, end to end, as a number to watch
// between runs of //perf:bench. The time is for the whole run, setting the
// exchange up included.
void BM_Evening(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(RunBotLoad(LoadOptions{
        .scene = Scene::kEvening,
        .events = 500,
        .members = 50,
        .history_days = 2,
    }));
  }
}
BENCHMARK(BM_Evening)->Unit(benchmark::kMillisecond);

void BM_Morning(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(RunBotLoad(LoadOptions{
        .scene = Scene::kMorning,
        .events = 500,
        .members = 50,
        .history_days = 2,
    }));
  }
}
BENCHMARK(BM_Morning)->Unit(benchmark::kMillisecond);

}  // namespace
