// The exchange by example: buying and selling shares in whether someone
// will say GM, guessing when, and what everyone is paid once it is known.

#include "market/exchange.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"
#include "sqlite/database.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using market::Gmbux;
using market::Outcome;
using market::TimeOfDay;
using testing::ElementsAre;
using testing::Eq;
using testing::Field;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Optional;
using testing::SizeIs;

// The day everything here is traded on. The markets are for the day after,
// kTuesday, and are over by kWednesday.
constexpr absl::CivilDay kMonday(2026, 10, 5);
constexpr absl::CivilDay kTuesday = kMonday + 1;
constexpr absl::CivilDay kWednesday = kMonday + 2;

// Luke is who the markets here are about; Ada and Grace trade in them.
constexpr uint64_t kLuke = 44;

market::Trader Luke() {
  return market::Trader{
      .id = kLuke,
      .name = "luke",
  };
}

market::Trader Ada() {
  return market::Trader{
      .id = 45,
      .name = "ada",
  };
}

market::Trader Grace() {
  return market::Trader{
      .id = 46,
      .name = "grace",
  };
}

market::Exchange NewExchange() {
  absl::StatusOr<market::Exchange> exchange = market::Exchange::InMemory();
  ABSL_EXPECT_OK(exchange);
  return std::move(*exchange);
}

TimeOfDay At(int hour, int minute) {
  return TimeOfDay::AfterMidnight(hour * 60 + minute);
}

// The balance of `trader` in `community`.
Gmbux BalanceOf(market::Community community, const market::Trader& trader) {
  const absl::StatusOr<market::Portfolio> portfolio =
      community.PortfolioOf(trader);
  ABSL_EXPECT_OK(portfolio);
  return portfolio.ok() ? portfolio->balance : Gmbux();
}

// Nobody said GM, as far as anyone else knows either.
absl::StatusOr<bool> NobodySaidGm(uint64_t, absl::CivilDay) { return false; }

// The matcher for a payout of `amount` to the trader called `name`.
auto PayoutTo(const char* name, Gmbux amount) {
  return AllOf(
      Field(&market::Payout::trader, Field(&market::Trader::name, Eq(name))),
      Field(&market::Payout::amount, amount));
}

// Before anyone trades, a market stands at even odds and everyone has the
// starting balance.
TEST(ExchangeTest, StartsEveryoneLevel) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);

  const absl::StatusOr<market::Quote> quote =
      community.QuoteFor(kLuke, kMonday);
  ABSL_ASSERT_OK(quote);
  EXPECT_EQ(quote->subject, kLuke);
  EXPECT_EQ(quote->day, kTuesday);
  EXPECT_DOUBLE_EQ(quote->chance, 0.5);
  EXPECT_EQ(quote->sold, market::Shares{});

  EXPECT_EQ(BalanceOf(community, Ada()), market::kStartingBalance);
  EXPECT_THAT(community.Quotes(kMonday), IsOkAndHolds(IsEmpty()));
}

// The basic trade: gmbux buy shares in tomorrow's market, and buying YES
// makes the market think a GM likelier.
TEST(ExchangeTest, BuyingSharesCostsGmbuxAndMovesThePrice) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);

  const absl::StatusOr<market::Fill> fill =
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday);

  ABSL_ASSERT_OK(fill);
  EXPECT_EQ(fill->shares, 83);
  EXPECT_EQ(fill->held, 83);
  // As many whole shares as fit: a little of the budget may be left over.
  EXPECT_LE(fill->amount, Gmbux::Whole(50));
  EXPECT_GT(fill->amount, Gmbux::Whole(49));
  EXPECT_EQ(fill->balance, market::kStartingBalance - fill->amount);
  EXPECT_EQ(BalanceOf(community, Ada()), fill->balance);

  EXPECT_GT(fill->quote.chance, 0.5);
  EXPECT_EQ(fill->quote.day, kTuesday);
  EXPECT_THAT(community.QuoteFor(kLuke, kMonday),
              IsOkAndHolds(Field(&market::Quote::sold, market::Shares{
                                                           .yes = 83,
                                                       })));
}

// Buying NO does the opposite, and a second buyer pays the price the first
// left behind.
TEST(ExchangeTest, LaterBuyersPayThePriceEarlierOnesLeft) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);

  const absl::StatusOr<market::Fill> first =
      community.Buy(Ada(), kLuke, Outcome::kNo, Gmbux::Whole(50), kMonday);
  const absl::StatusOr<market::Fill> second =
      community.Buy(Grace(), kLuke, Outcome::kNo, Gmbux::Whole(50), kMonday);

  ABSL_ASSERT_OK(first);
  ABSL_ASSERT_OK(second);
  EXPECT_LT(first->quote.chance, 0.5);
  EXPECT_LT(second->quote.chance, first->quote.chance);
  EXPECT_LT(second->shares, first->shares);
}

// Nobody can spend what they have not got, and a refusal changes nothing.
TEST(ExchangeTest, RefusesABetLargerThanTheBalance) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);

  const absl::StatusOr<market::Fill> fill =
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(1001), kMonday);

  EXPECT_THAT(fill, StatusIs(absl::StatusCode::kResourceExhausted,
                             HasSubstr("You only have 1000 gmbux")));
  EXPECT_TRUE(market::IsRefusal(fill.status()));
  EXPECT_EQ(BalanceOf(community, Ada()), market::kStartingBalance);
  EXPECT_THAT(community.Quotes(kMonday), IsOkAndHolds(IsEmpty()));
}

// Nor can they buy nothing, or less than a share's worth.
TEST(ExchangeTest, RefusesABetThatBuysNothing) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);

  for (const Gmbux budget :
       {Gmbux(), Gmbux::Whole(-5), Gmbux::Hundredths(10)}) {
    EXPECT_THAT(community.Buy(Ada(), kLuke, Outcome::kYes, budget, kMonday),
                StatusIs(absl::StatusCode::kOutOfRange,
                         HasSubstr("not enough to buy a single share")));
  }
}

// The answer to the market on yourself is yours to give, so all you may do
// in it is back yourself.
TEST(ExchangeTest, LetsSomeoneBackThemselvesButNotBetAgainst) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);

  ABSL_EXPECT_OK(
      community.Buy(Luke(), kLuke, Outcome::kYes, Gmbux::Whole(10), kMonday));

  const absl::StatusOr<market::Fill> against =
      community.Buy(Luke(), kLuke, Outcome::kNo, Gmbux::Whole(10), kMonday);
  EXPECT_THAT(against, StatusIs(absl::StatusCode::kOutOfRange,
                                HasSubstr("cannot bet against")));
  EXPECT_TRUE(market::IsRefusal(against.status()));

  EXPECT_THAT(community.Guess(Luke(), kLuke, At(7, 45), kMonday),
              StatusIs(absl::StatusCode::kOutOfRange,
                       HasSubstr("cannot guess when you will say GM")));
}

// Shares can be sold back to the market before it closes, for what they are
// then worth: here, a little less than was just paid.
TEST(ExchangeTest, SellsSharesBackAtTheGoingPrice) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  const absl::StatusOr<market::Fill> bought =
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday);
  ABSL_ASSERT_OK(bought);

  const absl::StatusOr<market::Fill> some =
      community.Sell(Ada(), kLuke, Outcome::kYes, 33, kMonday);
  ABSL_ASSERT_OK(some);
  EXPECT_EQ(some->shares, 33);
  EXPECT_EQ(some->held, 50);

  // Told no number, it sells the rest.
  const absl::StatusOr<market::Fill> rest =
      community.Sell(Ada(), kLuke, Outcome::kYes, std::nullopt, kMonday);
  ABSL_ASSERT_OK(rest);
  EXPECT_EQ(rest->shares, 50);
  EXPECT_EQ(rest->held, 0);
  EXPECT_DOUBLE_EQ(rest->quote.chance, 0.5);

  // Out and back loses the rounding at most.
  EXPECT_LE(rest->balance, market::kStartingBalance);
  EXPECT_GE(rest->balance + Gmbux::Hundredths(3), market::kStartingBalance);

  // With nothing held and nothing guessed, the market is not listed.
  EXPECT_THAT(community.Quotes(kMonday), IsOkAndHolds(IsEmpty()));
}

// Selling what one does not hold is refused.
TEST(ExchangeTest, RefusesToSellWhatIsNotHeld) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(5), kMonday));

  EXPECT_THAT(community.Sell(Ada(), kLuke, Outcome::kNo, std::nullopt, kMonday),
              StatusIs(absl::StatusCode::kResourceExhausted,
                       HasSubstr("You hold no NO shares")));
  EXPECT_THAT(community.Sell(Ada(), kLuke, Outcome::kYes, 500, kMonday),
              StatusIs(absl::StatusCode::kResourceExhausted,
                       HasSubstr("You only hold 9 YES shares")));
  EXPECT_THAT(
      community.Sell(Ada(), kLuke, Outcome::kYes, 0, kMonday),
      StatusIs(absl::StatusCode::kOutOfRange, HasSubstr("at least one share")));
}

// The moment the subject says GM their market settles: YES shares pay a
// gmbux each and NO shares nothing.
TEST(ExchangeTest, AGmPaysTheHoldersOfYes) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  const absl::StatusOr<market::Fill> yes =
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday);
  const absl::StatusOr<market::Fill> no =
      community.Buy(Grace(), kLuke, Outcome::kNo, Gmbux::Whole(30), kMonday);
  ABSL_ASSERT_OK(yes);
  ABSL_ASSERT_OK(no);

  const absl::StatusOr<std::optional<market::Settlement>> settled =
      community.Said(kLuke, kTuesday, At(8, 15));

  ABSL_ASSERT_OK(settled);
  ASSERT_TRUE(settled->has_value());
  const market::Settlement settlement = settled->value_or(market::Settlement{});
  EXPECT_EQ(settlement.subject, kLuke);
  EXPECT_EQ(settlement.day, kTuesday);
  EXPECT_EQ(settlement.outcome, Outcome::kYes);
  EXPECT_THAT(settlement.said_at, Optional(At(8, 15)));
  EXPECT_EQ(settlement.holders, 2);
  EXPECT_THAT(settlement.winnings,
              ElementsAre(PayoutTo("ada", Gmbux::Whole(yes->shares))));

  EXPECT_EQ(BalanceOf(community, Ada()),
            yes->balance + Gmbux::Whole(yes->shares));
  EXPECT_EQ(BalanceOf(community, Grace()), no->balance);
}

// A market settles once. A second GM the same day, or a GM from someone
// nobody bet on, settles nothing.
TEST(ExchangeTest, SettlesAMarketOnlyOnce) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday));
  ABSL_ASSERT_OK(community.Said(kLuke, kTuesday, At(8, 15)));
  const Gmbux after = BalanceOf(community, Ada());

  EXPECT_THAT(community.Said(kLuke, kTuesday, At(9, 0)),
              IsOkAndHolds(Eq(std::nullopt)));
  EXPECT_THAT(community.Said(45, kTuesday, At(9, 0)),
              IsOkAndHolds(Eq(std::nullopt)));
  EXPECT_THAT(community.Expire(kWednesday, NobodySaidGm),
              IsOkAndHolds(IsEmpty()));
  EXPECT_EQ(BalanceOf(community, Ada()), after);
}

// A day that goes by without a GM settles its markets the other way: NO
// shares pay, and YES shares are worth nothing.
TEST(ExchangeTest, ADayWithoutAGmPaysTheHoldersOfNo) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  const absl::StatusOr<market::Fill> yes =
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday);
  const absl::StatusOr<market::Fill> no =
      community.Buy(Grace(), kLuke, Outcome::kNo, Gmbux::Whole(30), kMonday);
  ABSL_ASSERT_OK(yes);
  ABSL_ASSERT_OK(no);

  // On the day itself there is still time, so nothing is settled.
  EXPECT_THAT(community.Expire(kTuesday, NobodySaidGm),
              IsOkAndHolds(IsEmpty()));

  const absl::StatusOr<std::vector<market::Settlement>> settled =
      community.Expire(kWednesday, NobodySaidGm);

  ABSL_ASSERT_OK(settled);
  ASSERT_THAT(*settled, SizeIs(1));
  const market::Settlement& settlement = settled->front();
  EXPECT_EQ(settlement.outcome, Outcome::kNo);
  EXPECT_EQ(settlement.said_at, std::nullopt);
  EXPECT_THAT(settlement.winnings,
              ElementsAre(PayoutTo("grace", Gmbux::Whole(no->shares))));
  EXPECT_EQ(BalanceOf(community, Ada()), yes->balance);
  EXPECT_EQ(BalanceOf(community, Grace()),
            no->balance + Gmbux::Whole(no->shares));
}

// A market closes when its day begins: by then a trade is in the market for
// the day after, and the one in waiting is untouched.
TEST(ExchangeTest, TradesOnlyInTomorrowsMarket) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday));

  const absl::StatusOr<market::Fill> next =
      community.Buy(Ada(), kLuke, Outcome::kNo, Gmbux::Whole(10), kTuesday);

  ABSL_ASSERT_OK(next);
  EXPECT_EQ(next->quote.day, kWednesday);
  EXPECT_THAT(community.QuoteFor(kLuke, kMonday),
              IsOkAndHolds(Field(&market::Quote::sold, market::Shares{
                                                           .yes = 83,
                                                       })));
  EXPECT_THAT(
      community.Sell(Ada(), kLuke, Outcome::kYes, std::nullopt, kTuesday),
      StatusIs(absl::StatusCode::kResourceExhausted));
}

// The pot: each guess stakes the same, and whoever is nearest to when the
// GM was said takes all of it.
TEST(ExchangeTest, TheNearestGuessTakesThePot) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);

  const absl::StatusOr<market::Ticket> early =
      community.Guess(Ada(), kLuke, At(7, 0), kMonday);
  const absl::StatusOr<market::Ticket> late =
      community.Guess(Grace(), kLuke, At(9, 0), kMonday);
  ABSL_ASSERT_OK(early);
  ABSL_ASSERT_OK(late);
  EXPECT_EQ(early->staked, market::kGuessStake);
  EXPECT_EQ(early->balance, market::kStartingBalance - market::kGuessStake);
  EXPECT_EQ(late->quote.guesses, 2);
  EXPECT_EQ(late->quote.pot, Gmbux::Whole(50));

  const absl::StatusOr<std::optional<market::Settlement>> settled =
      community.Said(kLuke, kTuesday, At(8, 15));

  ABSL_ASSERT_OK(settled);
  ASSERT_TRUE(settled->has_value());
  const market::Settlement settlement = settled->value_or(market::Settlement{});
  EXPECT_EQ(settlement.pot, Gmbux::Whole(50));
  EXPECT_FALSE(settlement.pot_returned);
  EXPECT_THAT(settlement.nearest_guess, Optional(At(9, 0)));
  EXPECT_THAT(settlement.pot_payouts,
              ElementsAre(PayoutTo("grace", Gmbux::Whole(50))));
  EXPECT_EQ(BalanceOf(community, Grace()), Gmbux::Whole(1025));
  EXPECT_EQ(BalanceOf(community, Ada()), Gmbux::Whole(975));
}

// Guesses equally near, on either side, share the pot. What does not divide
// goes to whoever guessed first.
TEST(ExchangeTest, LevelGuessesShareThePot) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  const market::Trader third = {
      .id = 47,
      .name = "edsger",
  };
  const market::Trader fourth = {
      .id = 48,
      .name = "barbara",
  };
  ABSL_ASSERT_OK(community.Guess(Ada(), kLuke, At(8, 0), kMonday));
  ABSL_ASSERT_OK(community.Guess(Grace(), kLuke, At(8, 30), kMonday));
  ABSL_ASSERT_OK(community.Guess(third, kLuke, At(8, 0), kMonday));
  ABSL_ASSERT_OK(community.Guess(fourth, kLuke, At(23, 0), kMonday));

  const absl::StatusOr<std::optional<market::Settlement>> settled =
      community.Said(kLuke, kTuesday, At(8, 15));

  ABSL_ASSERT_OK(settled);
  ASSERT_TRUE(settled->has_value());
  // A hundred gmbux among three: 33.34 to the first of them.
  EXPECT_THAT(settled->value_or(market::Settlement{}).pot_payouts,
              ElementsAre(PayoutTo("ada", Gmbux::Hundredths(3334)),
                          PayoutTo("grace", Gmbux::Hundredths(3333)),
                          PayoutTo("edsger", Gmbux::Hundredths(3333))));
}

// Thinking again is free: a second guess replaces the first, and its stake
// carries over.
TEST(ExchangeTest, ASecondGuessReplacesTheFirst) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(community.Guess(Ada(), kLuke, At(7, 0), kMonday));

  const absl::StatusOr<market::Ticket> again =
      community.Guess(Ada(), kLuke, At(8, 10), kMonday);

  ABSL_ASSERT_OK(again);
  EXPECT_EQ(again->staked, Gmbux());
  EXPECT_EQ(again->balance, market::kStartingBalance - market::kGuessStake);
  EXPECT_EQ(again->quote.guesses, 1);
  EXPECT_EQ(again->quote.pot, market::kGuessStake);

  const absl::StatusOr<market::Portfolio> portfolio =
      community.PortfolioOf(Ada());
  ABSL_ASSERT_OK(portfolio);
  EXPECT_THAT(portfolio->holdings,
              ElementsAre(Field(&market::Holding::guess, Optional(At(8, 10)))));
}

// With no GM there is no time to be near, so everyone gets their stake
// back.
TEST(ExchangeTest, ThePotIsReturnedIfThereIsNoGm) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(community.Guess(Ada(), kLuke, At(7, 0), kMonday));
  ABSL_ASSERT_OK(community.Guess(Grace(), kLuke, At(9, 0), kMonday));

  const absl::StatusOr<std::vector<market::Settlement>> settled =
      community.Expire(kWednesday, NobodySaidGm);

  ABSL_ASSERT_OK(settled);
  ASSERT_THAT(*settled, SizeIs(1));
  EXPECT_TRUE(settled->front().pot_returned);
  EXPECT_EQ(settled->front().nearest_guess, std::nullopt);
  EXPECT_THAT(settled->front().pot_payouts,
              ElementsAre(PayoutTo("ada", market::kGuessStake),
                          PayoutTo("grace", market::kGuessStake)));
  EXPECT_EQ(BalanceOf(community, Ada()), market::kStartingBalance);
  EXPECT_EQ(BalanceOf(community, Grace()), market::kStartingBalance);
}

// If the exchange was never told of a GM that did happen (the bot was not
// running, say), the market still settles as yes once the day is over. The
// time is anyone's guess, so the pot goes back.
TEST(ExchangeTest, AGmNobodySawStillSettlesAsYes) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  const absl::StatusOr<market::Fill> yes =
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday);
  ABSL_ASSERT_OK(yes);
  ABSL_ASSERT_OK(community.Guess(Grace(), kLuke, At(9, 0), kMonday));

  const absl::StatusOr<std::vector<market::Settlement>> settled =
      community.Expire(kWednesday, [](uint64_t subject, absl::CivilDay day) {
        return absl::StatusOr<bool>(subject == kLuke && day == kTuesday);
      });

  ABSL_ASSERT_OK(settled);
  ASSERT_THAT(*settled, SizeIs(1));
  EXPECT_EQ(settled->front().outcome, Outcome::kYes);
  EXPECT_EQ(settled->front().said_at, std::nullopt);
  EXPECT_THAT(settled->front().winnings,
              ElementsAre(PayoutTo("ada", Gmbux::Whole(yes->shares))));
  EXPECT_TRUE(settled->front().pot_returned);
  EXPECT_EQ(BalanceOf(community, Grace()), market::kStartingBalance);
}

// If it cannot be found out whether a GM was said, the market stays open
// for another try rather than being settled on a guess.
TEST(ExchangeTest, LeavesMarketsOpenIfTheAnswerCannotBeHad) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kNo, Gmbux::Whole(50), kMonday));

  EXPECT_THAT(community.Expire(kWednesday,
                               [](uint64_t, absl::CivilDay) {
                                 return absl::StatusOr<bool>(
                                     absl::UnavailableError("busy"));
                               }),
              StatusIs(absl::StatusCode::kUnavailable));

  EXPECT_THAT(community.Expire(kWednesday, NobodySaidGm),
              IsOkAndHolds(SizeIs(1)));
}

// A portfolio is the balance and whatever is riding on markets still open.
TEST(ExchangeTest, APortfolioListsWhatIsStillRiding) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday));
  ABSL_ASSERT_OK(community.Guess(Ada(), kLuke, At(7, 45), kMonday));
  ABSL_ASSERT_OK(community.Guess(Ada(), 46, At(10, 0), kMonday));

  absl::StatusOr<market::Portfolio> portfolio = community.PortfolioOf(Ada());
  ABSL_ASSERT_OK(portfolio);
  ASSERT_THAT(portfolio->holdings, SizeIs(2));
  EXPECT_EQ(portfolio->holdings[0].subject, kLuke);
  EXPECT_EQ(portfolio->holdings[0].day, kTuesday);
  EXPECT_EQ(portfolio->holdings[0].shares, (market::Shares{
                                               .yes = 83,
                                           }));
  EXPECT_THAT(portfolio->holdings[0].guess, Optional(At(7, 45)));
  EXPECT_EQ(portfolio->holdings[1].subject, 46);
  EXPECT_EQ(portfolio->holdings[1].shares, market::Shares{});

  // Once a market is settled it is in the balance instead.
  ABSL_ASSERT_OK(community.Said(kLuke, kTuesday, At(8, 0)));
  portfolio = community.PortfolioOf(Ada());
  ABSL_ASSERT_OK(portfolio);
  EXPECT_THAT(portfolio->holdings,
              ElementsAre(Field(&market::Holding::subject, 46)));
}

// The markets open for trading are listed busiest first.
TEST(ExchangeTest, ListsTomorrowsMarketsBusiestFirst) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(10), kMonday));
  ABSL_ASSERT_OK(
      community.Buy(Ada(), 46, Outcome::kNo, Gmbux::Whole(50), kMonday));
  ABSL_ASSERT_OK(community.Guess(Ada(), 47, At(7, 0), kMonday));

  EXPECT_THAT(community.Quotes(kMonday),
              IsOkAndHolds(ElementsAre(
                  Field(&market::Quote::subject, 46),
                  Field(&market::Quote::subject, kLuke),
                  Field(&market::Quote::pot, market::kGuessStake))));

  // A day on they are closed, and no longer listed.
  EXPECT_THAT(community.Quotes(kTuesday), IsOkAndHolds(IsEmpty()));
}

// The rich list is everyone who has traded, by balance, under the name they
// last went by.
TEST(ExchangeTest, RanksTradersByBalance) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday));
  ABSL_ASSERT_OK(community.Guess(Grace(), kLuke, At(8, 0), kMonday));
  ABSL_ASSERT_OK(community.PortfolioOf(market::Trader{
      .id = 46,
      .name = "amazing grace",
  }));

  const absl::StatusOr<std::vector<market::Account>> richest =
      community.Richest(10);

  ABSL_ASSERT_OK(richest);
  ASSERT_THAT(*richest, SizeIs(2));
  EXPECT_EQ((*richest)[0].trader.name, "amazing grace");
  EXPECT_EQ((*richest)[0].balance, Gmbux::Whole(975));
  EXPECT_EQ((*richest)[1].trader.name, "ada");

  EXPECT_THAT(community.Richest(1), IsOkAndHolds(SizeIs(1)));

  // No limit at all is not what a limit of less than nothing means: found
  // by exchange_fuzz_test.cc.
  EXPECT_THAT(community.Richest(0), IsOkAndHolds(IsEmpty()));
  EXPECT_THAT(community.Richest(-1), IsOkAndHolds(IsEmpty()));
}

// Communities share nothing: not balances, not markets.
TEST(ExchangeTest, KeepsCommunitiesApart) {
  market::Exchange exchange = NewExchange();
  market::Community here = exchange.community(33);
  market::Community there = exchange.community(34);
  ABSL_ASSERT_OK(
      here.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday));

  EXPECT_EQ(BalanceOf(there, Ada()), market::kStartingBalance);
  EXPECT_THAT(there.QuoteFor(kLuke, kMonday),
              IsOkAndHolds(Field(&market::Quote::sold, market::Shares{})));
  EXPECT_THAT(there.Said(kLuke, kTuesday, At(8, 0)),
              IsOkAndHolds(Eq(std::nullopt)));
}

// What the market maker gives away is all that is ever created: over a
// market the traders' gmbux grow by no more than its bounded loss, however
// it ends.
TEST(ExchangeTest, CreatesNoMoreGmbuxThanTheMakerCanLose) {
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  ABSL_ASSERT_OK(
      community.Buy(Ada(), kLuke, Outcome::kYes, Gmbux::Whole(900), kMonday));
  ABSL_ASSERT_OK(
      community.Buy(Grace(), kLuke, Outcome::kNo, Gmbux::Whole(400), kMonday));
  ABSL_ASSERT_OK(community.Guess(Grace(), kLuke, At(8, 0), kMonday));
  ABSL_ASSERT_OK(community.Said(kLuke, kTuesday, At(8, 15)));

  const Gmbux total =
      BalanceOf(community, Ada()) + BalanceOf(community, Grace());

  EXPECT_LE(total, market::kStartingBalance + market::kStartingBalance +
                       Gmbux::Whole(70));
}

// An exchange kept in a file is there, balances and open markets alike,
// when the file is opened again.
TEST(ExchangeTest, RemembersEverythingAcrossAReopen) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "predictions.db";

  Gmbux balance;
  {
    absl::StatusOr<market::Exchange> exchange = market::Exchange::Open(file);
    ABSL_ASSERT_OK(exchange);
    const absl::StatusOr<market::Fill> fill = exchange->community(33).Buy(
        Ada(), kLuke, Outcome::kYes, Gmbux::Whole(50), kMonday);
    ABSL_ASSERT_OK(fill);
    balance = fill->balance;
  }

  absl::StatusOr<market::Exchange> exchange = market::Exchange::Open(file);
  ABSL_ASSERT_OK(exchange);
  market::Community community = exchange->community(33);

  EXPECT_EQ(BalanceOf(community, Ada()), balance);
  EXPECT_THAT(community.Said(kLuke, kTuesday, At(8, 0)),
              IsOkAndHolds(Optional(Field(&market::Settlement::holders, 1))));
}

// A file from a later version of this code is refused rather than misread.
TEST(ExchangeTest, RefusesAFileFromALaterVersion) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "later.db";
  {
    absl::StatusOr<sqlite::Database> database = sqlite::Database::Open(file);
    ABSL_ASSERT_OK(database);
    ABSL_ASSERT_OK(database->Execute("PRAGMA user_version = 99"));
  }

  EXPECT_THAT(
      market::Exchange::Open(file),
      StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("layout 99")));
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //market:exchange_test -- --benchmark_filter=all
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
//   ----------------------------------------------------------------------------
//   Benchmark                                  Time             CPU   Iterations
//   ----------------------------------------------------------------------------
//   BM_Buy/5/10/7/iterations:200            9282 ns         9278 ns          200
//   BM_Buy/40/50/90/iterations:200          3281 ns         3277 ns          200
//   BM_BuyRefused/5/10/7                    2443 ns         2440 ns       172411
//   BM_BuyRefused/40/50/90                  2545 ns         2541 ns       166955
//   BM_Sell/5/10/7/iterations:200           8875 ns         8874 ns          200
//   BM_Sell/40/50/90/iterations:200         9784 ns         9780 ns          200
//   BM_GuessAgain/5/10/7                    6085 ns         6081 ns        69787
//   BM_GuessAgain/40/50/90                  6392 ns         6386 ns        66279
//   BM_QuoteFor/5/10/7                      1096 ns         1095 ns       385500
//   BM_QuoteFor/40/50/90                    1204 ns         1204 ns       346963
//   BM_Quotes/5/10/7                        8472 ns         8457 ns        49599
//   BM_Quotes/40/50/90                     28998 ns        28958 ns        14519
//   BM_PortfolioOf/5/10/7                  24674 ns        24655 ns        16859
//   BM_PortfolioOf/40/50/90                92622 ns        92447 ns         4622
//   BM_Richest/5/10/7                      47788 ns        47739 ns         8907
//   BM_Richest/40/50/90                    54427 ns        54349 ns         7843
//   BM_ExpireWithNothingOver/5/10/7          661 ns          661 ns       641175
//   BM_ExpireWithNothingOver/40/50/90        680 ns          679 ns       619068
//   BM_SaidWithNoMarket/5/10/7              2288 ns         2285 ns       182393
//   BM_SaidWithNoMarket/40/50/90            2426 ns         2423 ns       176424
//   BM_Said/5/iterations:50                16868 ns        16863 ns           50
//   BM_Said/40/iterations:50               66955 ns        66952 ns           50
//   BM_ExpireADay/5/10/iterations:5        0.428 ms        0.428 ms            5
//   BM_ExpireADay/40/50/iterations:5        4.59 ms         4.59 ms            5
// End of results.
// clang-format on

market::Trader TraderNumber(int number) {
  return market::Trader{
      .id = static_cast<uint64_t>(number) + 1000,
      .name = "trader",
  };
}

// The subjects of the markets below: members 1 upwards.
uint64_t SubjectNumber(int number) { return static_cast<uint64_t>(number) + 1; }

// Fills `community` with `markets` markets for the day after `today`, in
// each of which `traders` people hold a few shares, on alternate sides, and
// have a guess.
void Populate(market::Community community, int traders, int markets,
              absl::CivilDay today) {
  for (int m = 0; m < markets; ++m) {
    for (int t = 0; t < traders; ++t) {
      ABSL_EXPECT_OK(community.Buy(TraderNumber(t), SubjectNumber(m),
                                   t % 2 == 0 ? Outcome::kYes : Outcome::kNo,
                                   Gmbux::Whole(5), today));
      ABSL_EXPECT_OK(community.Guess(TraderNumber(t), SubjectNumber(m),
                                     At(6 + t % 6, (7 * t) % 60), today));
    }
  }
}

// Gives `community` a past: on each of the `days` days before kMonday there
// were `markets` markets, with a handful of regulars in each, and none of
// them saw a GM. All of it is settled.
void GiveHistory(market::Community community, int markets, int days) {
  constexpr int kInEach = 5;
  constexpr int kRegulars = 200;

  int regular = 0;
  for (int back = days; back >= 1; --back) {
    // Traded the day before, for the day that is `back` days before Monday.
    const absl::CivilDay traded = kMonday - back - 1;
    for (int m = 0; m < markets; ++m) {
      for (int i = 0; i < kInEach; ++i) {
        const market::Trader trader = TraderNumber(100000 + regular);
        regular = (regular + 1) % kRegulars;

        ABSL_EXPECT_OK(community.Buy(trader, SubjectNumber(m),
                                     i % 2 == 0 ? Outcome::kYes : Outcome::kNo,
                                     Gmbux::Whole(1), traded));
        ABSL_EXPECT_OK(
            community.Guess(trader, SubjectNumber(m), At(8, i), traded));
      }
    }
    ABSL_EXPECT_OK(community.Expire(traded + 2, NobodySaidGm));
  }
}

// An exchange with `markets` markets open for kTuesday and `traders` people
// in each, after `days` days of history, built once and kept: building it
// is slow, and the benchmark library calls each benchmark several times.
//
// Benchmarks that only read share one. One that writes names itself as
// `writer` and gets its own, so that what it adds does not slow the others
// down.
market::Community Established(const benchmark::State& state,
                              std::string_view writer = "") {
  const int traders = static_cast<int>(state.range(0));
  const int markets = static_cast<int>(state.range(1));
  const int days = static_cast<int>(state.range(2));

  static auto* const exchanges =
      new std::map<std::tuple<int, int, int, std::string>,
                   std::unique_ptr<market::Exchange>>();
  std::unique_ptr<market::Exchange>& exchange =
      (*exchanges)[{traders, markets, days, std::string(writer)}];
  if (exchange == nullptr) {
    exchange = std::make_unique<market::Exchange>(NewExchange());
    GiveHistory(exchange->community(33), markets, days);
    Populate(exchange->community(33), traders, markets, kMonday);
  }
  return exchange->community(33);
}

// The arguments are how many people are in each market, how many markets
// are open, and how many days of settled markets lie behind them: so the
// two are a quiet server in its first week and a busy one after a season.
void Sizes(benchmark::internal::Benchmark* benchmark) {
  benchmark->Args({5, 10, 7})->Args({40, 50, 90});
}

// A fixed number of iterations, for benchmarks that add to the exchange:
// enough to time, and few enough that what they add is small beside what
// was there.
constexpr int kWrites = 200;

// A /bet: the bot's main business. Each is a small one by the next trader
// in turn, in the first market.
void BM_Buy(benchmark::State& state) {
  const int traders = static_cast<int>(state.range(0));
  market::Community community = Established(state, "BM_Buy");
  int trader = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Buy(TraderNumber(trader),
                                           SubjectNumber(0), Outcome::kYes,
                                           Gmbux::Whole(1), kMonday));
    if (++trader == traders) trader = 0;
  }
}
BENCHMARK(BM_Buy)->Apply(Sizes)->Iterations(kWrites);

// A bet that is refused, for want of gmbux: everything a bet reads, and
// nothing written.
void BM_BuyRefused(benchmark::State& state) {
  market::Community community = Established(state);
  const market::Trader trader = TraderNumber(0);
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Buy(trader, SubjectNumber(0),
                                           Outcome::kYes, Gmbux::Whole(100000),
                                           kMonday));
  }
}
BENCHMARK(BM_BuyRefused)->Apply(Sizes);

// A /sell of one share, by the next trader in turn.
void BM_Sell(benchmark::State& state) {
  const int traders = static_cast<int>(state.range(0));
  market::Community community = Established(state, "BM_Sell");
  const auto side_of = [](int trader) {
    return trader % 2 == 0 ? Outcome::kYes : Outcome::kNo;
  };

  // Enough shares each, on the side they are already on, to last the run.
  for (int trader = 0; trader < traders; ++trader) {
    ABSL_EXPECT_OK(community.Buy(TraderNumber(trader), SubjectNumber(0),
                                 side_of(trader), Gmbux::Whole(50), kMonday));
  }

  int trader = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Sell(
        TraderNumber(trader), SubjectNumber(0), side_of(trader), 1, kMonday));
    if (++trader == traders) trader = 0;
  }
}
BENCHMARK(BM_Sell)->Apply(Sizes)->Iterations(kWrites);

// A /guess that changes a guess already made, which is all a second one
// does.
void BM_GuessAgain(benchmark::State& state) {
  market::Community community = Established(state, "BM_GuessAgain");
  const market::Trader trader = TraderNumber(0);
  int minute = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Guess(
        trader, SubjectNumber(0), TimeOfDay::AfterMidnight(minute), kMonday));
    minute = (minute + 1) % 1440;
  }
}
BENCHMARK(BM_GuessAgain)->Apply(Sizes);

// /market: one market's quote.
void BM_QuoteFor(benchmark::State& state) {
  market::Community community = Established(state);
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.QuoteFor(SubjectNumber(0), kMonday));
  }
}
BENCHMARK(BM_QuoteFor)->Apply(Sizes);

// /markets: every open market's.
void BM_Quotes(benchmark::State& state) {
  market::Community community = Established(state);
  for (auto _ : state) benchmark::DoNotOptimize(community.Quotes(kMonday));
}
BENCHMARK(BM_Quotes)->Apply(Sizes);

// /portfolio, for someone who is in every market.
void BM_PortfolioOf(benchmark::State& state) {
  market::Community community = Established(state);
  const market::Trader trader = TraderNumber(0);
  for (auto _ : state) benchmark::DoNotOptimize(community.PortfolioOf(trader));
}
BENCHMARK(BM_PortfolioOf)->Apply(Sizes);

// /richlist.
void BM_Richest(benchmark::State& state) {
  market::Community community = Established(state);
  for (auto _ : state) benchmark::DoNotOptimize(community.Richest(10));
}
BENCHMARK(BM_Richest)->Apply(Sizes);

// What every message in a server costs the exchange: asking whether any
// market's day is over, when none is. The markets here are all still open.
void BM_ExpireWithNothingOver(benchmark::State& state) {
  market::Community community = Established(state);
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Expire(kTuesday, NobodySaidGm));
  }
}
BENCHMARK(BM_ExpireWithNothingOver)->Apply(Sizes);

// What a GM from someone nobody bet on costs it: there is no market to
// settle.
void BM_SaidWithNoMarket(benchmark::State& state) {
  market::Community community = Established(state);
  for (auto _ : state) {
    benchmark::DoNotOptimize(community.Said(999999, kTuesday, At(8, 15)));
  }
}
BENCHMARK(BM_SaidWithNoMarket)->Apply(Sizes);

// Settling a market on its subject's GM: paying everyone in it and dealing
// with the pot. Each iteration settles a market of its own, in an exchange
// made for this run, with the first argument's worth of people in each.
void BM_Said(benchmark::State& state) {
  constexpr int kMarkets = 50;
  market::Exchange exchange = NewExchange();
  market::Community community = exchange.community(33);
  Populate(community, static_cast<int>(state.range(0)), kMarkets, kMonday);

  int subject = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        community.Said(SubjectNumber(subject++), kTuesday, At(8, 15)));
  }
}
BENCHMARK(BM_Said)->Arg(5)->Arg(40)->Iterations(50);

// Settling a whole day's markets at once, none of which saw a GM, which is
// the first thing to happen each morning. The first argument is how many
// people are in each market and the second how many markets there are; the
// time is for all of them together.
void BM_ExpireADay(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    market::Exchange exchange = NewExchange();
    market::Community community = exchange.community(33);
    Populate(community, static_cast<int>(state.range(0)),
             static_cast<int>(state.range(1)), kMonday);
    state.ResumeTiming();

    benchmark::DoNotOptimize(community.Expire(kWednesday, NobodySaidGm));
  }
}
BENCHMARK(BM_ExpireADay)
    ->Args({5, 10})
    ->Args({40, 50})
    ->Iterations(5)
    ->Unit(benchmark::kMillisecond);

}  // namespace
