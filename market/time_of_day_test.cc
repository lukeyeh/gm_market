// Times of day by example: how people may write one, and how far apart two
// of them are.

#include "market/time_of_day.h"

#include <benchmark/benchmark.h>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/time/civil_time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using market::TimeOfDay;
using testing::HasSubstr;
using testing::Property;

// On the 24-hour clock, with or without minutes or a leading zero.
TEST(TimeOfDayTest, ReadsTheTwentyFourHourClock) {
  EXPECT_THAT(TimeOfDay::Parse("7:45"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 7 * 60 + 45)));
  EXPECT_THAT(TimeOfDay::Parse("07:45"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 7 * 60 + 45)));
  EXPECT_THAT(TimeOfDay::Parse(" 19:30 "),
              IsOkAndHolds(Property(&TimeOfDay::minute, 19 * 60 + 30)));
  EXPECT_THAT(TimeOfDay::Parse("7"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 7 * 60)));
  EXPECT_THAT(TimeOfDay::Parse("0:00"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 0)));
  EXPECT_THAT(TimeOfDay::Parse("23:59"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 1439)));
}

// With am or pm, in either case, where twelve starts each half of the day.
TEST(TimeOfDayTest, ReadsAmAndPm) {
  EXPECT_THAT(TimeOfDay::Parse("7:45pm"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 19 * 60 + 45)));
  EXPECT_THAT(TimeOfDay::Parse("7 AM"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 7 * 60)));
  EXPECT_THAT(TimeOfDay::Parse("12am"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 0)));
  EXPECT_THAT(TimeOfDay::Parse("12:30pm"),
              IsOkAndHolds(Property(&TimeOfDay::minute, 12 * 60 + 30)));
}

// Anything else is refused, in words for the person who wrote it.
TEST(TimeOfDayTest, RefusesWhatIsNotATime) {
  for (const char* const text :
       {"", "soon", "24:00", "7:60", "7:5", "13pm", "0am", "-1", "7:45:10"}) {
    EXPECT_THAT(
        TimeOfDay::Parse(text),
        StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("7:45pm")))
        << text;
  }
}

// The time of day of a moment is its hour and minute.
TEST(TimeOfDayTest, IsTakenFromAMoment) {
  const TimeOfDay time(absl::CivilMinute(2026, 10, 6, 8, 15));

  EXPECT_EQ(time.minute(), 8 * 60 + 15);
  EXPECT_EQ(time.Describe(), "08:15");
}

// A stored time comes back as it was, and nonsense is kept inside the day.
TEST(TimeOfDayTest, IsStoredAsMinutesAfterMidnight) {
  EXPECT_EQ(TimeOfDay::AfterMidnight(465).Describe(), "07:45");
  EXPECT_EQ(TimeOfDay::AfterMidnight(-5), TimeOfDay());
  EXPECT_EQ(TimeOfDay::AfterMidnight(5000).Describe(), "23:59");
}

// Distance is measured within the day, in either direction.
TEST(TimeOfDayTest, MeasuresHowFarApartTwoTimesAre) {
  const TimeOfDay early = TimeOfDay::AfterMidnight(1);
  const TimeOfDay late = TimeOfDay::AfterMidnight(1439);

  EXPECT_EQ(early.MinutesFrom(late), 1438);
  EXPECT_EQ(late.MinutesFrom(early), 1438);
  EXPECT_EQ(early.MinutesFrom(early), 0);
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //market:time_of_day_test -- --benchmark_filter=all
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
//   ----------------------------------------------------------
//   Benchmark                Time             CPU   Iterations
//   ----------------------------------------------------------
//   BM_Parse              86.9 ns         86.8 ns      4902831
//   BM_ParseRefusal       58.7 ns         58.6 ns      7287220
//   BM_DescribeTime       78.6 ns         78.6 ns      5260972
// End of results.
// clang-format on

// Reading what someone typed into /guess, each way they might.
void BM_Parse(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(TimeOfDay::Parse("7:45"));
    benchmark::DoNotOptimize(TimeOfDay::Parse(" 7:45 PM "));
  }
}
BENCHMARK(BM_Parse);

void BM_ParseRefusal(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(TimeOfDay::Parse("early"));
}
BENCHMARK(BM_ParseRefusal);

void BM_DescribeTime(benchmark::State& state) {
  const TimeOfDay time = TimeOfDay::AfterMidnight(465);
  for (auto _ : state) benchmark::DoNotOptimize(time.Describe());
}
BENCHMARK(BM_DescribeTime);

}  // namespace
