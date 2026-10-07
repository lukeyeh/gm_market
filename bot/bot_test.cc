// The bot by example: what it does, seen from Discord's side, when people
// trade in its markets and when the people those markets are about say GM.

#include "bot/bot.h"

#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/strip.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "async/task.h"
#include "bot/config.h"
#include "discord/client.h"
#include "discord/model.h"
#include "gm/fake_ledger.h"
#include "gm/record.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "http/fake_client.h"
#include "json/json.h"
#include "market/exchange.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"
#include "net/event_loop.h"
#include "websocket/fake_server.h"

namespace {

using absl_testing::StatusIs;
using market::Gmbux;
using market::Outcome;
using testing::AllOf;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::StartsWith;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

constexpr char kApi[] = "https://discord.com/api/v10";

// The day the bot takes it to be, in the time zone the tests give it.
absl::CivilDay Today() {
  return absl::ToCivilDay(absl::Now(), absl::UTCTimeZone());
}

// A message as the gateway reports it. Ids are small numbers for legibility:
// the message's own id is its sequence number.
struct Said {
  int sequence = 0;
  int channel = 22;
  int guild = 33;
  int author = 44;
  std::string_view name = "luke";
  bool bot = false;
  std::string_view content;
};

std::string Dispatch(const Said& said) {
  return absl::StrCat(R"({"op":0,"t":"MESSAGE_CREATE","s":)", said.sequence,
                      R"(,"d":{"id":")", said.sequence, R"(","channel_id":")",
                      said.channel, R"(","guild_id":")", said.guild,
                      R"(","content":")", said.content, R"(","author":{"id":")",
                      said.author, R"(","username":")", said.name,
                      R"(","bot":)", said.bot ? "true" : "false", "}}}");
}

// Someone using a slash command, as the gateway reports it. `options` are
// the ones they filled in, each made by Text or Integer below.
struct Used {
  int sequence = 0;
  // The server it was used in, or 0 for a direct message.
  int guild = 33;
  int author = 45;
  std::string_view name = "ada";
  std::string_view command;
  std::vector<std::string> options;
};

std::string Text(std::string_view option, std::string_view value) {
  return absl::StrCat(R"({"name":")", option, R"(","type":3,"value":")", value,
                      R"("})");
}

std::string Integer(std::string_view option, int value) {
  return absl::StrCat(R"({"name":")", option, R"(","type":4,"value":)", value,
                      "}");
}

std::string Dispatch(const Used& used) {
  return absl::StrCat(
      R"({"op":0,"t":"INTERACTION_CREATE","s":)", used.sequence,
      R"(,"d":{"type":2,"id":")", used.sequence, R"(","token":"token-)",
      used.sequence, R"(","channel_id":"22","guild_id":)",
      used.guild == 0 ? "null" : absl::StrCat("\"", used.guild, "\""),
      R"(,"data":{"name":")", used.command, R"(","options":[)",
      absl::StrJoin(used.options, ","), R"(]},"member":{"user":{"id":")",
      used.author, R"(","username":")", used.name, R"("}}}})");
}

// What the bot said, as Discord's API saw it: "REPLY <n>: <text>" for its
// answer to the command numbered n, and "SAY <channel>: <text>" for a
// message it posted.
std::vector<std::string> Actions(const http::FakeClient& http) {
  std::vector<std::string> actions;
  for (const http::Request& request : http.requests()) {
    const std::string_view path =
        std::string_view(request.url).substr(std::string_view(kApi).size());
    // Finding the gateway, registering the commands, and the question about
    // each channel that goes with it, have tests of their own.
    if (request.method != http::Method::kPost) continue;

    const absl::StatusOr<json::Value> body = json::Parse(request.body);
    ABSL_EXPECT_OK(body);
    if (!body.ok()) continue;

    std::string_view rest = path;
    if (absl::ConsumePrefix(&rest, "/interactions/")) {
      actions.push_back(absl::StrCat("REPLY ", rest.substr(0, rest.find('/')),
                                     ": ",
                                     (*body)["data"]["content"].AsString()));
    } else if (absl::ConsumePrefix(&rest, "/channels/")) {
      actions.push_back(absl::StrCat("SAY ", rest.substr(0, rest.find('/')),
                                     ": ", (*body)["content"].AsString()));
    } else {
      ADD_FAILURE() << "the bot posted to " << path;
    }
  }
  return actions;
}

// Runs the bot against a Discord that sends `dispatches` and then revokes
// the bot's token, which is what ends Serve. Returns what the bot did.
struct Result {
  absl::Status stopped;
  std::vector<std::string> actions;
};

Task<Result> ServeEvents(std::vector<std::string> dispatches,
                         market::Exchange& exchange, gm::Record& record,
                         bot::Config config,
                         std::vector<http::Response> answers = {}) {
  auto http = std::make_unique<http::FakeClient>();
  const http::FakeClient& requests = *http;
  http->Answer(http::Response{
      .status = 200,
      .body = R"({"url":"wss://gateway.test"})",
  });
  // On starting, the bot asks which server its channel is in.
  if (answers.empty()) {
    answers.push_back(http::Response{
        .status = 200,
        .body = R"({"id":"22","guild_id":"33"})",
    });
  }
  for (http::Response& answer : answers) http->Answer(std::move(answer));

  websocket::FakeServer gateway;
  gateway.Sends(R"({"op":10,"d":{"heartbeat_interval":3600000}})");
  gateway.Sends(R"({"op":0,"s":1,"t":"READY","d":{"session_id":"abc",
      "resume_gateway_url":"wss://resume.test",
      "user":{"id":"99","username":"gm_market","bot":true},
      "application":{"id":"77"}}})");
  for (std::string& dispatch : dispatches) gateway.Sends(std::move(dispatch));
  gateway.Closes(4004);

  absl::StatusOr<discord::Client> client = co_await discord::Client::Connect(
      "secret", std::move(http), gateway.connector());
  ABSL_EXPECT_OK(client);
  if (!client.ok()) co_return Result{};

  const absl::Status stopped =
      co_await bot::Serve(*client, exchange, record, config);
  co_return Result{
      .stopped = stopped,
      .actions = Actions(requests),
  };
}

market::Exchange NewExchange() {
  absl::StatusOr<market::Exchange> exchange = market::Exchange::InMemory();
  ABSL_EXPECT_OK(exchange);
  return std::move(*exchange);
}

gm::FakeLedger NewLedger() {
  absl::StatusOr<gm::FakeLedger> ledger = gm::FakeLedger::InMemory();
  ABSL_EXPECT_OK(ledger);
  return std::move(*ledger);
}

// What gm_bot has on record, given the ledger it wrote.
gm::Record RecordOf(gm::FakeLedger ledger) {
  absl::StatusOr<gm::Record> record =
      gm::Record::Over(std::move(ledger).Take());
  ABSL_EXPECT_OK(record);
  return std::move(*record);
}

// The GM channel is 22, in server 33, which is where Said and Used put
// things unless told not to.
bot::Config InChannel22() {
  return bot::Config{
      .token = "secret",
      .channels =
          {
              discord::ChannelId{
                  .value = 22,
              },
          },
      .time_zone = absl::UTCTimeZone(),
  };
}

// Ada, who uses the commands unless a test says otherwise, as a trader.
market::Trader Ada() {
  return market::Trader{
      .id = 45,
      .name = "ada",
  };
}

// The whole point, first half: someone bets that luke will say GM tomorrow.
// The bot answers with what they bought, and the exchange has it.
TEST(BotTest, TakesABet) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());

    const Result result = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "bet",
                .options =
                    {
                        Text("member", "<@44>"),
                        Text("outcome", "yes"),
                        Integer("gmbux", 50),
                    },
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(result.stopped, StatusIs(absl::StatusCode::kUnauthenticated));
    EXPECT_THAT(
        result.actions,
        ElementsAre("REPLY 2: ✅ Bought **83 YES shares** on <@44> saying GM "
                    "tomorrow, for **49.88 gmbux**. The market now says YES "
                    "70% · NO 30%. You hold 83 and have 950.12 gmbux."));

    const absl::StatusOr<market::Quote> quote =
        exchange.community(33).QuoteFor(44, Today());
    ABSL_EXPECT_OK(quote);
    if (!quote.ok()) co_return;
    EXPECT_EQ(quote->sold.yes, 83);
  }());
}

// The whole point, second half: the next day luke says GM. The market on
// him settles there and then, and the bot announces who was paid.
TEST(BotTest, SettlesAMarketWhenItsSubjectSaysGm) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    // Yesterday, ada bet that he would and grace guessed when.
    ABSL_EXPECT_OK(exchange.community(33).Buy(Ada(), 44, Outcome::kYes,
                                              Gmbux::Whole(50), Today() - 1));
    ABSL_EXPECT_OK(exchange.community(33).Guess(
        market::Trader{
            .id = 46,
            .name = "grace",
        },
        44, market::TimeOfDay(), Today() - 1));

    const Result result = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm everyone",
            }),
            // Only the first GM of the day settles anything.
            Dispatch(Said{
                .sequence = 3,
                .content = "gm again",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(
        result.actions,
        ElementsAre(AllOf(StartsWith("SAY 22: 📈 <@44> said GM at **"),
                          HasSubstr("YES it is.\nPaid: **ada** 83 gmbux.\n"),
                          HasSubstr("**grace** guessed nearest, with 00:00, "
                                    "and takes the time pot of 25 gmbux."))));

    const absl::StatusOr<market::Portfolio> portfolio =
        exchange.community(33).PortfolioOf(Ada());
    ABSL_EXPECT_OK(portfolio);
    if (!portfolio.ok()) co_return;
    EXPECT_EQ(portfolio->balance, Gmbux::Hundredths(103312));
    EXPECT_THAT(portfolio->holdings, IsEmpty());
  }());
}

// A GM from someone nobody bet on settles nothing, and the bot keeps quiet:
// the GM channel is gm_bot's to answer in.
TEST(BotTest, SaysNothingAboutAGmNobodyBetOn) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());

    const Result result = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(result.actions, IsEmpty());
  }());
}

// Only a GM settles a market as yes: not other talk in the GM channel, not
// "gm" said in some other channel, and not anything a bot says.
TEST(BotTest, OnlyAGmInAGmChannelFromAPersonCounts) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    ABSL_EXPECT_OK(exchange.community(33).Buy(Ada(), 44, Outcome::kYes,
                                              Gmbux::Whole(50), Today() - 1));

    const Result result = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "what time is it",
            }),
            Dispatch(Said{
                .sequence = 3,
                .channel = 23,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 4,
                .bot = true,
                .content = "gm",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(result.actions, IsEmpty());

    // Still open.
    const absl::StatusOr<market::Portfolio> portfolio =
        exchange.community(33).PortfolioOf(Ada());
    ABSL_EXPECT_OK(portfolio);
    if (!portfolio.ok()) co_return;
    EXPECT_THAT(portfolio->holdings, testing::SizeIs(1));
  }());
}

// What counts as a GM is whatever gm_bot's ledger says for that server:
// here "howdy" does, and "gm" no longer.
TEST(BotTest, GoesByThePhrasesInGmBotsLedger) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::FakeLedger ledger = NewLedger();
    ABSL_EXPECT_OK(ledger.AddPhrase(33, "howdy"));
    gm::Record record = RecordOf(std::move(ledger));
    ABSL_EXPECT_OK(exchange.community(33).Buy(Ada(), 44, Outcome::kYes,
                                              Gmbux::Whole(50), Today() - 1));

    const Result result = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "Howdy partners",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(result.actions,
                ElementsAre(StartsWith("SAY 22: 📈 <@44> said GM at **")));
  }());
}

// A day that went by without a GM settles its markets as no. Nothing has to
// happen at midnight for that: the next thing anyone says in the server, in
// any channel, brings it about.
TEST(BotTest, SettlesADayWithoutAGmAtTheNextSignOfLife) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    // The day before yesterday, ada bet luke would not say GM yesterday.
    const absl::StatusOr<market::Fill> bet = exchange.community(33).Buy(
        Ada(), 44, Outcome::kNo, Gmbux::Whole(50), Today() - 2);
    ABSL_EXPECT_OK(bet);

    const Result result = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .channel = 23,
                .author = 50,
                .name = "passerby",
                .content = "anyone around?",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(result.actions, ElementsAre(absl::StrCat(
                                    "SAY 22: 📉 <@44> did not say GM on ",
                                    absl::FormatCivilTime(Today() - 1),
                                    ": NO it is.\nPaid: **ada** 83 gmbux.")));
  }());
}

// If the bot was not running when the GM was said, gm_bot still recorded
// it, and the market settles as yes on that record once the day is over.
TEST(BotTest, TakesGmBotsWordForAGmItDidNotSee) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::FakeLedger ledger = NewLedger();
    ABSL_EXPECT_OK(ledger.RecordGm(33, 44, Today() - 1));
    gm::Record record = RecordOf(std::move(ledger));
    ABSL_EXPECT_OK(exchange.community(33).Buy(Ada(), 44, Outcome::kYes,
                                              Gmbux::Whole(50), Today() - 2));

    const Result result = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "portfolio",
            }),
        },
        exchange, record, InChannel22());

    // The command is answered first, with the winnings already in it, and
    // the announcement follows.
    EXPECT_THAT(
        result.actions,
        ElementsAre("REPLY 2: <@45>, you have **1033.12 gmbux**.\n"
                    "Nothing is riding on any market.",
                    absl::StrCat("SAY 22: 📈 <@44> said GM on ",
                                 absl::FormatCivilTime(Today() - 1),
                                 ": YES it is.\nPaid: **ada** 83 gmbux.")));
  }());
}

// A refusal from the exchange is an answer to the person, not a failure of
// the bot.
TEST(BotTest, TellsSomeoneWhyTheirBetWasRefused) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());

    const Result result = co_await ServeEvents(
        {
            // More than she has.
            Dispatch(Used{
                .sequence = 2,
                .command = "bet",
                .options =
                    {
                        Text("member", "<@44>"),
                        Text("outcome", "yes"),
                        Integer("gmbux", 5000),
                    },
            }),
            // Against herself.
            Dispatch(Used{
                .sequence = 3,
                .command = "bet",
                .options =
                    {
                        Text("member", "<@45>"),
                        Text("outcome", "no"),
                        Integer("gmbux", 10),
                    },
            }),
            // On her own time.
            Dispatch(Used{
                .sequence = 4,
                .command = "guess",
                .options =
                    {
                        Text("member", "<@45>"),
                        Text("time", "7:45"),
                    },
            }),
            // Of what she does not hold.
            Dispatch(Used{
                .sequence = 5,
                .command = "sell",
                .options =
                    {
                        Text("member", "<@44>"),
                        Text("outcome", "yes"),
                    },
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(
        result.actions,
        ElementsAre("REPLY 2: ❌ You only have 1000 gmbux.",
                    "REPLY 3: ❌ You can back yourself to say GM, but you "
                    "cannot bet against it.",
                    "REPLY 4: ❌ You cannot guess when you will say GM "
                    "yourself: it is yours to choose.",
                    "REPLY 5: ❌ You hold no YES shares in that market."));
  }());
}

// So is something typed that the bot cannot make sense of.
TEST(BotTest, TellsSomeoneWhatItCouldNotRead) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());

    const Result result = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "market",
                .options =
                    {
                        Text("member", "luke"),
                    },
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "bet",
                .options =
                    {
                        Text("member", "<@44>"),
                        Text("outcome", "maybe"),
                        Integer("gmbux", 10),
                    },
            }),
            Dispatch(Used{
                .sequence = 4,
                .command = "guess",
                .options =
                    {
                        Text("member", "<@44>"),
                        Text("time", "early"),
                    },
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(
        result.actions,
        ElementsAre("REPLY 2: ❌ Say who by mentioning them, like `@luke`.",
                    "REPLY 3: ❌ The outcome is either `yes` or `no`.",
                    "REPLY 4: ❌ Give a time like `7:45`, `19:30` or "
                    "`7:45pm`."));
  }());
}

// A day at the market: a guess, a look at the odds, selling some, and
// seeing where that leaves things.
TEST(BotTest, AnswersTheRestOfItsCommands) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    ABSL_EXPECT_OK(exchange.community(33).Buy(Ada(), 44, Outcome::kYes,
                                              Gmbux::Whole(50), Today()));

    const Result result = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "guess",
                .options =
                    {
                        Text("member", "<@44>"),
                        Text("time", "7:45am"),
                    },
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "market",
                .options =
                    {
                        Text("member", "<@44>"),
                    },
            }),
            Dispatch(Used{
                .sequence = 4,
                .command = "sell",
                .options =
                    {
                        Text("member", "<@44>"),
                        Text("outcome", "YES"),
                        Integer("shares", 33),
                    },
            }),
            Dispatch(Used{
                .sequence = 5,
                .command = "markets",
            }),
            Dispatch(Used{
                .sequence = 6,
                .command = "portfolio",
            }),
            Dispatch(Used{
                .sequence = 7,
                .command = "richlist",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(
        result.actions,
        ElementsAre(
            "REPLY 2: 🕖 You guessed that <@44> will say GM at **07:45** "
            "tomorrow, for 25 gmbux. The pot is 25 gmbux, on 1 guess. You "
            "have 925.12 gmbux.",
            "REPLY 3: **Will <@44> say GM tomorrow?**\n"
            "YES 70% · NO 30%\n"
            "83 YES and 0 NO shares are held.\n"
            "The time pot is 25 gmbux, on 1 guess.",
            AllOf(StartsWith("REPLY 4: ✅ Sold **33 YES shares** on <@44>"),
                  HasSubstr("The market now says YES 62% · NO 38%. You hold "
                            "50 and have ")),
            "REPLY 5: **Tomorrow's markets**\n"
            "• <@44>: YES 62% · NO 38% · 50 shares · pot 25 gmbux, on 1 "
            "guess",
            AllOf(StartsWith("REPLY 6: <@45>, you have **"),
                  HasSubstr("\n• <@44> tomorrow: 50 YES · guessed 07:45")),
            AllOf(StartsWith("REPLY 7: **The rich list**\n1. **ada**: "),
                  HasSubstr(" gmbux"))));
  }());
}

// Each server is a market of its own, and there is none in a direct
// message.
TEST(BotTest, HasNoMarketOutsideAServerItServes) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());

    const Result result = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .guild = 0,
                .command = "portfolio",
            }),
            // "gm" in a server whose GM channel the bot was not given.
            Dispatch(Said{
                .sequence = 3,
                .channel = 62,
                .guild = 63,
                .content = "gm",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(result.actions,
                ElementsAre("REPLY 2: I only work in a server with a GM "
                            "channel, sorry."));
  }());
}

// A command Discord still offers from an earlier version of the bot gets an
// answer rather than silence.
TEST(BotTest, AnswersACommandItNoLongerHas) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());

    const Result result = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "short",
            }),
        },
        exchange, record, InChannel22());

    EXPECT_THAT(result.actions,
                ElementsAre("REPLY 2: I don't know that command any more."));
  }());
}

// On starting, the bot offers its commands in the server of each GM
// channel: once per server, even where a server has two.
TEST(BotTest, OffersItsCommandsInEachServer) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    bot::Config config = InChannel22();
    config.channels.push_back(discord::ChannelId{
        .value = 23,
    });
    config.channels.push_back(discord::ChannelId{
        .value = 52,
    });

    auto http = std::make_unique<http::FakeClient>();
    const http::FakeClient& sent = *http;
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"url":"wss://gateway.test"})",
    });
    // Channel 22: asked about, then the commands offered and withdrawn
    // everywhere, which get the fake's own answer.
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"id":"22","guild_id":"33"})",
    });
    http->Answer(http::Response{
        .status = 204,
    });
    http->Answer(http::Response{
        .status = 204,
    });
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"id":"23","guild_id":"33"})",
    });
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"id":"52","guild_id":"53"})",
    });

    websocket::FakeServer gateway;
    gateway.Sends(R"({"op":10,"d":{"heartbeat_interval":3600000}})");
    gateway.Sends(R"({"op":0,"s":1,"t":"READY","d":{"session_id":"abc",
        "resume_gateway_url":"wss://resume.test",
        "user":{"id":"99","username":"gm_market","bot":true},
        "application":{"id":"77"}}})");
    gateway.Closes(4004);

    absl::StatusOr<discord::Client> client = co_await discord::Client::Connect(
        "secret", std::move(http), gateway.connector());
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;

    EXPECT_THAT(co_await bot::Serve(*client, exchange, record, config),
                StatusIs(absl::StatusCode::kUnauthenticated));

    std::vector<std::string> offered;
    for (const http::Request& request : sent.requests()) {
      if (request.url.find("/guilds/") == std::string::npos) continue;

      offered.push_back(request.url.substr(std::string_view(kApi).size()));
      for (const char* const command : {"market", "markets", "bet", "sell",
                                        "guess", "portfolio", "richlist"}) {
        EXPECT_THAT(request.body,
                    HasSubstr(absl::StrCat(R"("name":")", command, "\"")));
      }
    }
    EXPECT_THAT(offered, ElementsAre("/applications/77/guilds/33/commands",
                                     "/applications/77/guilds/53/commands"));
  }());
}

// A GM channel the bot cannot see stops it at the start, saying which.
TEST(BotTest, StopsIfItCannotSeeAGmChannel) {
  RunOnEventLoop([]() -> Task<> {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());

    const Result result =
        co_await ServeEvents({}, exchange, record, InChannel22(),
                             {
                                 http::Response{
                                     .status = 404,
                                     .body = R"({"message":"Unknown Channel"})",
                                 },
                             });

    EXPECT_THAT(result.stopped, StatusIs(absl::StatusCode::kNotFound,
                                         HasSubstr("GM channel 22")));
  }());
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:bot_test -- --benchmark_filter=all
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
//   --------------------------------------------------------------------------
//   Benchmark                Time             CPU   Iterations UserCounters...
//   --------------------------------------------------------------------------
//   BM_ServeBets          6.52 ms         6.52 ms           63 items_per_second=39.2466k/s
//   BM_ServeGms           2.04 ms         2.03 ms          207 items_per_second=125.814k/s
//   BM_ServeChatter      0.939 ms        0.939 ms          441 items_per_second=272.609k/s
// End of results.
// clang-format on

constexpr int kEvents = 256;

Task<> ServeBets(benchmark::State& state) {
  for (auto _ : state) {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    std::vector<std::string> dispatches;
    dispatches.reserve(kEvents);
    for (int i = 0; i < kEvents; ++i) {
      dispatches.push_back(Dispatch(Used{
          .sequence = i + 2,
          .author = 1000 + i,
          .command = "bet",
          .options =
              {
                  Text("member", "<@44>"),
                  Text("outcome", i % 2 == 0 ? "yes" : "no"),
                  Integer("gmbux", 5),
              },
      }));
    }
    benchmark::DoNotOptimize(co_await ServeEvents(
        std::move(dispatches), exchange, record, InChannel22()));
  }
  state.SetItemsProcessed(state.iterations() * kEvents);
}

// The whole bot taking an evening's bets, everything but the network:
// parsing each command, the exchange (in memory), and composing the answer.
void BM_ServeBets(benchmark::State& state) {
  EventLoop::Create()->Run(ServeBets(state));
}
BENCHMARK(BM_ServeBets)->Unit(benchmark::kMillisecond);

Task<> ServeGms(benchmark::State& state) {
  for (auto _ : state) {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    std::vector<std::string> dispatches;
    dispatches.reserve(kEvents);
    for (int i = 0; i < kEvents; ++i) {
      dispatches.push_back(Dispatch(Said{
          .sequence = i + 2,
          .author = 1000 + i,
          .content = "gm everyone",
      }));
    }
    benchmark::DoNotOptimize(co_await ServeEvents(
        std::move(dispatches), exchange, record, InChannel22()));
  }
  state.SetItemsProcessed(state.iterations() * kEvents);
}

// The whole bot hearing a morning's GMs from people nobody bet on, which is
// most of what it hears: for each, asking whether a day is over, reading the
// phrases, and finding no market to settle.
void BM_ServeGms(benchmark::State& state) {
  EventLoop::Create()->Run(ServeGms(state));
}
BENCHMARK(BM_ServeGms)->Unit(benchmark::kMillisecond);

Task<> ServeChatter(benchmark::State& state) {
  for (auto _ : state) {
    market::Exchange exchange = NewExchange();
    gm::Record record = RecordOf(NewLedger());
    std::vector<std::string> dispatches;
    dispatches.reserve(kEvents);
    for (int i = 0; i < kEvents; ++i) {
      dispatches.push_back(Dispatch(Said{
          .sequence = i + 2,
          .channel = 23,
          .author = 1000 + i,
          .content = "did anyone watch the game",
      }));
    }
    benchmark::DoNotOptimize(co_await ServeEvents(
        std::move(dispatches), exchange, record, InChannel22()));
  }
  state.SetItemsProcessed(state.iterations() * kEvents);
}

// The whole bot hearing talk in the server's other channels, which it is
// told of too: all it does is ask whether a day is over.
void BM_ServeChatter(benchmark::State& state) {
  EventLoop::Create()->Run(ServeChatter(state));
}
BENCHMARK(BM_ServeChatter)->Unit(benchmark::kMillisecond);

}  // namespace
