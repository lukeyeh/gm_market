#include "bot/report.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/time/civil_time.h"
#include "discord/model.h"
#include "market/exchange.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"

namespace bot {
namespace {

// The most entries a list shows. Discord refuses a message much longer than
// this makes one.
constexpr size_t kMostEntries = 15;

std::string Mention(const uint64_t member) {
  return discord::Mention(discord::UserId{
      .value = member,
  });
}

// "1 share", "83 shares".
std::string Counted(const int64_t count, const std::string_view thing) {
  return absl::StrCat(count, " ", thing, count == 1 ? "" : "s");
}

// "49.83 gmbux". The word is the same for one as for many.
std::string Amount(const market::Gmbux amount) {
  return absl::StrCat(amount.Describe(), " gmbux");
}

std::string_view NameOf(const market::Outcome outcome) {
  return outcome == market::Outcome::kYes ? "YES" : "NO";
}

// A chance as a whole percentage. One that is not settled is never shown
// as certain.
int Percent(const double chance) {
  return std::clamp(static_cast<int>(std::lround(chance * 100)), 1, 99);
}

// "YES 62% · NO 38%".
std::string Odds(const market::Quote& quote) {
  const int yes = Percent(quote.chance);

  return absl::StrCat("YES ", yes, "% · NO ", 100 - yes, "%");
}

// "83 YES", "12 NO", "83 YES and 12 NO", or nothing for none.
std::string Held(const market::Shares shares) {
  std::vector<std::string> sides;
  if (shares.yes > 0) sides.push_back(absl::StrCat(shares.yes, " YES"));
  if (shares.no > 0) sides.push_back(absl::StrCat(shares.no, " NO"));

  return absl::StrJoin(sides, " and ");
}

// "50 gmbux, on 2 guesses".
std::string Pot(const market::Quote& quote) {
  return absl::StrCat(Amount(quote.pot), ", on ", quote.guesses,
                      quote.guesses == 1 ? " guess" : " guesses");
}

// "a", "a and b", "a, b and c".
std::string Listed(const std::span<const std::string> items) {
  if (items.size() < 2) return absl::StrJoin(items, "");

  return absl::StrCat(absl::StrJoin(items.first(items.size() - 1), ", "),
                      " and ", items.back());
}

// Says after `text`, which lists the first of `total` entries, how many it
// left out.
void NoteTheRest(std::string& text, const size_t total) {
  if (total > kMostEntries) {
    absl::StrAppend(&text, "\n…and ", total - kMostEntries, " more.");
  }
}

// When a market's day is, seen from `today`.
std::string DayOf(const absl::CivilDay day, const absl::CivilDay today) {
  if (day == today) return "today";
  if (day == today + 1) return "tomorrow";

  return absl::StrCat("on ", absl::FormatCivilTime(day));
}

}  // namespace

std::string Refusal(const std::string_view why) {
  return absl::StrCat("❌ ", why);
}

std::string QuoteReport(const market::Quote& quote) {
  std::string report = absl::StrCat("**Will ", Mention(quote.subject),
                                    " say GM tomorrow?**\n", Odds(quote), "\n");

  if (quote.sold == market::Shares{}) {
    absl::StrAppend(&report, "No shares are held yet.");
  } else {
    absl::StrAppend(&report, quote.sold.yes, " YES and ", quote.sold.no,
                    " NO shares are held.");
  }

  if (quote.guesses > 0) {
    absl::StrAppend(&report, "\nThe time pot is ", Pot(quote), ".");
  }

  return report;
}

std::string MarketsReport(const std::span<const market::Quote> quotes) {
  if (quotes.empty()) {
    return "There are no markets for tomorrow yet. `/bet` or `/guess` opens "
           "one.";
  }

  std::string report = "**Tomorrow's markets**";
  for (const market::Quote& quote :
       quotes.first(std::min(quotes.size(), kMostEntries))) {
    absl::StrAppend(&report, "\n• ", Mention(quote.subject), ": ", Odds(quote),
                    " · ", Counted(quote.sold.yes + quote.sold.no, "share"));
    if (quote.guesses > 0) absl::StrAppend(&report, " · pot ", Pot(quote));
  }
  NoteTheRest(report, quotes.size());

  return report;
}

std::string BoughtReport(const market::Fill& fill,
                         const market::Outcome outcome) {
  return absl::StrCat(
      "✅ Bought **", fill.shares, " ", NameOf(outcome), " ",
      fill.shares == 1 ? "share" : "shares", "** on ",
      Mention(fill.quote.subject), " saying GM tomorrow, for **",
      Amount(fill.amount), "**. The market now says ", Odds(fill.quote),
      ". You hold ", fill.held, " and have ", Amount(fill.balance), ".");
}

std::string SoldReport(const market::Fill& fill,
                       const market::Outcome outcome) {
  return absl::StrCat(
      "✅ Sold **", fill.shares, " ", NameOf(outcome), " ",
      fill.shares == 1 ? "share" : "shares", "** on ",
      Mention(fill.quote.subject), " saying GM tomorrow, for **",
      Amount(fill.amount), "**. The market now says ", Odds(fill.quote),
      ". You hold ", fill.held, " and have ", Amount(fill.balance), ".");
}

std::string GuessReport(const market::Ticket& ticket,
                        const market::TimeOfDay guess) {
  const std::string subject = Mention(ticket.quote.subject);

  // A changed guess cost nothing new.
  if (ticket.staked == market::Gmbux()) {
    return absl::StrCat("🕖 Your guess for ", subject, " is now **",
                        guess.Describe(), "**. The pot is ", Pot(ticket.quote),
                        ".");
  }

  return absl::StrCat("🕖 You guessed that ", subject, " will say GM at **",
                      guess.Describe(), "** tomorrow, for ",
                      Amount(ticket.staked), ". The pot is ", Pot(ticket.quote),
                      ". You have ", Amount(ticket.balance), ".");
}

std::string PortfolioReport(const market::Portfolio& portfolio,
                            const std::string_view mention,
                            const absl::CivilDay today) {
  std::string report =
      absl::StrCat(mention, ", you have **", Amount(portfolio.balance), "**.");
  if (portfolio.holdings.empty()) {
    absl::StrAppend(&report, "\nNothing is riding on any market.");
    return report;
  }

  const std::span<const market::Holding> holdings = portfolio.holdings;
  for (const market::Holding& holding :
       holdings.first(std::min(holdings.size(), kMostEntries))) {
    std::vector<std::string> stakes;
    if (holding.shares != market::Shares{}) {
      stakes.push_back(Held(holding.shares));
    }
    if (holding.guess.has_value()) {
      stakes.push_back(absl::StrCat("guessed ", holding.guess->Describe()));
    }

    absl::StrAppend(&report, "\n• ", Mention(holding.subject), " ",
                    DayOf(holding.day, today), ": ",
                    absl::StrJoin(stakes, " · "));
  }
  NoteTheRest(report, holdings.size());

  return report;
}

std::string RichListReport(const std::span<const market::Account> richest) {
  if (richest.empty()) return "Nobody has traded yet.";

  std::string report = "**The rich list**";
  int place = 0;
  for (const market::Account& account : richest) {
    absl::StrAppend(&report, "\n", ++place, ". **", account.trader.name,
                    "**: ", Amount(account.balance));
  }

  return report;
}

std::optional<std::string> SettlementAnnouncement(
    const market::Settlement& settlement) {
  if (settlement.holders == 0 && settlement.pot_payouts.empty()) {
    return std::nullopt;
  }

  const std::string subject = Mention(settlement.subject);
  const std::string date = absl::FormatCivilTime(settlement.day);

  std::string text;
  if (settlement.outcome == market::Outcome::kNo) {
    text = absl::StrCat("📉 ", subject, " did not say GM on ", date,
                        ": NO it is.");
  } else if (settlement.said_at.has_value()) {
    text = absl::StrCat("📈 ", subject, " said GM at **",
                        settlement.said_at->Describe(), "**: YES it is.");
  } else {
    text = absl::StrCat("📈 ", subject, " said GM on ", date, ": YES it is.");
  }

  // The shares.
  if (settlement.holders > 0 && settlement.winnings.empty()) {
    absl::StrAppend(&text, "\nNobody held the winning side.");
  } else if (settlement.holders > 0) {
    const std::span<const market::Payout> winnings = settlement.winnings;

    std::vector<std::string> paid;
    for (const market::Payout& payout :
         winnings.first(std::min(winnings.size(), kMostEntries))) {
      paid.push_back(
          absl::StrCat("**", payout.trader.name, "** ", Amount(payout.amount)));
    }

    absl::StrAppend(&text, "\nPaid: ", absl::StrJoin(paid, ", "), ".");
    NoteTheRest(text, winnings.size());
  }

  // The pot.
  if (settlement.pot_payouts.empty()) return text;

  if (settlement.pot_returned) {
    absl::StrAppend(&text, "\n⏱️ The time pot of ", Amount(settlement.pot),
                    " goes back to those who guessed.");
    return text;
  }

  std::vector<std::string> nearest;
  nearest.reserve(settlement.pot_payouts.size());
  for (const market::Payout& payout : settlement.pot_payouts) {
    nearest.push_back(absl::StrCat("**", payout.trader.name, "**"));
  }

  if (nearest.size() == 1) {
    absl::StrAppend(
        &text, "\n⏱️ ", nearest.front(), " guessed nearest, with ",
        settlement.nearest_guess.value_or(market::TimeOfDay()).Describe(),
        ", and takes the time pot of ", Amount(settlement.pot), ".");
  } else {
    absl::StrAppend(&text, "\n⏱️ ", Listed(nearest),
                    " guessed equally near and share the time pot of ",
                    Amount(settlement.pot), ".");
  }

  return text;
}

}  // namespace bot
