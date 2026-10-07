// What the bot says, by example: each answer and announcement as the text
// Discord is sent.

#include "bot/report.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/time/civil_time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "market/exchange.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"

namespace {

using market::Gmbux;
using market::Outcome;
using market::TimeOfDay;
using testing::Eq;
using testing::Optional;

constexpr absl::CivilDay kMonday(2026, 10, 5);
constexpr absl::CivilDay kTuesday = kMonday + 1;

TimeOfDay At(int hour, int minute) {
  return TimeOfDay::AfterMidnight(hour * 60 + minute);
}

// Tomorrow's market on member 44, having sold `sold`, with `guesses`
// guesses in its pot.
market::Quote QuoteOn44(market::Shares sold, int guesses = 0) {
  return market::Quote{
      .subject = 44,
      .day = kTuesday,
      .sold = sold,
      .chance = market::ChanceOfYes(sold),
      .guesses = guesses,
      .pot = Gmbux::Whole(int64_t{25} * guesses),
  };
}

market::Payout PayoutTo(const char* name, Gmbux amount) {
  return market::Payout{
      .trader =
          market::Trader{
              .name = name,
          },
      .amount = amount,
  };
}

// A refusal is marked so that it cannot be mistaken for success.
TEST(ReportTest, MarksARefusal) {
  EXPECT_EQ(bot::Refusal("You only have 3 gmbux."),
            "❌ You only have 3 gmbux.");
}

// A market nobody has touched is at even odds and says so.
TEST(ReportTest, QuotesAnUntradedMarketAtEvenOdds) {
  EXPECT_EQ(bot::QuoteReport(QuoteOn44({})),
            "**Will <@44> say GM tomorrow?**\n"
            "YES 50% · NO 50%\n"
            "No shares are held yet.");
}

// One that has been traded shows where the odds have moved to, what is
// held, and the pot if there is one.
TEST(ReportTest, QuotesATradedMarket) {
  EXPECT_EQ(bot::QuoteReport(QuoteOn44(
                {
                    .yes = 83,
                    .no = 10,
                },
                2)),
            "**Will <@44> say GM tomorrow?**\n"
            "YES 67% · NO 33%\n"
            "83 YES and 10 NO shares are held.\n"
            "The time pot is 50 gmbux, on 2 guesses.");
}

// Odds are never shown as certain while the market is open, however
// lopsided it gets.
TEST(ReportTest, NeverQuotesACertainty) {
  EXPECT_THAT(bot::QuoteReport(QuoteOn44({
                  .yes = 5000,
              })),
              testing::HasSubstr("YES 99% · NO 1%"));
}

// The list of markets has a line for each, or says how to start one.
TEST(ReportTest, ListsTheMarkets) {
  market::Quote other = QuoteOn44({
      .no = 1,
  });
  other.subject = 45;
  const std::vector<market::Quote> quotes = {
      QuoteOn44(
          {
              .yes = 83,
          },
          1),
      other,
  };

  EXPECT_EQ(bot::MarketsReport(quotes),
            "**Tomorrow's markets**\n"
            "• <@44>: YES 70% · NO 30% · 83 shares · pot 25 gmbux, on 1 guess\n"
            "• <@45>: YES 50% · NO 50% · 1 share");
  EXPECT_EQ(bot::MarketsReport({}),
            "There are no markets for tomorrow yet. `/bet` or `/guess` opens "
            "one.");
}

// A purchase says what was bought, for how much, and where that leaves the
// market and the buyer.
TEST(ReportTest, ReportsAPurchase) {
  const market::Fill fill = {
      .shares = 83,
      .amount = Gmbux::Hundredths(4988),
      .held = 83,
      .balance = Gmbux::Hundredths(95012),
      .quote = QuoteOn44({
          .yes = 83,
      }),
  };

  EXPECT_EQ(bot::BoughtReport(fill, Outcome::kYes),
            "✅ Bought **83 YES shares** on <@44> saying GM tomorrow, for "
            "**49.88 gmbux**. The market now says YES 70% · NO 30%. You hold "
            "83 and have 950.12 gmbux.");
}

// A sale likewise.
TEST(ReportTest, ReportsASale) {
  const market::Fill fill = {
      .shares = 1,
      .amount = Gmbux::Hundredths(48),
      .held = 0,
      .balance = Gmbux::Whole(1000),
      .quote = QuoteOn44({}),
  };

  EXPECT_EQ(bot::SoldReport(fill, Outcome::kNo),
            "✅ Sold **1 NO share** on <@44> saying GM tomorrow, for **0.48 "
            "gmbux**. The market now says YES 50% · NO 50%. You hold 0 and "
            "have 1000 gmbux.");
}

// A guess says what it cost and what the pot has come to. A changed guess
// cost nothing, and says only what it is now.
TEST(ReportTest, ReportsAGuess) {
  const market::Ticket first = {
      .staked = market::kGuessStake,
      .balance = Gmbux::Whole(975),
      .quote = QuoteOn44({}, 2),
  };
  const market::Ticket changed = {
      .balance = Gmbux::Whole(975),
      .quote = QuoteOn44({}, 2),
  };

  EXPECT_EQ(bot::GuessReport(first, At(7, 45)),
            "🕖 You guessed that <@44> will say GM at **07:45** tomorrow, for "
            "25 gmbux. The pot is 50 gmbux, on 2 guesses. You have 975 "
            "gmbux.");
  EXPECT_EQ(bot::GuessReport(changed, At(8, 10)),
            "🕖 Your guess for <@44> is now **08:10**. The pot is 50 gmbux, "
            "on 2 guesses.");
}

// A portfolio is the balance and a line for each market with something
// riding on it, saying which day's market it is.
TEST(ReportTest, ReportsAPortfolio) {
  const market::Portfolio portfolio = {
      .balance = Gmbux::Hundredths(95012),
      .holdings =
          {
              market::Holding{
                  .subject = 46,
                  .day = kMonday,
                  .shares =
                      {
                          .no = 12,
                      },
              },
              market::Holding{
                  .subject = 44,
                  .day = kTuesday,
                  .shares =
                      {
                          .yes = 83,
                          .no = 2,
                      },
                  .guess = At(7, 45),
              },
              market::Holding{
                  .subject = 47,
                  .day = kTuesday,
                  .guess = At(9, 0),
              },
          },
  };

  EXPECT_EQ(bot::PortfolioReport(portfolio, "<@45>", kMonday),
            "<@45>, you have **950.12 gmbux**.\n"
            "• <@46> today: 12 NO\n"
            "• <@44> tomorrow: 83 YES and 2 NO · guessed 07:45\n"
            "• <@47> tomorrow: guessed 09:00");
  EXPECT_EQ(bot::PortfolioReport(
                market::Portfolio{
                    .balance = Gmbux::Whole(1),
                },
                "<@45>", kMonday),
            "<@45>, you have **1 gmbux**.\n"
            "Nothing is riding on any market.");
}

// The rich list numbers the traders it is given.
TEST(ReportTest, ReportsTheRichList) {
  const std::vector<market::Account> richest = {
      market::Account{
          .trader =
              market::Trader{
                  .name = "ada",
              },
          .balance = Gmbux::Hundredths(104020),
      },
      market::Account{
          .trader =
              market::Trader{
                  .name = "grace",
              },
          .balance = Gmbux::Whole(975),
      },
  };

  EXPECT_EQ(bot::RichListReport(richest),
            "**The rich list**\n"
            "1. **ada**: 1040.20 gmbux\n"
            "2. **grace**: 975 gmbux");
  EXPECT_EQ(bot::RichListReport({}), "Nobody has traded yet.");
}

// A GM settles its market as yes: the announcement says when it was said,
// who was paid for their shares, and who took the pot.
TEST(ReportTest, AnnouncesAGm) {
  const market::Settlement settlement = {
      .subject = 44,
      .day = kTuesday,
      .outcome = Outcome::kYes,
      .said_at = At(8, 15),
      .holders = 3,
      .winnings =
          {
              PayoutTo("ada", Gmbux::Whole(83)),
              PayoutTo("edsger", Gmbux::Whole(1)),
          },
      .pot = Gmbux::Whole(50),
      .pot_payouts =
          {
              PayoutTo("grace", Gmbux::Whole(50)),
          },
      .nearest_guess = At(9, 0),
  };

  EXPECT_THAT(bot::SettlementAnnouncement(settlement),
              Optional(Eq("📈 <@44> said GM at **08:15**: YES it is.\n"
                          "Paid: **ada** 83 gmbux, **edsger** 1 gmbux.\n"
                          "⏱️ **grace** guessed nearest, with 09:00, and takes "
                          "the time pot of 50 gmbux.")));
}

// Guesses that were equally near are named together.
TEST(ReportTest, AnnouncesASharedPot) {
  const market::Settlement settlement = {
      .subject = 44,
      .day = kTuesday,
      .outcome = Outcome::kYes,
      .said_at = At(8, 15),
      .pot = Gmbux::Whole(100),
      .pot_payouts =
          {
              PayoutTo("ada", Gmbux::Hundredths(3334)),
              PayoutTo("grace", Gmbux::Hundredths(3333)),
              PayoutTo("edsger", Gmbux::Hundredths(3333)),
          },
      .nearest_guess = At(8, 0),
  };

  EXPECT_THAT(bot::SettlementAnnouncement(settlement),
              Optional(Eq("📈 <@44> said GM at **08:15**: YES it is.\n"
                          "⏱️ **ada**, **grace** and **edsger** guessed equally "
                          "near and share the time pot of 100 gmbux.")));
}

// A day without a GM settles as no, and the pot goes back.
TEST(ReportTest, AnnouncesADayWithoutAGm) {
  const market::Settlement settlement = {
      .subject = 44,
      .day = kTuesday,
      .outcome = Outcome::kNo,
      .holders = 1,
      .pot = Gmbux::Whole(50),
      .pot_returned = true,
      .pot_payouts =
          {
              PayoutTo("ada", Gmbux::Whole(25)),
              PayoutTo("grace", Gmbux::Whole(25)),
          },
  };

  EXPECT_THAT(bot::SettlementAnnouncement(settlement),
              Optional(Eq("📉 <@44> did not say GM on 2026-10-06: NO it is.\n"
                          "Nobody held the winning side.\n"
                          "⏱️ The time pot of 50 gmbux goes back to those who "
                          "guessed.")));
}

// A GM that only gm_bot saw settles as yes on a day, with no time to give.
TEST(ReportTest, AnnouncesAGmNobodySaw) {
  const market::Settlement settlement = {
      .subject = 44,
      .day = kTuesday,
      .outcome = Outcome::kYes,
      .holders = 1,
      .winnings =
          {
              PayoutTo("ada", Gmbux::Whole(83)),
          },
  };

  EXPECT_THAT(bot::SettlementAnnouncement(settlement),
              Optional(Eq("📈 <@44> said GM on 2026-10-06: YES it is.\n"
                          "Paid: **ada** 83 gmbux.")));
}

// A market everyone had sold out of ends without a word.
TEST(ReportTest, SaysNothingOfAMarketNobodyWasIn) {
  const market::Settlement settlement = {
      .subject = 44,
      .day = kTuesday,
      .outcome = Outcome::kYes,
      .said_at = At(8, 15),
  };

  EXPECT_EQ(bot::SettlementAnnouncement(settlement), std::nullopt);
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:report_test -- --benchmark_filter=all
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
//   --------------------------------------------------------------------
//   Benchmark                          Time             CPU   Iterations
//   --------------------------------------------------------------------
//   BM_QuoteReport                   196 ns          196 ns      2155195
//   BM_BoughtReport                  330 ns          330 ns      1284336
//   BM_MarketsReport                2526 ns         2524 ns       169857
//   BM_RichListReport               2975 ns         2975 ns       141001
//   BM_SettlementAnnouncement        968 ns          967 ns       441179
// End of results.
// clang-format on

// The answer to /market, and to /bet.
void BM_QuoteReport(benchmark::State& state) {
  const market::Quote quote = QuoteOn44(
      {
          .yes = 83,
          .no = 10,
      },
      2);
  for (auto _ : state) benchmark::DoNotOptimize(bot::QuoteReport(quote));
}
BENCHMARK(BM_QuoteReport);

void BM_BoughtReport(benchmark::State& state) {
  const market::Fill fill = {
      .shares = 83,
      .amount = Gmbux::Hundredths(4988),
      .held = 83,
      .balance = Gmbux::Hundredths(95012),
      .quote = QuoteOn44({
          .yes = 83,
      }),
  };
  for (auto _ : state) {
    benchmark::DoNotOptimize(bot::BoughtReport(fill, Outcome::kYes));
  }
}
BENCHMARK(BM_BoughtReport);

// The lists: as many markets, and as many traders, as one message shows.
void BM_MarketsReport(benchmark::State& state) {
  const std::vector<market::Quote> quotes(15, QuoteOn44(
                                                  {
                                                      .yes = 83,
                                                      .no = 10,
                                                  },
                                                  2));
  for (auto _ : state) benchmark::DoNotOptimize(bot::MarketsReport(quotes));
}
BENCHMARK(BM_MarketsReport);

void BM_RichListReport(benchmark::State& state) {
  const std::vector<market::Account> richest(
      25, market::Account{
              .trader =
                  market::Trader{
                      .name = "someone",
                  },
              .balance = Gmbux::Hundredths(104020),
          });
  for (auto _ : state) benchmark::DoNotOptimize(bot::RichListReport(richest));
}
BENCHMARK(BM_RichListReport);

// The announcement of a market ten people were in, with a pot.
void BM_SettlementAnnouncement(benchmark::State& state) {
  const market::Settlement settlement = {
      .subject = 44,
      .day = kTuesday,
      .outcome = Outcome::kYes,
      .said_at = At(8, 15),
      .holders = 10,
      .winnings =
          std::vector<market::Payout>(6, PayoutTo("someone", Gmbux::Whole(83))),
      .pot = Gmbux::Whole(100),
      .pot_payouts =
          {
              PayoutTo("grace", Gmbux::Whole(100)),
          },
      .nearest_guess = At(9, 0),
  };
  for (auto _ : state) {
    benchmark::DoNotOptimize(bot::SettlementAnnouncement(settlement));
  }
}
BENCHMARK(BM_SettlementAnnouncement);

}  // namespace
