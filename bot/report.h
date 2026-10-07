// What the bot says: its answers to commands and its announcements, as text
// for Discord, which reads **this** as bold and <@44> as a link to a member.
//
// Members a market is about are mentioned; traders are called by name.

#ifndef BOT_REPORT_H_
#define BOT_REPORT_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "absl/time/civil_time.h"
#include "market/exchange.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"

namespace bot {

// The answer to a command that was not carried out: `why`, a sentence, said
// so that it cannot be taken for success.
std::string Refusal(std::string_view why);

// How one of tomorrow's markets stands.
std::string QuoteReport(const market::Quote& quote);

// Tomorrow's markets, in the order given, or that there are none.
std::string MarketsReport(std::span<const market::Quote> quotes);

// What buying, or selling, shares of `outcome` came to.
std::string BoughtReport(const market::Fill& fill, market::Outcome outcome);
std::string SoldReport(const market::Fill& fill, market::Outcome outcome);

// What guessing `guess` came to.
std::string GuessReport(const market::Ticket& ticket, market::TimeOfDay guess);

// Someone's balance and holdings as of `today`, addressed to them as
// `mention`.
std::string PortfolioReport(const market::Portfolio& portfolio,
                            std::string_view mention, absl::CivilDay today);

// The richest traders, in the order given, or that there are none.
std::string RichListReport(std::span<const market::Account> richest);

// What to announce when a market has ended like this: the answer, and who
// was paid. Nothing if nobody had anything riding on it.
std::optional<std::string> SettlementAnnouncement(
    const market::Settlement& settlement);

}  // namespace bot

#endif  // BOT_REPORT_H_
