// The exchange, checked against a model.
//
// The exchange keeps its books in SQL, a transaction to a trade. The model
// here keeps them the obvious way, in maps, taking only the price of a share
// from the market maker (which lmsr_fuzz_test.cc checks on its own). A fuzz
// test puts both through the same arbitrary sequence of purchases, sales,
// guesses, GMs seen and unseen, and days going by, spread over two
// communities that must not affect each other, and requires them to agree on
// everything the exchange can be asked: what each trade came to or why it
// was refused, how each market was settled and who was paid, and after every
// step each trader's balance and holdings, every quote, and the rich list.
//
//   bazel test //market:exchange_fuzz_test
//       Tries the property on a few thousand random inputs.
//
//   bazel run --config=fuzz //market:exchange_fuzz_test -- \
//       --fuzz=ExchangeFuzzTest.AgreesWithTheModel --fuzz_for=60s
//       Searches for a counterexample for as long as asked, steered by which
//       code each input reaches.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "market/exchange.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"

namespace {

using market::Gmbux;
using market::Outcome;
using market::Shares;
using market::TimeOfDay;

// Who trades and whom the markets are about, by index. The ids span what a
// 64-bit id can be.
constexpr std::array<uint64_t, 4> kIds = {
    1,
    42,
    1234567890123456789,
    std::numeric_limits<uint64_t>::max(),
};

// The two communities things happen in.
constexpr std::array<uint64_t, 2> kCommunities = {
    33,
    std::numeric_limits<uint64_t>::max(),
};

// The day every history starts on.
constexpr absl::CivilDay kFirstDay(2026, 10, 5);

// One thing that happens. Which of the other fields matter depends on
// `kind`.
struct Step {
  enum Kind : uint8_t {
    kBuy,
    kSell,
    kGuess,
    kSaid,
    kSaidUnseen,
    kNextDay,
    kExpire,
    kLookAround,
    kKinds,
  };

  int kind = 0;
  // Indexes into kCommunities and kIds.
  int community = 0;
  int trader = 0;
  int subject = 0;
  // The trader's name at the time.
  std::string name;
  // 0 for YES, 1 for NO.
  int outcome = 0;
  // A budget in hundredths, or a number of shares.
  int amount = 0;
  // Whether a sale is of everything held, whatever `amount` says.
  bool everything = false;
  // A time of day, in minutes after midnight, give or take.
  int minute = 0;
  // Whether a GM is said to have been on the day after today, which no real
  // one is, and which settles a market that is still trading.
  bool ahead = false;
};

// What the exchange said, in forms that compare and print.
//
// A quote: subject, day, YES sold, NO sold, guesses, pot.
using SeenQuote =
    std::tuple<uint64_t, absl::CivilDay, int64_t, int64_t, int, int64_t>;
// A purchase or sale: its status, shares, amount, held after, balance after,
// and the quote after. All zero when refused.
using SeenFill =
    std::tuple<absl::StatusCode, int64_t, int64_t, int64_t, int64_t, SeenQuote>;
// A guess: its status, what was staked, balance after, quote after.
using SeenTicket = std::tuple<absl::StatusCode, int64_t, int64_t, SeenQuote>;
// A payout: to whom, by what name, how much.
using SeenPayout = std::tuple<uint64_t, std::string, int64_t>;
// A settlement: subject, day, whether yes, when said (-1 if unknown),
// holders, winnings, pot, whether returned, pot payouts, nearest guess (-1
// if none).
using SeenSettlement = std::tuple<uint64_t, absl::CivilDay, bool, int, int,
                                  std::vector<SeenPayout>, int64_t, bool,
                                  std::vector<SeenPayout>, int>;
// A holding: subject, day, YES, NO, guess (-1 if none).
using SeenHolding = std::tuple<uint64_t, absl::CivilDay, int64_t, int64_t, int>;
using SeenPortfolio = std::pair<int64_t, std::vector<SeenHolding>>;
// An account: whose, by what name, balance.
using SeenAccount = std::tuple<uint64_t, std::string, int64_t>;

int MinuteOf(const std::optional<TimeOfDay>& time) {
  return time.has_value() ? time->minute() : -1;
}

SeenQuote Seen(const market::Quote& quote) {
  // The chance is the market maker's, of what has been sold.
  EXPECT_EQ(quote.chance, market::ChanceOfYes(quote.sold));

  return {quote.subject, quote.day,     quote.sold.yes,
          quote.sold.no, quote.guesses, quote.pot.hundredths()};
}

SeenFill Seen(const absl::StatusOr<market::Fill>& fill) {
  if (!fill.ok()) return {fill.status().code(), 0, 0, 0, 0, SeenQuote()};

  return {absl::StatusCode::kOk,      fill->shares,
          fill->amount.hundredths(),  fill->held,
          fill->balance.hundredths(), Seen(fill->quote)};
}

SeenTicket Seen(const absl::StatusOr<market::Ticket>& ticket) {
  if (!ticket.ok()) return {ticket.status().code(), 0, 0, SeenQuote()};

  return {absl::StatusCode::kOk, ticket->staked.hundredths(),
          ticket->balance.hundredths(), Seen(ticket->quote)};
}

std::vector<SeenPayout> Seen(const std::vector<market::Payout>& payouts) {
  std::vector<SeenPayout> seen;
  seen.reserve(payouts.size());
  for (const market::Payout& payout : payouts) {
    seen.emplace_back(payout.trader.id, payout.trader.name,
                      payout.amount.hundredths());
  }
  return seen;
}

SeenSettlement Seen(const market::Settlement& settlement) {
  return {settlement.subject,
          settlement.day,
          settlement.outcome == Outcome::kYes,
          MinuteOf(settlement.said_at),
          settlement.holders,
          Seen(settlement.winnings),
          settlement.pot.hundredths(),
          settlement.pot_returned,
          Seen(settlement.pot_payouts),
          MinuteOf(settlement.nearest_guess)};
}

SeenPortfolio Seen(const market::Portfolio& portfolio) {
  std::vector<SeenHolding> holdings;
  holdings.reserve(portfolio.holdings.size());
  for (const market::Holding& holding : portfolio.holdings) {
    holdings.emplace_back(holding.subject, holding.day, holding.shares.yes,
                          holding.shares.no, MinuteOf(holding.guess));
  }
  return {portfolio.balance.hundredths(), std::move(holdings)};
}

// One community of an exchange, kept the obvious way.
class Model {
 public:
  SeenQuote QuoteFor(uint64_t subject, absl::CivilDay today) const {
    const absl::CivilDay day = today + 1;
    const auto found = markets_.find({day, subject});

    return found == markets_.end() ? QuoteOf(subject, day, Market())
                                   : QuoteOf(subject, day, found->second);
  }

  // Tomorrow's open markets with anything in them: most shares first, then
  // in order of subject.
  std::vector<SeenQuote> Quotes(absl::CivilDay today) const {
    std::vector<std::pair<const Key*, const Market*>> open;
    for (const auto& [key, market] : markets_) {
      if (key.first != today + 1 || market.settled) continue;
      if (market.sold == Shares{} && market.guesses.empty()) continue;

      open.emplace_back(&key, &market);
    }
    std::ranges::stable_sort(open, [](const auto& a, const auto& b) {
      const int64_t sold_a = a.second->sold.yes + a.second->sold.no;
      const int64_t sold_b = b.second->sold.yes + b.second->sold.no;
      if (sold_a != sold_b) return sold_a > sold_b;

      return Stored(a.first->second) < Stored(b.first->second);
    });

    std::vector<SeenQuote> quotes;
    quotes.reserve(open.size());
    for (const auto& [key, market] : open) {
      quotes.push_back(QuoteOf(key->second, key->first, *market));
    }
    return quotes;
  }

  SeenFill Buy(uint64_t trader, const std::string& name, uint64_t subject,
               Outcome outcome, Gmbux budget, absl::CivilDay today) {
    const Key key = {today + 1, subject};
    const Market before = MarketAt(key);
    const int64_t balance = BalanceOf(trader);

    if (before.settled) return Refused(absl::StatusCode::kOutOfRange);
    if (trader == subject && outcome == Outcome::kNo) {
      return Refused(absl::StatusCode::kOutOfRange);
    }
    if (budget.hundredths() > balance) {
      return Refused(absl::StatusCode::kResourceExhausted);
    }

    const int64_t shares =
        market::SharesAffordable(before.sold, outcome, budget);
    if (shares == 0) return Refused(absl::StatusCode::kOutOfRange);

    const int64_t cost =
        market::CostOfBuying(before.sold, outcome, shares).hundredths();

    Market& market = markets_[key];
    market.sold = market.sold.With(outcome, shares);
    market.held[trader] = market.held[trader].With(outcome, shares);
    names_[trader] = name;
    balances_[trader] = balance - cost;

    return {absl::StatusCode::kOk,
            shares,
            cost,
            market.held[trader].of(outcome),
            balance - cost,
            QuoteOf(subject, key.first, market)};
  }

  SeenFill Sell(uint64_t trader, const std::string& name, uint64_t subject,
                Outcome outcome, std::optional<int64_t> shares,
                absl::CivilDay today) {
    const Key key = {today + 1, subject};
    const Market before = MarketAt(key);
    const int64_t balance = BalanceOf(trader);

    if (before.settled) return Refused(absl::StatusCode::kOutOfRange);

    const auto holder = before.held.find(trader);
    const int64_t holding =
        holder == before.held.end() ? 0 : holder->second.of(outcome);
    if (holding == 0) return Refused(absl::StatusCode::kResourceExhausted);

    const int64_t selling = shares.value_or(holding);
    if (selling < 1) return Refused(absl::StatusCode::kOutOfRange);
    if (selling > holding) {
      return Refused(absl::StatusCode::kResourceExhausted);
    }

    const int64_t proceeds =
        market::ProceedsOfSelling(before.sold, outcome, selling).hundredths();

    Market& market = markets_[key];
    market.sold = market.sold.With(outcome, -selling);
    market.held[trader] = market.held[trader].With(outcome, -selling);
    names_[trader] = name;
    balances_[trader] = balance + proceeds;

    return {absl::StatusCode::kOk,
            selling,
            proceeds,
            market.held[trader].of(outcome),
            balance + proceeds,
            QuoteOf(subject, key.first, market)};
  }

  SeenTicket Guess(uint64_t trader, const std::string& name, uint64_t subject,
                   TimeOfDay time, absl::CivilDay today) {
    const Key key = {today + 1, subject};
    const Market before = MarketAt(key);
    const int64_t balance = BalanceOf(trader);

    if (before.settled) {
      return {absl::StatusCode::kOutOfRange, 0, 0, SeenQuote()};
    }
    if (trader == subject) {
      return {absl::StatusCode::kOutOfRange, 0, 0, SeenQuote()};
    }

    const bool guessed_before = std::ranges::any_of(
        before.guesses,
        [trader](const auto& guess) { return guess.first == trader; });
    const int64_t stake = market::kGuessStake.hundredths();
    if (!guessed_before && stake > balance) {
      return {absl::StatusCode::kResourceExhausted, 0, 0, SeenQuote()};
    }

    Market& market = markets_[key];
    names_[trader] = name;

    // A second guess takes the place of the first, where the first was, and
    // costs nothing.
    if (guessed_before) {
      for (auto& [who, minute] : market.guesses) {
        if (who == trader) minute = time.minute();
      }
      return {absl::StatusCode::kOk, 0, balance,
              QuoteOf(subject, key.first, market)};
    }

    market.guesses.emplace_back(trader, time.minute());
    balances_[trader] = balance - stake;

    return {absl::StatusCode::kOk, stake, balance - stake,
            QuoteOf(subject, key.first, market)};
  }

  // The settlement of the market on `subject` for `day` as yes, said at
  // `time`, if there is one still open.
  std::optional<SeenSettlement> Said(uint64_t subject, absl::CivilDay day,
                                     TimeOfDay time) {
    const auto found = markets_.find({day, subject});
    if (found == markets_.end() || found->second.settled) return std::nullopt;

    return Settle(found->first, found->second, true, time.minute());
  }

  // The settlements of every open market for a day before `today`: earliest
  // day first, then in order of subject. `unseen` are the GMs that gm_bot
  // has on record and the exchange was never told of.
  std::vector<SeenSettlement> Expire(
      absl::CivilDay today,
      const std::set<std::pair<uint64_t, absl::CivilDay>>& unseen) {
    std::vector<Key> due;
    for (const auto& [key, market] : markets_) {
      if (key.first < today && !market.settled) due.push_back(key);
    }
    std::ranges::stable_sort(due, [](const Key& a, const Key& b) {
      if (a.first != b.first) return a.first < b.first;

      return Stored(a.second) < Stored(b.second);
    });

    std::vector<SeenSettlement> settlements;
    settlements.reserve(due.size());
    for (const Key& key : due) {
      settlements.push_back(Settle(
          key, markets_[key], unseen.contains({key.second, key.first}), -1));
    }
    return settlements;
  }

  // The balance of `trader`, who from now on has an account by this name,
  // and what they hold in open markets: by day, then in order of subject.
  SeenPortfolio PortfolioOf(uint64_t trader, const std::string& name) {
    balances_[trader] = BalanceOf(trader);
    names_[trader] = name;

    std::vector<SeenHolding> holdings;
    for (const auto& [key, market] : markets_) {
      if (market.settled) continue;

      const auto held = market.held.find(trader);
      const Shares shares = held == market.held.end() ? Shares{} : held->second;
      int guess = -1;
      for (const auto& [who, minute] : market.guesses) {
        if (who == trader) guess = minute;
      }
      if (shares == Shares{} && guess == -1) continue;

      holdings.emplace_back(key.second, key.first, shares.yes, shares.no,
                            guess);
    }
    std::ranges::stable_sort(
        holdings, [](const SeenHolding& a, const SeenHolding& b) {
          if (std::get<1>(a) != std::get<1>(b)) {
            return std::get<1>(a) < std::get<1>(b);
          }

          return Stored(std::get<0>(a)) < Stored(std::get<0>(b));
        });

    return {balances_[trader], std::move(holdings)};
  }

  // Up to `limit` accounts: richest first, then by name, then by id.
  std::vector<SeenAccount> Richest(size_t limit) const {
    std::vector<SeenAccount> accounts;
    accounts.reserve(balances_.size());
    for (const auto& [trader, balance] : balances_) {
      accounts.emplace_back(trader, names_.at(trader), balance);
    }
    std::ranges::sort(accounts, [](const SeenAccount& a, const SeenAccount& b) {
      if (std::get<2>(a) != std::get<2>(b)) {
        return std::get<2>(a) > std::get<2>(b);
      }
      if (std::get<1>(a) != std::get<1>(b)) {
        return std::get<1>(a) < std::get<1>(b);
      }

      return Stored(std::get<0>(a)) < Stored(std::get<0>(b));
    });

    if (accounts.size() > limit) accounts.resize(limit);
    return accounts;
  }

 private:
  // Which market: its day and its subject.
  using Key = std::pair<absl::CivilDay, uint64_t>;

  struct Market {
    Shares sold;
    // What each trader holds.
    std::map<uint64_t, Shares> held;
    // Who guessed what minute, in the order they first guessed.
    std::vector<std::pair<uint64_t, int>> guesses;
    bool settled = false;
  };

  // Where ids are put in order, it is as the numbers the database stores
  // them as, which are signed: the largest ids come first.
  static int64_t Stored(uint64_t id) { return static_cast<int64_t>(id); }

  static SeenFill Refused(absl::StatusCode code) {
    return {code, 0, 0, 0, 0, SeenQuote()};
  }

  static SeenQuote QuoteOf(uint64_t subject, absl::CivilDay day,
                           const Market& market) {
    const int guesses = static_cast<int>(market.guesses.size());

    return {subject,         day,
            market.sold.yes, market.sold.no,
            guesses,         guesses * market::kGuessStake.hundredths()};
  }

  // The market at `key`, or one nobody has been in. Makes none.
  Market MarketAt(const Key& key) const {
    const auto found = markets_.find(key);
    return found == markets_.end() ? Market() : found->second;
  }

  // What `trader` has, which before their first trade is the starting
  // balance.
  int64_t BalanceOf(uint64_t trader) const {
    const auto found = balances_.find(trader);
    return found == balances_.end() ? market::kStartingBalance.hundredths()
                                    : found->second;
  }

  // Settles `market`, paying the shares of the right answer and dealing
  // with the pot. `said_at` is -1 if nobody knows when the GM was said, or
  // there was none.
  SeenSettlement Settle(const Key& key, Market& market, bool yes, int said_at) {
    market.settled = true;

    // The shares: a gmbux each to the right ones, in order of holder and
    // then of size.
    std::vector<uint64_t> holders;
    for (const auto& [holder, shares] : market.held) {
      if (shares != Shares{}) holders.push_back(holder);
    }
    std::ranges::sort(holders, {}, &Stored);

    std::vector<SeenPayout> winnings;
    for (const uint64_t holder : holders) {
      const int64_t winning =
          yes ? market.held[holder].yes : market.held[holder].no;
      if (winning == 0) continue;

      const int64_t amount = winning * market::kSharePayout.hundredths();
      balances_[holder] += amount;
      winnings.emplace_back(holder, names_[holder], amount);
    }
    std::ranges::stable_sort(winnings,
                             [](const SeenPayout& a, const SeenPayout& b) {
                               return std::get<2>(a) > std::get<2>(b);
                             });

    // The pot: to the nearest guesses if there is a time to be near, and
    // otherwise back where it came from.
    const int64_t stake = market::kGuessStake.hundredths();
    const int64_t pot = stake * static_cast<int64_t>(market.guesses.size());
    std::vector<SeenPayout> pot_payouts;
    int nearest_guess = -1;
    const bool returned = said_at == -1 && !market.guesses.empty();

    if (returned) {
      for (const auto& [who, minute] : market.guesses) {
        balances_[who] += stake;
        pot_payouts.emplace_back(who, names_[who], stake);
      }
    } else if (!market.guesses.empty()) {
      int nearest = std::numeric_limits<int>::max();
      for (const auto& [who, minute] : market.guesses) {
        nearest = std::min(nearest, std::abs(minute - said_at));
      }

      std::vector<std::pair<uint64_t, int>> winners;
      for (const auto& guess : market.guesses) {
        if (std::abs(guess.second - said_at) == nearest) {
          winners.push_back(guess);
        }
      }
      nearest_guess = winners.front().second;

      // Shared equally, the odd hundredths going to the earliest guesses.
      const int64_t count = static_cast<int64_t>(winners.size());
      for (int64_t i = 0; i < count; ++i) {
        const uint64_t who = winners[static_cast<size_t>(i)].first;
        const int64_t amount = pot / count + (i < pot % count ? 1 : 0);
        balances_[who] += amount;
        pot_payouts.emplace_back(who, names_[who], amount);
      }
    }

    return {key.second,
            key.first,
            yes,
            yes ? said_at : -1,
            static_cast<int>(holders.size()),
            std::move(winnings),
            pot,
            returned,
            std::move(pot_payouts),
            nearest_guess};
  }

  // Hundredths of a gmbux, for everyone with an account.
  std::map<uint64_t, int64_t> balances_;
  std::map<uint64_t, std::string> names_;
  std::map<Key, Market> markets_;
};

// Puts an exchange and the model through `steps`, and requires that they
// agree at every one, and afterwards on all that can be asked.
void AgreesWithTheModel(const std::vector<Step>& steps, int limit) {
  absl::StatusOr<market::Exchange> exchange = market::Exchange::InMemory();
  ASSERT_TRUE(exchange.ok()) << exchange.status();

  std::array<Model, kCommunities.size()> models;
  // The GMs gm_bot has on record that the exchange was never told of, in
  // each community: whose and on which day.
  std::array<std::set<std::pair<uint64_t, absl::CivilDay>>, kCommunities.size()>
      unseen;
  // The day it is, which is the same everywhere.
  absl::CivilDay today = kFirstDay;

  for (const Step& step : steps) {
    const size_t where = static_cast<size_t>(step.community);
    market::Community community = exchange->community(kCommunities[where]);
    Model& model = models[where];

    const uint64_t subject = kIds[static_cast<size_t>(step.subject)];
    const market::Trader trader = {
        .id = kIds[static_cast<size_t>(step.trader)],
        .name = step.name,
    };
    const Outcome outcome = step.outcome == 0 ? Outcome::kYes : Outcome::kNo;
    const TimeOfDay time = TimeOfDay::AfterMidnight(step.minute);

    const auto said_gm = [&unseen, where](uint64_t who, absl::CivilDay day) {
      return absl::StatusOr<bool>(unseen[where].contains({who, day}));
    };

    switch (step.kind) {
      case Step::kBuy: {
        const Gmbux budget = Gmbux::Hundredths(step.amount);
        ASSERT_EQ(
            Seen(community.Buy(trader, subject, outcome, budget, today)),
            model.Buy(trader.id, trader.name, subject, outcome, budget, today));
        break;
      }

      case Step::kSell: {
        const std::optional<int64_t> shares =
            step.everything ? std::nullopt
                            : std::optional<int64_t>(step.amount);
        ASSERT_EQ(Seen(community.Sell(trader, subject, outcome, shares, today)),
                  model.Sell(trader.id, trader.name, subject, outcome, shares,
                             today));
        break;
      }

      case Step::kGuess:
        ASSERT_EQ(Seen(community.Guess(trader, subject, time, today)),
                  model.Guess(trader.id, trader.name, subject, time, today));
        break;

      case Step::kSaid: {
        const absl::CivilDay day = today + (step.ahead ? 1 : 0);
        const absl::StatusOr<std::optional<market::Settlement>> settled =
            community.Said(subject, day, time);
        ASSERT_TRUE(settled.ok()) << settled.status();

        const std::optional<SeenSettlement> expected =
            model.Said(subject, day, time);
        ASSERT_EQ(settled->has_value(), expected.has_value());
        if (expected.has_value()) {
          ASSERT_EQ(Seen(settled->value_or(market::Settlement{})), *expected);
        }
        break;
      }

      case Step::kSaidUnseen: unseen[where].insert({subject, today}); break;

      case Step::kNextDay: ++today; [[fallthrough]];

      case Step::kExpire: {
        const absl::StatusOr<std::vector<market::Settlement>> settled =
            community.Expire(today, said_gm);
        ASSERT_TRUE(settled.ok()) << settled.status();

        std::vector<SeenSettlement> seen;
        for (const market::Settlement& settlement : *settled) {
          seen.push_back(Seen(settlement));
        }
        ASSERT_EQ(seen, model.Expire(today, unseen[where]));
        break;
      }

      default: break;
    }

    // Whatever just happened, each community is as its model says, the one
    // nothing happened in included.
    for (size_t i = 0; i < kCommunities.size(); ++i) {
      market::Community looked_at = exchange->community(kCommunities[i]);

      for (const uint64_t id : kIds) {
        const absl::StatusOr<market::Quote> quote =
            looked_at.QuoteFor(id, today);
        ASSERT_TRUE(quote.ok()) << quote.status();
        ASSERT_EQ(Seen(*quote), models[i].QuoteFor(id, today));
      }

      const absl::StatusOr<std::vector<market::Quote>> quotes =
          looked_at.Quotes(today);
      ASSERT_TRUE(quotes.ok()) << quotes.status();
      std::vector<SeenQuote> seen_quotes;
      for (const market::Quote& quote : *quotes) {
        seen_quotes.push_back(Seen(quote));
      }
      ASSERT_EQ(seen_quotes, models[i].Quotes(today));

      const absl::StatusOr<std::vector<market::Account>> richest =
          looked_at.Richest(limit);
      ASSERT_TRUE(richest.ok()) << richest.status();
      std::vector<SeenAccount> seen_accounts;
      for (const market::Account& account : *richest) {
        seen_accounts.emplace_back(account.trader.id, account.trader.name,
                                   account.balance.hundredths());
      }
      ASSERT_EQ(seen_accounts,
                models[i].Richest(static_cast<size_t>(std::max(limit, 0))));
    }

    // Looking at a portfolio opens an account, so it is a step of its own
    // rather than something done after every other.
    if (step.kind == Step::kLookAround) {
      const absl::StatusOr<market::Portfolio> portfolio =
          community.PortfolioOf(trader);
      ASSERT_TRUE(portfolio.ok()) << portfolio.status();
      ASSERT_EQ(Seen(*portfolio), model.PortfolioOf(trader.id, trader.name));
    }
  }

  // At the end, everyone's portfolio, everywhere.
  for (size_t i = 0; i < kCommunities.size(); ++i) {
    for (const uint64_t id : kIds) {
      const absl::StatusOr<market::Portfolio> portfolio =
          exchange->community(kCommunities[i])
              .PortfolioOf(market::Trader{
                  .id = id,
                  .name = "at the end",
              });
      ASSERT_TRUE(portfolio.ok()) << portfolio.status();
      ASSERT_EQ(Seen(*portfolio), models[i].PortfolioOf(id, "at the end"));
    }
  }
}

// Names of any bytes at all, and a few that collide, so that the rich list
// has ties to break.
auto AnyName() {
  return fuzztest::OneOf(fuzztest::Arbitrary<std::string>(),
                         fuzztest::ElementOf<std::string>({
                             "",
                             "ada",
                             "Ada",
                             "grace",
                         }));
}

// A few traders in a few markets over a few days, so that markets fill up,
// empty, collect guesses that tie, and settle every way there is. Budgets
// run from nothing to more than anyone has, and times a little past either
// end of the day.
FUZZ_TEST(ExchangeFuzzTest, AgreesWithTheModel)
    .WithDomains(
        fuzztest::VectorOf(
            fuzztest::StructOf<Step>(
                fuzztest::InRange(0, static_cast<int>(Step::kKinds) - 1),
                fuzztest::InRange(0, 1), fuzztest::InRange(0, 3),
                fuzztest::InRange(0, 3), AnyName(), fuzztest::InRange(0, 1),
                fuzztest::OneOf(fuzztest::InRange(-100, 150000),
                                fuzztest::InRange(-2, 300)),
                fuzztest::Arbitrary<bool>(),
                fuzztest::OneOf(fuzztest::InRange(-5, 1445),
                                fuzztest::ElementOf({420, 480, 540})),
                fuzztest::Arbitrary<bool>()))
            .WithMaxSize(60),
        fuzztest::InRange(-1, 6));

}  // namespace
