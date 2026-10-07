// The market maker's prices by example: what a share costs, how buying
// moves the price, and that the maker cannot be milked.

#include "market/lmsr.h"

#include <benchmark/benchmark.h>

#include <cstdint>

#include "gtest/gtest.h"
#include "market/gmbux.h"

namespace {

using market::ChanceOfYes;
using market::CostOfBuying;
using market::Gmbux;
using market::Outcome;
using market::ProceedsOfSelling;
using market::Shares;
using market::SharesAffordable;

// A market nobody has traded in thinks yes and no equally likely.
TEST(LmsrTest, StartsAtEvenOdds) {
  EXPECT_DOUBLE_EQ(ChanceOfYes(Shares{}), 0.5);
}

// Buying an outcome makes the market think it likelier, and the other less.
TEST(LmsrTest, BuyingMovesThePriceTowardsWhatWasBought) {
  EXPECT_GT(ChanceOfYes(Shares{
                .yes = 50,
            }),
            0.5);
  EXPECT_LT(ChanceOfYes(Shares{
                .no = 50,
            }),
            0.5);

  // Equal amounts of each cancel out.
  EXPECT_DOUBLE_EQ(ChanceOfYes(Shares{
                       .yes = 80,
                       .no = 80,
                   }),
                   0.5);
}

// The first share of an even market costs about half its payout, and each
// one after costs more than the last, so a big bet pays worse odds than a
// small one.
TEST(LmsrTest, EachShareCostsMoreThanTheLast) {
  EXPECT_EQ(CostOfBuying(Shares{}, Outcome::kYes, 1), Gmbux::Hundredths(51));

  const Gmbux ten = CostOfBuying(Shares{}, Outcome::kYes, 10);
  const Gmbux next_ten = CostOfBuying(
      Shares{
          .yes = 10,
      },
      Outcome::kYes, 10);
  EXPECT_LT(ten, next_ten);
  EXPECT_LT(next_ten, Gmbux::Whole(10));

  // Bought in two lots or one, it comes to the same within the rounding.
  const Gmbux twenty = CostOfBuying(Shares{}, Outcome::kYes, 20);
  EXPECT_LE(twenty, ten + next_ten);
  EXPECT_GE(twenty + Gmbux::Hundredths(1), ten + next_ten);
}

// A share of what the market is nearly sure of costs nearly its payout, and
// never more.
TEST(LmsrTest, ASureThingCostsItsPayout) {
  const Shares lopsided = {
      .yes = 100000,
  };

  EXPECT_EQ(CostOfBuying(lopsided, Outcome::kYes, 5), Gmbux::Whole(5));
  EXPECT_NEAR(ChanceOfYes(lopsided), 1.0, 1e-9);
}

// A share of what the market is nearly sure will not happen is nearly
// worthless, but is still paid for: found by lmsr_fuzz_test.cc, when the
// price rounded to nothing and the shares were free.
TEST(LmsrTest, ALongShotIsNeverFree) {
  const Shares lopsided = {
      .yes = 1000000,
      .no = 7,
  };

  EXPECT_EQ(CostOfBuying(lopsided, Outcome::kNo, 87722), Gmbux::Hundredths(1));
  EXPECT_EQ(CostOfBuying(lopsided, Outcome::kNo, 0), Gmbux());
}

// Selling back at once what was just bought loses at most the rounding: it
// is never a way to make gmbux.
TEST(LmsrTest, BuyingAndSellingBackIsNeverAProfit) {
  for (const Shares sold : {
           Shares{},
           Shares{
               .yes = 37,
               .no = 5,
           },
           Shares{
               .yes = 3,
               .no = 250,
           },
       }) {
    for (const int64_t shares : {1, 7, 100, 1234}) {
      const Gmbux cost = CostOfBuying(sold, Outcome::kNo, shares);
      const Gmbux back = ProceedsOfSelling(sold.With(Outcome::kNo, shares),
                                           Outcome::kNo, shares);

      EXPECT_LE(back, cost);
      EXPECT_GE(back + Gmbux::Hundredths(1), cost);
    }
  }
}

// However much is bought, the maker's loss on a market is bounded: the
// winners are paid one payout a share, and paid at least that less 70
// gmbux for them.
TEST(LmsrTest, TheMakersLossIsBounded) {
  for (const int64_t shares : {1, 50, 500, 50000}) {
    const Gmbux taken_in = CostOfBuying(Shares{}, Outcome::kYes, shares);
    const Gmbux paid_out = Gmbux::Whole(shares);

    EXPECT_LE(paid_out - taken_in, Gmbux::Whole(70)) << shares;
  }
}

// A budget buys the most shares whose cost fits within it, and no more.
TEST(LmsrTest, ABudgetBuysAsManySharesAsFit) {
  for (const Shares sold : {
           Shares{},
           Shares{
               .yes = 400,
           },
           Shares{
               .no = 400,
           },
       }) {
    for (const Gmbux budget :
         {Gmbux::Whole(1), Gmbux::Whole(50), Gmbux::Whole(100000)}) {
      const int64_t shares = SharesAffordable(sold, Outcome::kYes, budget);

      EXPECT_GT(shares, 0);
      EXPECT_LE(CostOfBuying(sold, Outcome::kYes, shares), budget);
      EXPECT_GT(CostOfBuying(sold, Outcome::kYes, shares + 1), budget);
    }
  }
}

// Fifty gmbux on an even market buys a good deal more than fifty shares,
// since each costs about half a gmbux to begin with.
TEST(LmsrTest, FiftyGmbuxBuysAboutEightyShares) {
  EXPECT_EQ(SharesAffordable(Shares{}, Outcome::kYes, Gmbux::Whole(50)), 83);
}

// Too little for one share, or nothing at all, buys none.
TEST(LmsrTest, TooSmallABudgetBuysNothing) {
  EXPECT_EQ(SharesAffordable(Shares{}, Outcome::kYes, Gmbux()), 0);
  EXPECT_EQ(SharesAffordable(Shares{}, Outcome::kYes, Gmbux::Hundredths(10)),
            0);
  EXPECT_EQ(SharesAffordable(Shares{}, Outcome::kYes, Gmbux::Whole(-5)), 0);
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //market:lmsr_test -- --benchmark_filter=all
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
//   BM_ChanceOfYes                    16.3 ns         16.3 ns     25762082
//   BM_CostOfBuying                   44.1 ns         44.1 ns      9571911
//   BM_ProceedsOfSelling              44.4 ns         44.3 ns      9587037
//   BM_SharesAffordable/1              452 ns          451 ns       945042
//   BM_SharesAffordable/50             512 ns          511 ns       823164
//   BM_SharesAffordable/1000           444 ns          443 ns       942881
//   BM_SharesAffordable/1000000        319 ns          318 ns      1333872
// End of results.
// clang-format on

// A market with a fair amount sold on both sides.
constexpr Shares kBusy = {
    .yes = 830,
    .no = 410,
};

// Worked out for every quote shown.
void BM_ChanceOfYes(benchmark::State& state) {
  Shares sold = kBusy;
  for (auto _ : state) {
    benchmark::DoNotOptimize(sold);
    benchmark::DoNotOptimize(ChanceOfYes(sold));
  }
}
BENCHMARK(BM_ChanceOfYes);

// Pricing a purchase and a sale of a known size.
void BM_CostOfBuying(benchmark::State& state) {
  Shares sold = kBusy;
  for (auto _ : state) {
    benchmark::DoNotOptimize(sold);
    benchmark::DoNotOptimize(CostOfBuying(sold, Outcome::kYes, 83));
  }
}
BENCHMARK(BM_CostOfBuying);

void BM_ProceedsOfSelling(benchmark::State& state) {
  Shares sold = kBusy;
  for (auto _ : state) {
    benchmark::DoNotOptimize(sold);
    benchmark::DoNotOptimize(ProceedsOfSelling(sold, Outcome::kYes, 83));
  }
}
BENCHMARK(BM_ProceedsOfSelling);

// Finding what a budget buys, which is the work of every /bet: for a budget
// of the argument, in gmbux.
void BM_SharesAffordable(benchmark::State& state) {
  Shares sold = kBusy;
  const Gmbux budget = Gmbux::Whole(state.range(0));
  for (auto _ : state) {
    benchmark::DoNotOptimize(sold);
    benchmark::DoNotOptimize(SharesAffordable(sold, Outcome::kNo, budget));
  }
}
BENCHMARK(BM_SharesAffordable)->Arg(1)->Arg(50)->Arg(1000)->Arg(1000000);

}  // namespace
