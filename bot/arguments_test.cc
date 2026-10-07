// What people may type into the bot's commands, by example.

#include "bot/arguments.h"

#include <benchmark/benchmark.h>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "discord/model.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "market/lmsr.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using testing::HasSubstr;

// "@luke" typed into a command arrives as a mention, in one of two forms.
// A bare id will do as well.
TEST(ParseMemberTest, ReadsAMentionOrAnId) {
  const discord::UserId luke = {
      .value = 44,
  };

  EXPECT_THAT(bot::ParseMember("<@44>"), IsOkAndHolds(luke));
  EXPECT_THAT(bot::ParseMember("<@!44>"), IsOkAndHolds(luke));
  EXPECT_THAT(bot::ParseMember("  <@44> "), IsOkAndHolds(luke));
  EXPECT_THAT(bot::ParseMember("44"), IsOkAndHolds(luke));
}

// A name alone does not say who, and neither does a role or a channel.
TEST(ParseMemberTest, RefusesWhatIsNotAMember) {
  for (const char* const text :
       {"", "luke", "@luke", "<@44", "<@&44>", "<#44>", "<@0>", "<@>"}) {
    EXPECT_THAT(bot::ParseMember(text),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("mentioning them")))
        << text;
  }
}

// Yes or no, however it is capitalised, or the first letter of either.
TEST(ParseOutcomeTest, ReadsYesAndNo) {
  EXPECT_THAT(bot::ParseOutcome("yes"), IsOkAndHolds(market::Outcome::kYes));
  EXPECT_THAT(bot::ParseOutcome(" YES "), IsOkAndHolds(market::Outcome::kYes));
  EXPECT_THAT(bot::ParseOutcome("y"), IsOkAndHolds(market::Outcome::kYes));
  EXPECT_THAT(bot::ParseOutcome("No"), IsOkAndHolds(market::Outcome::kNo));
  EXPECT_THAT(bot::ParseOutcome("n"), IsOkAndHolds(market::Outcome::kNo));
}

TEST(ParseOutcomeTest, RefusesAnythingElse) {
  for (const char* const text : {"", "maybe", "yess", "1"}) {
    EXPECT_THAT(bot::ParseOutcome(text),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("`yes` or `no`")))
        << text;
  }
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:arguments_test -- --benchmark_filter=all
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
//   BM_ParseMember        26.6 ns         26.6 ns     15890204
//   BM_ParseOutcome       24.0 ns         24.0 ns     17703455
// End of results.
// clang-format on

// Reading the member named in a command, as Discord sends a mention.
void BM_ParseMember(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(bot::ParseMember("<@400000000000000004>"));
  }
}
BENCHMARK(BM_ParseMember);

void BM_ParseOutcome(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(bot::ParseOutcome("Yes"));
    benchmark::DoNotOptimize(bot::ParseOutcome("no"));
  }
}
BENCHMARK(BM_ParseOutcome);

}  // namespace
