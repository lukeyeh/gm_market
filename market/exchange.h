// The exchange: the markets on who will say GM, what everyone holds in them
// and how many gmbux everyone has, kept in a SQLite database.
//
// There is a market for each member and each day, asking whether that member
// (its subject) will say GM that day. It trades the day before and at no
// other time: every trade named here is in the market for the day after
// `today`. Once its own day begins a market only waits for its answer.
//
// A market is settled once, as soon as its answer is known: when the subject
// says GM, or when the day has gone by. Each share of the right answer then
// pays kSharePayout, and the rest pay nothing.
//
// Beside its shares, a market holds a pot for guesses of the time the GM
// will be said. A guess costs kGuessStake. If the subject says GM, whoever
// guessed nearest takes the pot; if not, or if nobody saw when, everyone
// gets their stake back.
//
// What can be done about the market on oneself is limited, since its answer
// is one's own to give: YES shares may be bought, and that is all.
//
// An exchange is divided into communities, which share nothing: the same
// person in two of them has two balances. Everyone starts in each with
// kStartingBalance.

#ifndef MARKET_EXCHANGE_H_
#define MARKET_EXCHANGE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"
#include "sqlite/database.h"

namespace market {

// What everyone has before their first trade.
inline constexpr Gmbux kStartingBalance = Gmbux::Whole(1000);

// What a guess at the time costs, and so what each adds to the pot.
inline constexpr Gmbux kGuessStake = Gmbux::Whole(25);

// Someone who trades. `id` is what identifies them; `name` is what to call
// them, and may change from one trade to the next.
struct Trader {
  uint64_t id = 0;
  std::string name;
};

// Whether `status` is the exchange declining to do what was asked, as
// opposed to failing at it. A refusal's message is a sentence fit to show to
// whoever asked. Its code is ResourceExhausted if they have not enough
// gmbux or shares for it, and OutOfRange if it is not something the market
// allows.
bool IsRefusal(const absl::Status& status);

// How a market stands.
struct Quote {
  // Whose GM it is about, and on which day.
  uint64_t subject = 0;
  absl::CivilDay day;

  Shares sold;
  // The chance, 0 to 1, that the market puts on the subject saying GM.
  double chance = 0.5;

  // How many guesses at the time there are, and what they have staked.
  int guesses = 0;
  Gmbux pot;
};

// What a purchase or a sale came to.
struct Fill {
  // How many shares changed hands, and for how many gmbux.
  int64_t shares = 0;
  Gmbux amount;

  // Where that leaves the trader: what they now hold of that outcome in
  // that market, and their balance.
  int64_t held = 0;
  Gmbux balance;

  // The market afterwards.
  Quote quote;
};

// What placing a guess came to.
struct Ticket {
  // What was staked for it: kGuessStake, or nothing if it replaced an
  // earlier guess, whose stake stands.
  Gmbux staked;
  Gmbux balance;

  // The market afterwards.
  Quote quote;
};

// What someone has riding on one market.
struct Holding {
  uint64_t subject = 0;
  absl::CivilDay day;
  Shares shares;
  std::optional<TimeOfDay> guess;
};

// Someone's balance, and what they hold in markets yet to be settled:
// earliest day first.
struct Portfolio {
  Gmbux balance;
  std::vector<Holding> holdings;
};

struct Account {
  Trader trader;
  Gmbux balance;
};

// Gmbux paid to someone when a market is settled.
struct Payout {
  Trader trader;
  Gmbux amount;
};

// How a market ended.
struct Settlement {
  uint64_t subject = 0;
  absl::CivilDay day;

  // Whether the subject said GM, and when, if that is known.
  Outcome outcome = Outcome::kNo;
  std::optional<TimeOfDay> said_at;

  // How many traders held shares at the end, and what those who held the
  // right ones were paid: largest first.
  int holders = 0;
  std::vector<Payout> winnings;

  // What the guesses had staked, and where it went: to the nearest guess (or
  // guesses, level), or back to everyone who guessed if `pot_returned`.
  Gmbux pot;
  bool pot_returned = false;
  std::vector<Payout> pot_payouts;
  // What the nearest guess was. Empty if the pot was returned.
  std::optional<TimeOfDay> nearest_guess;
};

// One community's part of an exchange.
//
// This is a handle on the exchange it came from, cheap to make and to copy,
// and must not outlive that exchange. Whatever it declines to do it declines
// with a refusal (see IsRefusal), having changed nothing.
class Community {
 public:
  // How tomorrow's market on `subject` stands. One nobody has traded in
  // stands at even odds.
  absl::StatusOr<Quote> QuoteFor(uint64_t subject, absl::CivilDay today);

  // Tomorrow's markets that have shares out or guesses in: most shares sold
  // first.
  absl::StatusOr<std::vector<Quote>> Quotes(absl::CivilDay today);

  // Buys `trader` as many shares of `outcome` in tomorrow's market on
  // `subject` as `budget` covers, and takes what they cost, which may be a
  // little less. Refused if the trader has not that many gmbux, if it buys
  // no shares at all, or if it would be a bet against themselves.
  absl::StatusOr<Fill> Buy(const Trader& trader, uint64_t subject,
                           Outcome outcome, Gmbux budget, absl::CivilDay today);

  // Sells `shares` of the shares of `outcome` that `trader` holds in
  // tomorrow's market on `subject` back to the market, or all of them if
  // not told how many. Refused if they do not hold that many, or any.
  absl::StatusOr<Fill> Sell(const Trader& trader, uint64_t subject,
                            Outcome outcome, std::optional<int64_t> shares,
                            absl::CivilDay today);

  // Has `trader` guess that `subject` will say GM tomorrow at `time`, for
  // kGuessStake. A second guess in the same market replaces the first, at
  // no further cost. Refused if the trader has not the stake, or is the
  // subject.
  absl::StatusOr<Ticket> Guess(const Trader& trader, uint64_t subject,
                               TimeOfDay time, absl::CivilDay today);

  // The balance and holdings of `trader`.
  absl::StatusOr<Portfolio> PortfolioOf(const Trader& trader);

  // Up to `limit` of those who have ever traded, richest first, those level
  // in order of name. Nobody, if `limit` is not positive.
  absl::StatusOr<std::vector<Account>> Richest(int limit);

  // Tells the exchange that `subject` said GM on `day` at `time`, which
  // settles the market on it, if there is one still open. Returns how it
  // ended, or nothing if there was none to settle: nobody had traded in it,
  // or the subject had already said GM that day.
  absl::StatusOr<std::optional<Settlement>> Said(uint64_t subject,
                                                 absl::CivilDay day,
                                                 TimeOfDay time);

  // Settles every market whose day was over before `today` began and which
  // is still open: earliest day first. A market gets this far only if
  // nobody told the exchange of a GM, so `said_gm` is asked whether the
  // subject said one that day all the same. If so the market settles as
  // yes, at a time nobody knows; otherwise as no. Fails, with those markets
  // left open, if `said_gm` does.
  absl::StatusOr<std::vector<Settlement>> Expire(
      absl::CivilDay today,
      absl::FunctionRef<absl::StatusOr<bool>(uint64_t subject,
                                             absl::CivilDay day)>
          said_gm);

 private:
  friend class Exchange;

  Community(sqlite::Database* database, uint64_t id)
      : database_(database), id_(static_cast<int64_t>(id)) {}

  sqlite::Database* database_;
  // As the database stores it.
  int64_t id_;
};

class Exchange {
 public:
  // Opens the exchange kept in `file`, creating it if there is none yet.
  // Fails with FailedPrecondition if a later version of this code wrote it.
  static absl::StatusOr<Exchange> Open(const std::filesystem::path& file);

  // An exchange that is empty and forgotten when destroyed.
  static absl::StatusOr<Exchange> InMemory();

  // The part of the exchange that belongs to the community with this id.
  Community community(uint64_t id) { return Community(database_.get(), id); }

 private:
  explicit Exchange(sqlite::Database database)
      : database_(std::make_unique<sqlite::Database>(std::move(database))) {}

  static absl::StatusOr<Exchange> Prepare(sqlite::Database database);

  // On the heap so that communities' handles survive the exchange being
  // moved.
  std::unique_ptr<sqlite::Database> database_;
};

}  // namespace market

#endif  // MARKET_EXCHANGE_H_
