#include "market/lmsr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "market/gmbux.h"

namespace market {
namespace {

// How deep the market is: the number of shares that have to be bought to
// move the price a long way. With more, prices move less per share and the
// maker stands to lose more: at most kLiquidity * ln 2 payouts a market.
constexpr double kLiquidity = 100.0;

// Slack for arithmetic that should have come out a whole number of
// hundredths, in hundredths, so that rounding does not tip it over.
constexpr double kSlack = 1e-6;

// The rule's cost function, in share payouts: what the maker has taken in
// altogether once it has sold `sold`, up to a constant. Buying and selling
// cost the difference between its values before and after.
//
// It is b * ln(e^(yes/b) + e^(no/b)), written so that large holdings do not
// overflow.
double Cost(const Shares sold) {
  const double yes = static_cast<double>(sold.yes);
  const double no = static_cast<double>(sold.no);

  return std::max(yes, no) +
         kLiquidity * std::log1p(std::exp(-std::abs(yes - no) / kLiquidity));
}

// Hundredths of a gmbux that `payouts` share payouts come to, as a fraction.
double InHundredths(const double payouts) {
  return payouts * static_cast<double>(kSharePayout.hundredths());
}

}  // namespace

Shares Shares::With(const Outcome outcome, const int64_t more) const {
  Shares result = *this;
  (outcome == Outcome::kYes ? result.yes : result.no) += more;
  return result;
}

double ChanceOfYes(const Shares sold) {
  const double lead = static_cast<double>(sold.yes - sold.no) / kLiquidity;

  return 1.0 / (1.0 + std::exp(-lead));
}

Gmbux CostOfBuying(const Shares sold, const Outcome outcome,
                   const int64_t shares) {
  if (shares <= 0) return Gmbux();

  const double cost = Cost(sold.With(outcome, shares)) - Cost(sold);
  const int64_t hundredths =
      static_cast<int64_t>(std::ceil(InHundredths(cost) - kSlack));

  // Shares of what the market is all but sure will not happen are worth
  // next to nothing, but they are not given away.
  return Gmbux::Hundredths(std::max<int64_t>(hundredths, 1));
}

Gmbux ProceedsOfSelling(const Shares sold, const Outcome outcome,
                        const int64_t shares) {
  const double proceeds = Cost(sold) - Cost(sold.With(outcome, -shares));

  return Gmbux::Hundredths(
      static_cast<int64_t>(std::floor(InHundredths(proceeds) + kSlack)));
}

int64_t SharesAffordable(const Shares sold, const Outcome outcome,
                         const Gmbux budget) {
  if (budget <= Gmbux()) return 0;

  // A share never costs more than its payout, so the budget buys at least
  // one for each payout it holds. A share of an outcome the market thinks
  // unlikely costs much less, though: the cost function is at least the
  // larger holding, so `shares` more cost at least that many payouts less
  // what the market has already made the outcome's price fall short by.
  const double payouts = static_cast<double>(budget.hundredths()) /
                         static_cast<double>(kSharePayout.hundredths());
  const double shortfall = Cost(sold) - static_cast<double>(sold.of(outcome));

  // The most that is affordable is the last count whose cost fits, somewhere
  // between those two: costs only rise with the count.
  int64_t fits = budget.hundredths() / kSharePayout.hundredths();
  int64_t too_many = static_cast<int64_t>(std::ceil(payouts + shortfall)) + 2;
  while (too_many - fits > 1) {
    const int64_t middle = fits + (too_many - fits) / 2;
    if (CostOfBuying(sold, outcome, middle) <= budget) {
      fits = middle;
    } else {
      too_many = middle;
    }
  }

  return fits;
}

}  // namespace market
