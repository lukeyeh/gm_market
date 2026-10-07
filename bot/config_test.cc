// The bot's configuration by example: which variables it reads and what it
// makes of them.

#include "bot/config.h"

#include <benchmark/benchmark.h>
#include <stdlib.h>  // setenv, which <cstdlib> does not promise.

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "discord/model.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;
using testing::HasSubstr;

// Variables that are exactly `values`.
bot::Variables From(std::map<std::string, std::string> values) {
  return [values = std::move(values)](
             const std::string& name) -> std::optional<std::string> {
    const auto found = values.find(name);
    if (found == values.end()) return std::nullopt;

    return found->second;
  };
}

// The least that will do: a token, the GM channel and gm_bot's ledger. The
// rest has defaults.
TEST(LoadConfigTest, NeedsATokenAChannelAndTheLedger) {
  const absl::StatusOr<bot::Config> config = bot::LoadConfig(From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "22"},
      {"GM_DATABASE", "/data/gm.db"},
  }));

  ABSL_ASSERT_OK(config);
  EXPECT_EQ(config->token, "secret");
  EXPECT_THAT(config->channels, ElementsAre(discord::ChannelId{
                                    .value = 22,
                                }));
  EXPECT_EQ(config->ledger, "/data/gm.db");
  EXPECT_EQ(config->time_zone, absl::UTCTimeZone());
  EXPECT_EQ(config->database, "./predictions.db");
}

TEST(LoadConfigTest, ReadsEveryVariable) {
  const absl::StatusOr<bot::Config> config = bot::LoadConfig(From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "1234567890123456789, 22"},
      {"GM_DATABASE", "/data/gm.db"},
      {"TIMEZONE", "America/New_York"},
      {"DATA_DIR", "/data"},
  }));

  ABSL_ASSERT_OK(config);
  EXPECT_THAT(config->channels, ElementsAre(
                                    discord::ChannelId{
                                        .value = 1234567890123456789,
                                    },
                                    discord::ChannelId{
                                        .value = 22,
                                    }));
  EXPECT_EQ(config->time_zone.name(), "America/New_York");
  EXPECT_EQ(config->database, "/data/predictions.db");
}

// Each of the three it cannot do without is asked for by name. A variable
// set to nothing counts as missing.
TEST(LoadConfigTest, NamesTheVariableThatIsMissing) {
  EXPECT_THAT(
      bot::LoadConfig(From({
          {"GM_CHANNEL_IDS", "22"},
          {"GM_DATABASE", "/data/gm.db"},
      })),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("DISCORD_TOKEN")));
  EXPECT_THAT(bot::LoadConfig(From({
                  {"DISCORD_TOKEN", "secret"},
                  {"GM_CHANNEL_IDS", ""},
                  {"GM_DATABASE", "/data/gm.db"},
              })),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("GM_CHANNEL_IDS")));
  EXPECT_THAT(
      bot::LoadConfig(From({
          {"DISCORD_TOKEN", "secret"},
          {"GM_CHANNEL_IDS", "22"},
      })),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("GM_DATABASE")));
}

// Nonsense is refused at the start rather than discovered later.
TEST(LoadConfigTest, RefusesAChannelOrTimeZoneThatMakesNoSense) {
  EXPECT_THAT(
      bot::LoadConfig(From({
          {"DISCORD_TOKEN", "secret"},
          {"GM_CHANNEL_IDS", "22,general"},
          {"GM_DATABASE", "/data/gm.db"},
      })),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("general")));
  EXPECT_THAT(bot::LoadConfig(From({
                  {"DISCORD_TOKEN", "secret"},
                  {"GM_CHANNEL_IDS", "22"},
                  {"GM_DATABASE", "/data/gm.db"},
                  {"TIMEZONE", "Mars/Olympus_Mons"},
              })),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Mars/Olympus_Mons")));
}

// By default the variables are the process's own.
TEST(LoadConfigTest, ReadsTheEnvironmentUnlessToldOtherwise) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): the test has one thread.
  setenv("GM_MARKET_CONFIG_TEST", "here", 1);

  EXPECT_EQ(bot::Environment("GM_MARKET_CONFIG_TEST"), "here");
  EXPECT_EQ(bot::Environment("GM_MARKET_CONFIG_TEST_UNSET"), std::nullopt);
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:config_test -- --benchmark_filter=all
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
//   --------------------------------------------------------
//   Benchmark              Time             CPU   Iterations
//   --------------------------------------------------------
//   BM_LoadConfig        476 ns          475 ns       863416
// End of results.
// clang-format on

void BM_LoadConfig(benchmark::State& state) {
  const bot::Variables variables = From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "2000000000000000002, 2000000000000000003"},
      {"GM_DATABASE", "/data/gm.db"},
      {"TIMEZONE", "America/New_York"},
  });
  for (auto _ : state) benchmark::DoNotOptimize(bot::LoadConfig(variables));
}
BENCHMARK(BM_LoadConfig);

}  // namespace
