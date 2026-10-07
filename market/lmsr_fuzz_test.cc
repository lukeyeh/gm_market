// The market maker's prices, checked for what must hold of them whatever
// has been sold and whatever is asked for: a price is a chance, nothing
// costs more than it pays, a round trip is never a profit, a budget buys
// exactly as much as fits, and the maker's loss is bounded.
//
//   bazel test //market:lmsr_fuzz_test
//   bazel run --config=fuzz //market:lmsr_fuzz_test -- \
//       --fuzz=LmsrFuzzTest.PricesAreSound --fuzz_for=60s

#include <cstdint>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "market/gmbux.h"
#include "market/lmsr.h"

namespace {

using market::Gmbux;
using market::Outcome;
using market::Shares;

// A hundredth of a gmbux: what rounding may cost.
constexpr Gmbux kRounding = Gmbux::Hundredths(1);

// Whatever a market has sold, buying `shares` more of either outcome is
// priced sensibly.
void PricesAreSound(int64_t yes, int64_t no, bool buy_yes, int64_t shares,
                    int64_t more) {
  const Shares sold = {
      .yes = yes,
      .no = no,
  };
  const Outcome outcome = buy_yes ? Outcome::kYes : Outcome::kNo;
  const Outcome other = buy_yes ? Outcome::kNo : Outcome::kYes;

  // The price is a chance.
  const double chance = market::ChanceOfYes(sold);
  ASSERT_GE(chance, 0.0);
  ASSERT_LE(chance, 1.0);

  // Shares cost something, and never more than they could pay.
  const Gmbux cost = market::CostOfBuying(sold, outcome, shares);
  ASSERT_GT(cost, Gmbux());
  ASSERT_LE(cost, Gmbux::Whole(shares));

  // Buying moves the chance towards what was bought, or leaves it.
  const Shares after = sold.With(outcome, shares);
  if (buy_yes) {
    ASSERT_GE(market::ChanceOfYes(after), chance);
  } else {
    ASSERT_LE(market::ChanceOfYes(after), chance);
  }

  // Selling straight back returns what was paid, less the rounding at most.
  const Gmbux back = market::ProceedsOfSelling(after, outcome, shares);
  ASSERT_LE(back, cost);
  ASSERT_GE(back + kRounding, cost);

  // In two lots it costs what it does in one, give or take the rounding of
  // the second lot.
  const Gmbux rest = market::CostOfBuying(after, outcome, more);
  const Gmbux together = market::CostOfBuying(sold, outcome, shares + more);
  ASSERT_LE(together, cost + rest);
  ASSERT_GE(together + kRounding, cost + rest);

  // A share of each outcome is a sure gmbux, and costs one.
  const Gmbux both = cost + market::CostOfBuying(after, other, shares);
  ASSERT_GE(both, Gmbux::Whole(shares));
  ASSERT_LE(both, Gmbux::Whole(shares) + kRounding + kRounding);
}

// Holdings up to a million shares a side, which is a thousand times what
// anyone starts able to buy, and purchases up to a hundred thousand.
FUZZ_TEST(LmsrFuzzTest, PricesAreSound)
    .WithDomains(fuzztest::InRange<int64_t>(0, 1'000'000),
                 fuzztest::InRange<int64_t>(0, 1'000'000),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::InRange<int64_t>(1, 100'000),
                 fuzztest::InRange<int64_t>(1, 100'000));

// A budget buys the most shares whose cost fits in it: that many fit, and
// one more does not.
void ABudgetBuysExactlyWhatFits(int64_t yes, int64_t no, bool buy_yes,
                                int64_t hundredths) {
  const Shares sold = {
      .yes = yes,
      .no = no,
  };
  const Outcome outcome = buy_yes ? Outcome::kYes : Outcome::kNo;
  const Gmbux budget = Gmbux::Hundredths(hundredths);

  const int64_t shares = market::SharesAffordable(sold, outcome, budget);

  ASSERT_GE(shares, 0);
  if (shares > 0) {
    ASSERT_LE(market::CostOfBuying(sold, outcome, shares), budget);
  }
  if (budget > Gmbux()) {
    ASSERT_GT(market::CostOfBuying(sold, outcome, shares + 1), budget);
  } else {
    ASSERT_EQ(shares, 0);
  }
}

// Budgets from less than nothing to ten million gmbux.
FUZZ_TEST(LmsrFuzzTest, ABudgetBuysExactlyWhatFits)
    .WithDomains(fuzztest::InRange<int64_t>(0, 1'000'000),
                 fuzztest::InRange<int64_t>(0, 1'000'000),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::InRange<int64_t>(-100, 1'000'000'000));

// Whatever one side buys in a market nobody else is in, the maker loses no
// more than 70 gmbux when that side is right.
void TheMakersLossIsBounded(bool buy_yes, int64_t shares) {
  const Outcome outcome = buy_yes ? Outcome::kYes : Outcome::kNo;

  const Gmbux taken_in = market::CostOfBuying(Shares{}, outcome, shares);

  ASSERT_LE(Gmbux::Whole(shares) - taken_in, Gmbux::Whole(70));
}

FUZZ_TEST(LmsrFuzzTest, TheMakersLossIsBounded)
    .WithDomains(fuzztest::Arbitrary<bool>(),
                 fuzztest::InRange<int64_t>(1, 10'000'000));

}  // namespace
