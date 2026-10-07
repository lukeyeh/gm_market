// How a market's shares are priced: by a market maker that will always sell
// and always buy back, at prices that move with what it has sold (Hanson's
// logarithmic market scoring rule).
//
// A market asks one yes-or-no question. A YES share pays kSharePayout if the
// answer is yes and nothing otherwise, and a NO share the reverse. The price
// of a YES share, as a fraction of that payout, is the market's estimate of
// the chance of yes: it starts at a half, rises as YES shares are bought and
// falls as NO shares are. The two prices always sum to the payout.
//
// The maker can lose gmbux, since it sells cheaply to whoever is first to
// be right. It never loses more than about 70 gmbux on one market, whatever
// is bought.

#ifndef MARKET_LMSR_H_
#define MARKET_LMSR_H_

#include <cstdint>

#include "market/gmbux.h"

namespace market {

enum class Outcome {
  kYes,
  kNo,
};

// What one share of the outcome that happens pays.
inline constexpr Gmbux kSharePayout = Gmbux::Whole(1);

// A number of shares of each outcome: what a market has sold, or what
// someone holds.
struct Shares {
  int64_t yes = 0;
  int64_t no = 0;

  int64_t of(Outcome outcome) const {
    return outcome == Outcome::kYes ? yes : no;
  }

  // These, and `more` of `outcome` besides (or fewer, if negative).
  Shares With(Outcome outcome, int64_t more) const;

  friend bool operator==(const Shares&, const Shares&) = default;
};

// The chance of yes, from 0 to 1, that a market which has sold `sold` puts
// on it: the price of a YES share as a fraction of its payout.
double ChanceOfYes(Shares sold);

// What `shares` more shares of `outcome` cost from a market that has sold
// `sold`, rounded up to a hundredth of a gmbux: so never nothing, however
// unlikely the market thinks the outcome.
Gmbux CostOfBuying(Shares sold, Outcome outcome, int64_t shares);

// What a market that has sold `sold` pays to take back `shares` of them of
// `outcome`, rounded down to a hundredth of a gmbux. Selling back what was
// just bought never returns more than it cost.
Gmbux ProceedsOfSelling(Shares sold, Outcome outcome, int64_t shares);

// The most shares of `outcome` that `budget` buys from a market that has
// sold `sold`. Zero if it does not stretch to one.
int64_t SharesAffordable(Shares sold, Outcome outcome, Gmbux budget);

}  // namespace market

#endif  // MARKET_LMSR_H_
