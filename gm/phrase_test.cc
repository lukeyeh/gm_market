// What counts as saying GM, by example. The cases are gm_bot's own: the two
// bots have to agree on every one of them.

#include "gm/phrase.h"

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace {

// The phrases of a server that has left them alone, counting all day.
std::vector<gm::Phrase> Defaults() {
  std::vector<gm::Phrase> phrases;
  phrases.reserve(gm::kDefaultPhrases.size());
  for (const std::string_view text : gm::kDefaultPhrases) {
    phrases.push_back(gm::Phrase{
        .text = std::string(text),
    });
  }
  return phrases;
}

// A phrase on its own is a GM, however it is capitalised or padded.
TEST(SaysGmTest, APhraseOnItsOwnIsAGm) {
  const std::vector<gm::Phrase> phrases = Defaults();

  EXPECT_TRUE(gm::SaysGm("gm", phrases, 9));
  EXPECT_TRUE(gm::SaysGm("  GM ", phrases, 9));
  EXPECT_TRUE(gm::SaysGm("Good Morning", phrases, 9));
}

// So is a longer message that starts or ends with one.
TEST(SaysGmTest, APhraseAtEitherEndIsAGm) {
  const std::vector<gm::Phrase> phrases = Defaults();

  EXPECT_TRUE(gm::SaysGm("gm everyone!", phrases, 9));
  EXPECT_TRUE(gm::SaysGm("ok, good morning", phrases, 9));
}

// The phrase has to be words of its own, and not buried in the middle.
TEST(SaysGmTest, APhraseInsideAWordOrMidSentenceIsNot) {
  const std::vector<gm::Phrase> phrases = Defaults();

  EXPECT_FALSE(gm::SaysGm("gmail is down", phrases, 9));
  EXPECT_FALSE(gm::SaysGm("program", phrases, 9));
  EXPECT_FALSE(gm::SaysGm("I said gm to them yesterday", phrases, 9));
  EXPECT_FALSE(gm::SaysGm("hello", phrases, 9));
  EXPECT_FALSE(gm::SaysGm("", phrases, 9));
}

// With no phrases nothing is a GM, which is what a server that removed them
// all has asked for.
TEST(SaysGmTest, NothingIsAGmWithoutPhrases) {
  EXPECT_FALSE(gm::SaysGm("gm", {}, 9));
}

// A phrase limited to some hours counts from the first to just before the
// second.
TEST(SaysGmTest, APhraseCountsOnlyDuringItsHours) {
  const std::vector<gm::Phrase> phrases = {
      gm::Phrase{
          .text = "gm",
          .opens = 5,
          .closes = 12,
      },
  };

  EXPECT_FALSE(gm::SaysGm("gm", phrases, 4));
  EXPECT_TRUE(gm::SaysGm("gm", phrases, 5));
  EXPECT_TRUE(gm::SaysGm("gm", phrases, 11));
  EXPECT_FALSE(gm::SaysGm("gm", phrases, 12));
}

// Hours that end before they begin run past midnight.
TEST(SaysGmTest, HoursCanRunPastMidnight) {
  const std::vector<gm::Phrase> phrases = {
      gm::Phrase{
          .text = "gn",
          .opens = 22,
          .closes = 2,
      },
  };

  EXPECT_TRUE(gm::SaysGm("gn", phrases, 23));
  EXPECT_TRUE(gm::SaysGm("gn", phrases, 1));
  EXPECT_FALSE(gm::SaysGm("gn", phrases, 2));
  EXPECT_FALSE(gm::SaysGm("gn", phrases, 12));
}

// A message that says several phrases is a GM if any of them counts now.
TEST(SaysGmTest, AnyPhraseThatCountsNowWillDo) {
  const std::vector<gm::Phrase> phrases = {
      gm::Phrase{
          .text = "gm",
          .opens = 5,
          .closes = 12,
      },
      gm::Phrase{
          .text = "gm gamers",
      },
  };

  EXPECT_TRUE(gm::SaysGm("gm gamers", phrases, 20));
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:phrase_test -- --benchmark_filter=all
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
//   BM_SaysGm        69.7 ns         69.6 ns      6056243
// End of results.
// clang-format on

// The check made on every message in a GM channel: one that is a GM and one
// that is not.
void BM_SaysGm(benchmark::State& state) {
  const std::vector<gm::Phrase> phrases = Defaults();
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        gm::SaysGm("GM everyone, hope the coffee is strong", phrases, 9));
    benchmark::DoNotOptimize(gm::SaysGm(
        "did anyone see the game last night? what a finish", phrases, 9));
  }
}
BENCHMARK(BM_SaysGm);

}  // namespace
