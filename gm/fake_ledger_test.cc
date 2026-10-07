// The fake ledger by example: a test writes what gm_bot would have, and the
// code under test reads it back through gm::Record.

#include "gm/fake_ledger.h"

#include <benchmark/benchmark.h>

#include <utility>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "gm/phrase.h"
#include "gm/record.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::IsOkAndHolds;
using testing::ElementsAre;
using testing::IsEmpty;

// What goes into the fake comes out of a Record over it.
TEST(FakeLedgerTest, HoldsWhatATestPutsInIt) {
  absl::StatusOr<gm::FakeLedger> fake = gm::FakeLedger::InMemory();
  ABSL_ASSERT_OK(fake);
  ABSL_ASSERT_OK(fake->AddPhrase(33, "howdy", 5, 12));
  ABSL_ASSERT_OK(fake->RecordGm(33, 44, absl::CivilDay(2026, 10, 5)));

  absl::StatusOr<gm::Record> record = gm::Record::Over(std::move(*fake).Take());
  ABSL_ASSERT_OK(record);

  EXPECT_THAT(record->Phrases(33), IsOkAndHolds(ElementsAre(gm::Phrase{
                                       .text = "howdy",
                                       .opens = 5,
                                       .closes = 12,
                                   })));
  EXPECT_THAT(record->SaidGm(33, 44, absl::CivilDay(2026, 10, 5)),
              IsOkAndHolds(true));
}

// A community gm_bot has met but left without phrases has none, rather than
// the defaults.
TEST(FakeLedgerTest, CanMeetACommunityWithoutGivingItPhrases) {
  absl::StatusOr<gm::FakeLedger> fake = gm::FakeLedger::InMemory();
  ABSL_ASSERT_OK(fake);
  ABSL_ASSERT_OK(fake->Meet(33));

  absl::StatusOr<gm::Record> record = gm::Record::Over(std::move(*fake).Take());
  ABSL_ASSERT_OK(record);

  EXPECT_THAT(record->Phrases(33), IsOkAndHolds(IsEmpty()));
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:fake_ledger_test -- --benchmark_filter=all
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
//   BM_InMemory     159463 ns       159243 ns         2635
// End of results.
// clang-format on

// What every test that needs a ledger pays to get one.
void BM_InMemory(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(gm::FakeLedger::InMemory());
}
BENCHMARK(BM_InMemory);

}  // namespace
