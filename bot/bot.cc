#include "bot/bot.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/functional/overload.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "bot/arguments.h"
#include "bot/config.h"
#include "bot/report.h"
#include "discord/client.h"
#include "discord/model.h"
#include "gm/phrase.h"
#include "gm/record.h"
#include "market/exchange.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"
#include "net/event_loop.h"

namespace bot {
namespace {

// How many people /richlist shows when not told, and at most.
constexpr int64_t kDefaultListSize = 10;
constexpr int64_t kMaxListSize = 25;

// The most gmbux one bet may be for: more than anyone will have, and few
// enough to count in hundredths without a care.
constexpr int64_t kMaxBet = 1'000'000'000;

// A server the bot serves, and the channel there that its announcements go
// to: the server's GM channel, or the first of them listed if it has more
// than one.
struct Venue {
  discord::GuildId guild;
  discord::ChannelId channel;

  // The day as of which every market that was over has been settled. See
  // SettleWhatIsOver.
  absl::CivilDay settled_as_of;
};

// The venue that is `guild`, or null if the bot does not serve it.
Venue* VenueOf(std::span<Venue> venues, discord::GuildId guild) {
  const auto found = std::ranges::find(venues, guild, &Venue::guild);

  return found == venues.end() ? nullptr : &*found;
}

market::Trader TraderOf(const discord::User& user) {
  return market::Trader{
      .id = user.id.value,
      .name = user.name,
  };
}

// The slash commands the bot offers. HandleCommand answers each.
std::vector<discord::Command> Commands() {
  const discord::Option member = {
      .name = "member",
      .description = "Whose GM: mention them",
      .type = discord::OptionType::kText,
      .required = true,
  };
  const discord::Option outcome = {
      .name = "outcome",
      .description = "yes (they will say GM) or no (they will not)",
      .type = discord::OptionType::kText,
      .required = true,
  };

  return {
      discord::Command{
          .name = "market",
          .description = "Show the odds on someone saying GM tomorrow",
          .options =
              {
                  member,
              },
      },
      discord::Command{
          .name = "markets",
          .description = "Show every market open for tomorrow",
      },
      discord::Command{
          .name = "bet",
          .description = "Buy shares in whether someone says GM tomorrow",
          .options =
              {
                  member,
                  outcome,
                  discord::Option{
                      .name = "gmbux",
                      .description = "How many gmbux to spend",
                      .type = discord::OptionType::kInteger,
                      .required = true,
                  },
              },
      },
      discord::Command{
          .name = "sell",
          .description = "Sell shares back before the market closes",
          .options =
              {
                  member,
                  outcome,
                  discord::Option{
                      .name = "shares",
                      .description = "How many to sell (default: all)",
                      .type = discord::OptionType::kInteger,
                      .required = false,
                  },
              },
      },
      discord::Command{
          .name = "guess",
          .description =
              "Stake 25 gmbux on when someone says GM tomorrow: nearest "
              "takes the pot",
          .options =
              {
                  member,
                  discord::Option{
                      .name = "time",
                      .description = "When, like 7:45 or 7:45pm",
                      .type = discord::OptionType::kText,
                      .required = true,
                  },
              },
      },
      discord::Command{
          .name = "portfolio",
          .description = "Show your gmbux and what you have riding",
      },
      discord::Command{
          .name = "richlist",
          .description = "Show who has the most gmbux",
          .options =
              {
                  discord::Option{
                      .name = "limit",
                      .description =
                          "How many people to show (1-25, default 10)",
                      .type = discord::OptionType::kInteger,
                      .required = false,
                  },
              },
      },
  };
}

// Announces in `venue` how each of `settlements` ended, where there is
// anything to say.
Task<absl::Status> Announce(discord::Client& client, const Venue& venue,
                            std::span<const market::Settlement> settlements) {
  for (const market::Settlement& settlement : settlements) {
    const std::optional<std::string> announcement =
        SettlementAnnouncement(settlement);
    if (!announcement.has_value()) continue;

    CO_RETURN_IF_ERROR(co_await client.Send(venue.channel, *announcement));
  }

  co_return absl::OkStatus();
}

// Settles the markets of `venue` whose day is over, and returns how they
// ended, for announcing. Nothing runs at midnight to do this: it is done
// before anything else is, the first time something happens in the server
// each day, which comes to the same. After that there is nothing to settle
// until the day changes, since a market is only ever opened for tomorrow.
//
// A failure is logged and reads as nothing settled: the markets stay open,
// and the next thing to happen tries again.
std::vector<market::Settlement> SettleWhatIsOver(market::Community community,
                                                 gm::Record& record,
                                                 Venue& venue,
                                                 absl::CivilDay today) {
  if (venue.settled_as_of == today) return {};

  // A GM this bot was not running to see is still on gm_bot's record.
  const auto said_gm = [&record, &venue](const uint64_t subject,
                                         const absl::CivilDay day) {
    return record.SaidGm(venue.guild.value, subject, day);
  };

  absl::StatusOr<std::vector<market::Settlement>> settlements =
      community.Expire(today, said_gm);
  if (!settlements.ok()) {
    LOG(ERROR) << "could not settle the markets that are over: "
               << settlements.status();
    return {};
  }

  venue.settled_as_of = today;
  return std::move(*settlements);
}

// Handles something a person wrote in a GM channel: if it is a GM, the
// market on them for today is settled.
Task<absl::Status> HandleMessage(discord::Client& client,
                                 market::Community community,
                                 gm::Record& record, const Venue& venue,
                                 absl::CivilMinute now,
                                 const discord::Message& message) {
  // Read each time, so that a phrase counts from the moment gm_bot adds it.
  CO_ASSIGN_OR_RETURN(const std::vector<gm::Phrase> phrases,
                      record.Phrases(venue.guild.value));
  if (!gm::SaysGm(message.content, phrases, now.hour())) {
    co_return absl::OkStatus();
  }

  CO_ASSIGN_OR_RETURN(
      const std::optional<market::Settlement> settlement,
      community.Said(message.author.id.value, absl::CivilDay(now),
                     market::TimeOfDay(now)));
  if (!settlement.has_value()) co_return absl::OkStatus();

  co_return co_await Announce(
      client, venue, std::span<const market::Settlement>(&*settlement, 1));
}

// Answers `command` with `report` of what `result` holds, or, if the
// exchange refused, with why.
template <typename Result, typename Report>
Task<absl::Status> RespondWith(discord::Client& client,
                               const discord::CommandInvoked& command,
                               const absl::StatusOr<Result>& result,
                               Report report) {
  // A refusal is the user's to hear about, not a failure.
  if (market::IsRefusal(result.status())) {
    co_return co_await client.Respond(command,
                                      Refusal(result.status().message()));
  }
  CO_RETURN_IF_ERROR(result.status());

  co_return co_await client.Respond(command, report(*result));
}

// Answers a command that is about one member's market: /market, /bet,
// /sell and /guess.
Task<absl::Status> HandleMarketCommand(discord::Client& client,
                                       market::Community community,
                                       absl::CivilDay today,
                                       const discord::CommandInvoked& command) {
  // What was typed badly is the user's to hear about as well.
  const absl::StatusOr<discord::UserId> member =
      ParseMember(command.Text("member"));
  if (!member.ok()) {
    co_return co_await client.Respond(command,
                                      Refusal(member.status().message()));
  }
  const uint64_t subject = member->value;
  const market::Trader trader = TraderOf(command.user);

  if (command.name == "market") {
    co_return co_await RespondWith(
        client, command, community.QuoteFor(subject, today), &QuoteReport);
  }

  if (command.name == "guess") {
    const absl::StatusOr<market::TimeOfDay> time =
        market::TimeOfDay::Parse(command.Text("time"));
    if (!time.ok()) {
      co_return co_await client.Respond(command,
                                        Refusal(time.status().message()));
    }

    co_return co_await RespondWith(
        client, command, community.Guess(trader, subject, *time, today),
        [&time](const market::Ticket& ticket) {
          return GuessReport(ticket, *time);
        });
  }

  const absl::StatusOr<market::Outcome> outcome =
      ParseOutcome(command.Text("outcome"));
  if (!outcome.ok()) {
    co_return co_await client.Respond(command,
                                      Refusal(outcome.status().message()));
  }

  if (command.name == "bet") {
    const market::Gmbux budget = market::Gmbux::Whole(
        std::clamp<int64_t>(command.Integer("gmbux", 0), 0, kMaxBet));

    co_return co_await RespondWith(
        client, command,
        community.Buy(trader, subject, *outcome, budget, today),
        [&outcome](const market::Fill& fill) {
          return BoughtReport(fill, *outcome);
        });
  }

  // /sell. Left out, the number of shares is all of them.
  constexpr int64_t kAll = std::numeric_limits<int64_t>::min();
  const int64_t shares = command.Integer("shares", kAll);

  co_return co_await RespondWith(
      client, command,
      community.Sell(trader, subject, *outcome,
                     shares == kAll ? std::nullopt : std::optional(shares),
                     today),
      [&outcome](const market::Fill& fill) {
        return SoldReport(fill, *outcome);
      });
}

// Answers someone's use of one of the commands above.
Task<absl::Status> HandleCommand(discord::Client& client,
                                 market::Community community,
                                 absl::CivilDay today,
                                 const discord::CommandInvoked& command) {
  if (command.name == "market" || command.name == "bet" ||
      command.name == "sell" || command.name == "guess") {
    co_return co_await HandleMarketCommand(client, community, today, command);
  }

  if (command.name == "markets") {
    CO_ASSIGN_OR_RETURN(const std::vector<market::Quote> quotes,
                        community.Quotes(today));

    co_return co_await client.Respond(command, MarketsReport(quotes));
  }

  if (command.name == "portfolio") {
    CO_ASSIGN_OR_RETURN(const market::Portfolio portfolio,
                        community.PortfolioOf(TraderOf(command.user)));

    co_return co_await client.Respond(
        command,
        PortfolioReport(portfolio, discord::Mention(command.user.id), today));
  }

  if (command.name == "richlist") {
    const int limit = static_cast<int>(std::clamp<int64_t>(
        command.Integer("limit", kDefaultListSize), 1, kMaxListSize));
    CO_ASSIGN_OR_RETURN(const std::vector<market::Account> richest,
                        community.Richest(limit));

    co_return co_await client.Respond(command, RichListReport(richest));
  }

  // A command from an earlier version of the bot, which Discord has yet to
  // forget.
  co_return co_await client.Respond(command,
                                    "I don't know that command any more.");
}

// Deals with a message the bot has been told of, if it is in a server the
// bot serves. A failure is logged and otherwise ignored: one message going
// wrong is no reason to stop serving.
Task<> OnMessage(discord::Client& client, market::Exchange& exchange,
                 gm::Record& record, const Config& config,
                 std::span<Venue> venues, const discord::Message& message) {
  // Bots, this one included, do not have mornings.
  if (message.author.bot) co_return;

  Venue* const venue = VenueOf(venues, message.guild);
  if (venue == nullptr) co_return;

  const market::Community community = exchange.community(venue->guild.value);
  const absl::CivilMinute now =
      absl::ToCivilMinute(absl::Now(), config.time_zone);

  // Yesterday's markets first, so that they are announced in order.
  const std::vector<market::Settlement> over =
      SettleWhatIsOver(community, record, *venue, absl::CivilDay(now));
  const absl::Status announced = co_await Announce(client, *venue, over);
  if (!announced.ok()) {
    LOG(ERROR) << "could not announce how a market ended: " << announced;
  }

  // Only what is said in a GM channel can be a GM.
  if (std::ranges::find(config.channels, message.channel) ==
      config.channels.end()) {
    co_return;
  }

  const absl::Status handled =
      co_await HandleMessage(client, community, record, *venue, now, message);
  if (!handled.ok()) {
    LOG(ERROR) << "could not handle a message from " << message.author.name
               << ": " << handled;
  }
}

// Deals with someone's use of a command, likewise.
Task<> OnCommand(discord::Client& client, market::Exchange& exchange,
                 gm::Record& record, const Config& config,
                 std::span<Venue> venues,
                 const discord::CommandInvoked& command) {
  // Used in a direct message, or in a server whose commands date from when
  // the bot served it, there is no market to answer about.
  Venue* const venue = VenueOf(venues, command.guild);
  if (venue == nullptr) {
    const absl::Status answered = co_await client.Respond(
        command, "I only work in a server with a GM channel, sorry.");
    if (!answered.ok()) {
      LOG(ERROR) << "could not answer /" << command.name << ": " << answered;
    }
    co_return;
  }

  const market::Community community = exchange.community(venue->guild.value);
  const absl::CivilDay today = absl::ToCivilDay(absl::Now(), config.time_zone);

  // Settled before the command is answered, so that the answer has the
  // winnings in it, and announced after: Discord allows a command only a few
  // seconds, and an announcement can wait.
  const std::vector<market::Settlement> over =
      SettleWhatIsOver(community, record, *venue, today);

  const absl::Status handled =
      co_await HandleCommand(client, community, today, command);
  if (!handled.ok()) {
    LOG(ERROR) << "could not answer /" << command.name << " from "
               << command.user.name << ": " << handled;
  }

  const absl::Status announced = co_await Announce(client, *venue, over);
  if (!announced.ok()) {
    LOG(ERROR) << "could not announce how a market ended: " << announced;
  }
}

// Gets things ready for serving: finds the server of each GM channel, which
// also shows that the bot can see the channel, and offers the commands
// there. Returns the servers found.
Task<absl::StatusOr<std::vector<Venue>>> Prepare(discord::Client& client,
                                                 const Config& config) {
  const std::vector<discord::Command> commands = Commands();

  std::vector<Venue> venues;
  for (const discord::ChannelId channel : config.channels) {
    CO_ASSIGN_OR_RETURN(
        const discord::GuildId guild, co_await client.GuildOf(channel),
        _.SetPrepend() << "GM channel " << channel.value << ": ");

    // A server with a second GM channel has its commands already.
    if (VenueOf(venues, guild) != nullptr) continue;

    CO_RETURN_IF_ERROR(co_await client.OfferCommands(guild, commands))
        << "registering the bot's commands";
    LOG(INFO) << "running the markets of server " << guild.value
              << ", announcing in channel " << channel.value;

    venues.push_back(Venue{
        .guild = guild,
        .channel = channel,
    });
  }

  co_return venues;
}

// Everything Run does that has to happen on an event loop.
Task<absl::Status> ConnectAndServe(const Config& config,
                                   market::Exchange& exchange,
                                   gm::Record& record) {
  CO_ASSIGN_OR_RETURN(discord::Client client,
                      co_await discord::Client::Connect(config.token),
                      _.SetPrepend() << "connecting to Discord: ");
  LOG(INFO) << "connected to Discord as " << client.self().name;

  co_return co_await Serve(client, exchange, record, config);
}

}  // namespace

Task<absl::Status> Serve(discord::Client& client, market::Exchange& exchange,
                         gm::Record& record, const Config& config) {
  CO_ASSIGN_OR_RETURN(std::vector<Venue> venues,
                      co_await Prepare(client, config));

  for (;;) {
    CO_ASSIGN_OR_RETURN(const discord::Event event,
                        co_await client.NextEvent());

    co_await std::visit(absl::Overload{
                            [&](const discord::MessageCreated& created) {
                              return OnMessage(client, exchange, record, config,
                                               venues, created.message);
                            },
                            [&](const discord::CommandInvoked& invoked) {
                              return OnCommand(client, exchange, record, config,
                                               venues, invoked);
                            },
                        },
                        event);
  }
}

absl::Status Run(const Config& config) {
  ABSL_ASSIGN_OR_RETURN(market::Exchange exchange,
                        market::Exchange::Open(config.database),
                        _.SetPrepend() << "opening the exchange: ");
  LOG(INFO) << "keeping the markets in " << config.database;

  ABSL_ASSIGN_OR_RETURN(gm::Record record, gm::Record::Open(config.ledger),
                        _.SetPrepend() << "GM_DATABASE: ");
  LOG(INFO) << "reading what counts as a GM from " << config.ledger;

  ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());
  return loop.Run(ConnectAndServe(config, exchange, record));
}

}  // namespace bot
