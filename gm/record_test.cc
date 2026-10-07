// Reading gm_bot's ledger by example: which phrases a server has, and
// whether someone said GM on a day.

#include "gm/record.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "gm/fake_ledger.h"
#include "gm/phrase.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "sqlite/database.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using testing::ElementsAre;
using testing::Field;
using testing::HasSubstr;
using testing::IsEmpty;

// A record over what `fake` holds.
gm::Record Over(gm::FakeLedger fake) {
  absl::StatusOr<gm::Record> record = gm::Record::Over(std::move(fake).Take());
  ABSL_EXPECT_OK(record);
  return std::move(*record);
}

gm::FakeLedger NewFake() {
  absl::StatusOr<gm::FakeLedger> fake = gm::FakeLedger::InMemory();
  ABSL_EXPECT_OK(fake);
  return std::move(*fake);
}

// A server gm_bot has not met has the phrases gm_bot will start it with.
TEST(RecordTest, AnUnknownServerHasTheDefaultPhrases) {
  gm::Record record = Over(NewFake());

  EXPECT_THAT(record.Phrases(33),
              IsOkAndHolds(ElementsAre(Field(&gm::Phrase::text, "gm"),
                                       Field(&gm::Phrase::text, "good morning"),
                                       Field(&gm::Phrase::text, "morning"))));
}

// A server's own phrases come back with their hours, in alphabetical order,
// and another server's do not.
TEST(RecordTest, ReadsAServersOwnPhrases) {
  gm::FakeLedger fake = NewFake();
  ABSL_ASSERT_OK(fake.AddPhrase(33, "morning", 5, 12));
  ABSL_ASSERT_OK(fake.AddPhrase(33, "gm"));
  ABSL_ASSERT_OK(fake.AddPhrase(34, "howdy"));
  gm::Record record = Over(std::move(fake));

  EXPECT_THAT(record.Phrases(33), IsOkAndHolds(ElementsAre(
                                      gm::Phrase{
                                          .text = "gm",
                                      },
                                      gm::Phrase{
                                          .text = "morning",
                                          .opens = 5,
                                          .closes = 12,
                                      })));
}

// A server that removed every phrase has none: nothing is a GM there, and
// the defaults do not come back.
TEST(RecordTest, AServerThatRemovedEveryPhraseHasNone) {
  gm::FakeLedger fake = NewFake();
  ABSL_ASSERT_OK(fake.Meet(33));
  gm::Record record = Over(std::move(fake));

  EXPECT_THAT(record.Phrases(33), IsOkAndHolds(IsEmpty()));
}

// A GM is on record for the member, server and day it was said, and no
// other.
TEST(RecordTest, SaysWhetherSomeoneSaidGmOnADay) {
  const absl::CivilDay day(2026, 10, 5);
  gm::FakeLedger fake = NewFake();
  ABSL_ASSERT_OK(fake.RecordGm(33, 44, day));
  gm::Record record = Over(std::move(fake));

  EXPECT_THAT(record.SaidGm(33, 44, day), IsOkAndHolds(true));
  EXPECT_THAT(record.SaidGm(33, 44, day + 1), IsOkAndHolds(false));
  EXPECT_THAT(record.SaidGm(33, 45, day), IsOkAndHolds(false));
  EXPECT_THAT(record.SaidGm(34, 44, day), IsOkAndHolds(false));
}

// The record follows the file: what gm_bot writes after it was opened is
// seen, which is the whole of how the two bots stay in step.
TEST(RecordTest, SeesWhatIsWrittenAfterItWasOpened) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "live.db";
  absl::StatusOr<gm::FakeLedger> fake = gm::FakeLedger::Create(file);
  ABSL_ASSERT_OK(fake);

  absl::StatusOr<gm::Record> record = gm::Record::Open(file);
  ABSL_ASSERT_OK(record);
  ABSL_ASSERT_OK(fake->AddPhrase(33, "howdy"));

  EXPECT_THAT(record->Phrases(33),
              IsOkAndHolds(ElementsAre(Field(&gm::Phrase::text, "howdy"))));
}

// A missing file is an error, not an invitation to create one: the ledger
// is gm_bot's to make.
TEST(RecordTest, RefusesAFileThatIsNotThere) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "absent.db";

  EXPECT_THAT(gm::Record::Open(file), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_FALSE(std::filesystem::exists(file));
}

// A database that is not a ledger of the layout this reads is refused at
// the start, rather than misread later.
TEST(RecordTest, RefusesALedgerOfAnotherLayout) {
  absl::StatusOr<sqlite::Database> database = sqlite::Database::InMemory();
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute("PRAGMA user_version = 4"));

  EXPECT_THAT(
      gm::Record::Over(std::move(*database)),
      StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("layout 4")));
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //gm:record_test -- --benchmark_filter=all
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
//   ----------------------------------------------------------------------
//   Benchmark                            Time             CPU   Iterations
//   ----------------------------------------------------------------------
//   BM_Phrases                        1346 ns         1345 ns       314487
//   BM_PhrasesOfAnUnknownServer        823 ns          823 ns       510024
//   BM_SaidGm/10/30                   6511 ns         6504 ns        64218
//   BM_SaidGm/200/365                59466 ns        59466 ns         6997
// End of results.
// clang-format on

// A ledger in which each of `members` people has said GM on each of the
// `days` days up to the end of 2025, as a record to read. Built once and
// kept: building it is slow, and the benchmark library calls each benchmark
// several times.
gm::Record& Established(int members, int days) {
  static auto* const records =
      new std::map<std::pair<int, int>, std::unique_ptr<gm::Record>>();
  std::unique_ptr<gm::Record>& record = (*records)[{members, days}];
  if (record == nullptr) {
    gm::FakeLedger fake = NewFake();
    ABSL_EXPECT_OK(fake.AddPhrase(33, "gm"));
    ABSL_EXPECT_OK(fake.AddPhrase(33, "good morning"));
    ABSL_EXPECT_OK(fake.AddPhrase(33, "morning", 5, 12));
    for (int day = 0; day < days; ++day) {
      for (int member = 1; member <= members; ++member) {
        ABSL_EXPECT_OK(fake.RecordGm(33, static_cast<uint64_t>(member),
                                     absl::CivilDay(2025, 12, 31) - day));
      }
    }
    record = std::make_unique<gm::Record>(Over(std::move(fake)));
  }
  return *record;
}

// The first argument is how many members the server has and the second how
// many days of GMs each, so the pairs are a small young server and a busy
// established one.
void Sizes(benchmark::internal::Benchmark* benchmark) {
  benchmark->Args({10, 30})->Args({200, 365});
}

// Read for every message in a GM channel: the phrases of a server gm_bot
// knows.
void BM_Phrases(benchmark::State& state) {
  gm::Record& record = Established(10, 30);
  for (auto _ : state) benchmark::DoNotOptimize(record.Phrases(33));
}
BENCHMARK(BM_Phrases);

// The same for a server it has yet to meet, which takes a second question
// to tell from one with no phrases.
void BM_PhrasesOfAnUnknownServer(benchmark::State& state) {
  gm::Record& record = Established(10, 30);
  for (auto _ : state) benchmark::DoNotOptimize(record.Phrases(34));
}
BENCHMARK(BM_PhrasesOfAnUnknownServer);

// Asked once for each market that a day ends on: someone who did say GM
// that day, and someone who did not.
void BM_SaidGm(benchmark::State& state) {
  gm::Record& record = Established(static_cast<int>(state.range(0)),
                                   static_cast<int>(state.range(1)));
  const absl::CivilDay day(2025, 12, 31);
  for (auto _ : state) {
    benchmark::DoNotOptimize(record.SaidGm(33, 1, day));
    benchmark::DoNotOptimize(record.SaidGm(33, 1, day + 1));
  }
}
BENCHMARK(BM_SaidGm)->Apply(Sizes);

}  // namespace
