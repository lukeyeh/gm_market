#include "perf/load.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "bot/bot.h"
#include "bot/config.h"
#include "discord/client.h"
#include "discord/model.h"
#include "gm/fake_ledger.h"
#include "gm/phrase.h"
#include "gm/record.h"
#include "http/client.h"
#include "market/exchange.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"
#include "net/event_loop.h"
#include "net/stream.h"
#include "websocket/websocket.h"

namespace {

using Clock = std::chrono::steady_clock;

// The GM channel, another channel of the same server, and the server.
constexpr uint64_t kGmChannel = 22;
constexpr uint64_t kOtherChannel = 23;
constexpr uint64_t kGuild = 33;

// How many people are in each market the exchange is set up with.
constexpr int kInEachMarket = 5;

int64_t NowNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

// When each event was sent, by its number. Written by the server's thread
// and read by the bot's.
class SendTimes {
 public:
  explicit SendTimes(size_t events)
      : times_(std::make_unique<std::atomic<int64_t>[]>(events)) {}

  void MarkSent(size_t event) {
    times_[event].store(NowNanoseconds(), std::memory_order_relaxed);
  }

  std::chrono::nanoseconds Since(size_t event) const {
    return std::chrono::nanoseconds(
        NowNanoseconds() - times_[event].load(std::memory_order_relaxed));
  }

 private:
  std::unique_ptr<std::atomic<int64_t>[]> times_;
};

// A stream that passes everything to another it does not own, so that the
// server can have websocket::Accept do the handshake and then write frames
// to the stream itself.
class View final : public net::Stream {
 public:
  explicit View(net::Stream* stream) : stream_(stream) {}

  Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                    net::Deadline deadline) override {
    co_return co_await stream_->Read(buffer, deadline);
  }

  Task<absl::Status> Write(std::string_view data) override {
    co_return co_await stream_->Write(data);
  }

 private:
  net::Stream* stream_;
};

// A WebSocket frame as a server sends it: unmasked, in one piece. `opcode`
// is 1 for text and 8 for close.
std::string Frame(uint8_t opcode, std::string_view payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | opcode));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else {
    frame.push_back(126);
    frame.push_back(static_cast<char>(payload.size() >> 8));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
  }
  frame.append(payload);
  return frame;
}

// The gateway's side of the connection: greets it, sends `events` at the
// pace asked for, noting when each goes, and then closes the connection the
// way Discord does to a bot whose token has been revoked, which is what
// makes the bot stop.
Task<> PlayGateway(const net::Listener& listener,
                   const std::vector<std::string>& preamble,
                   const std::vector<std::string>& events,
                   std::chrono::microseconds interval, SendTimes& sent) {
  const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
      co_await listener.Accept();
  if (!stream.ok()) co_return;
  if (!(co_await websocket::Accept(std::make_unique<View>(stream->get())))
           .ok()) {
    co_return;
  }

  for (const std::string& message : preamble) {
    if (!(co_await (*stream)->Write(Frame(1, message))).ok()) co_return;
  }

  for (size_t i = 0; i < events.size(); ++i) {
    if (interval.count() > 0) co_await Sleep(interval);
    sent.MarkSent(i);
    if (!(co_await (*stream)->Write(Frame(1, events[i]))).ok()) co_return;
  }

  // Close code 4004, "authentication failed".
  (co_await (*stream)->Write(Frame(8, std::string_view("\x0F\xA4", 2))))
      .IgnoreError();

  // Stay until the client has hung up, so that nothing sent is cut short.
  std::string unread(4096, '\0');
  for (;;) {
    const absl::StatusOr<size_t> count =
        co_await (*stream)->Read(unread, net::After(std::chrono::seconds(30)));
    if (!count.ok() || *count == 0) co_return;
  }
}

// Runs PlayGateway on a thread with an event loop of its own, as a server
// elsewhere would be.
class GatewayThread {
 public:
  GatewayThread(const net::Listener& listener,
                std::vector<std::string> preamble,
                std::vector<std::string> events,
                std::chrono::microseconds interval, SendTimes& sent)
      : preamble_(std::move(preamble)),
        events_(std::move(events)),
        thread_([this, &listener, interval, &sent] {
          absl::StatusOr<EventLoop> loop = EventLoop::Create();
          if (!loop.ok()) return;
          loop->Run(PlayGateway(listener, preamble_, events_, interval, sent));
        }) {}

 private:
  std::vector<std::string> preamble_;
  std::vector<std::string> events_;
  std::jthread thread_;
};

// A message event numbered `sequence`, of the size Discord really sends.
std::string MessageEvent(int64_t sequence, uint64_t author, uint64_t channel,
                         std::string_view content) {
  return absl::StrCat(
      R"({"t":"MESSAGE_CREATE","s":)", sequence,
      R"(,"op":0,"d":{"type":0,"tts":false,)"
      R"("timestamp":"2026-10-05T12:00:00.000000+00:00","pinned":false,)"
      R"("mentions":[],"mention_roles":[],"mention_everyone":false,)"
      R"("member":{"roles":["500000000000000001","500000000000000002"],)"
      R"("premium_since":null,"pending":false,"nick":null,"mute":false,)"
      R"("joined_at":"2021-12-29T20:14:27.585000+00:00","flags":0,)"
      R"("deaf":false,"communication_disabled_until":null,"banner":null,)"
      R"("avatar":null},"id":")",
      sequence,
      R"(","flags":0,"embeds":[],"edited_timestamp":null,"content":")", content,
      R"(","components":[],"channel_type":0,"channel_id":")", channel,
      R"(","author":{"username":"member)", author,
      R"(","public_flags":0,"primary_guild":null,"id":")", author,
      R"(","global_name":null,"discriminator":"0","collectibles":null,)"
      R"("clan":null,"avatar_decoration_data":null,)"
      R"("avatar":"8e1f9d0c2b3a4d5e6f708192a3b4c5d6"},"attachments":[],)"
      R"("guild_id":")",
      kGuild, R"("}})");
}

// An option of a command, as Discord sends one: of text, or a whole number.
std::string Text(std::string_view name, std::string_view value) {
  return absl::StrCat(R"({"name":")", name, R"(","type":3,"value":")", value,
                      R"("})");
}

std::string Number(std::string_view name, int64_t value) {
  return absl::StrCat(R"({"name":")", name, R"(","type":4,"value":)", value,
                      "}");
}

std::string Mention(uint64_t member) { return absl::StrCat("<@", member, ">"); }

// Someone using the command called `name` with `options`, which are
// separated by commas.
std::string CommandEvent(int64_t sequence, uint64_t author,
                         std::string_view name, std::string_view options = "") {
  return absl::StrCat(R"({"t":"INTERACTION_CREATE","s":)", sequence,
                      R"(,"op":0,"d":{"type":2,"id":")", sequence,
                      R"(","token":"token-)", sequence, R"(","channel_id":")",
                      kOtherChannel, R"(","guild_id":")", kGuild,
                      R"(","data":{"name":")", name, R"(","options":[)",
                      options, R"(]},"member":{"user":{"id":")", author,
                      R"(","username":"member)", author, R"("}}}})");
}

// The events of `options.scene`: the same ones every time. The last is a
// command, so that its answer shows that everything before it was dealt
// with.
std::vector<std::string> Events(const LoadOptions& options) {
  std::mt19937 random(20261006);
  std::uniform_int_distribution<uint64_t> member(1, options.members);
  std::uniform_int_distribution<int> kind(0, 99);
  std::uniform_int_distribution<int> stake(1, 20);
  std::uniform_int_distribution<int> hour(5, 11);
  std::uniform_int_distribution<int> minute(0, 59);

  std::vector<std::string> events;
  events.reserve(options.events);
  for (int i = 0; i < options.events; ++i) {
    // Sequence numbers start at 2, READY being 1.
    const int64_t sequence = i + 2;
    const uint64_t who = member(random);
    const std::string about = Mention(member(random));
    const std::string_view side = kind(random) < 50 ? "yes" : "no";
    const int roll = kind(random);

    if (i == options.events - 1) {
      events.push_back(CommandEvent(sequence, who, "portfolio"));
      continue;
    }

    // Either way, a quarter of what the bot hears is talk elsewhere in the
    // server.
    if (roll < 25) {
      events.push_back(MessageEvent(sequence, who, kOtherChannel,
                                    "did anyone watch the game"));
      continue;
    }

    if (options.scene == Scene::kMorning) {
      if (roll < 65) {
        events.push_back(MessageEvent(sequence, who, kGmChannel, "gm"));
      } else if (roll < 88) {
        events.push_back(MessageEvent(sequence, who, kGmChannel,
                                      "Good morning everyone, coffee time"));
      } else if (roll < 95) {
        events.push_back(CommandEvent(sequence, who, "portfolio"));
      } else {
        events.push_back(
            CommandEvent(sequence, who, "market", Text("member", about)));
      }
      continue;
    }

    if (roll < 65) {
      events.push_back(CommandEvent(
          sequence, who, "bet",
          absl::StrCat(Text("member", about), ",", Text("outcome", side), ",",
                       Number("gmbux", stake(random)))));
    } else if (roll < 75) {
      events.push_back(CommandEvent(
          sequence, who, "guess",
          absl::StrCat(Text("member", about), ",",
                       Text("time", absl::StrCat(hour(random), ":",
                                                 10 + minute(random) % 50)))));
    } else if (roll < 80) {
      events.push_back(CommandEvent(
          sequence, who, "sell",
          absl::StrCat(Text("member", about), ",", Text("outcome", side))));
    } else if (roll < 90) {
      events.push_back(
          CommandEvent(sequence, who, "market", Text("member", about)));
    } else if (roll < 93) {
      events.push_back(CommandEvent(sequence, who, "markets"));
    } else if (roll < 98) {
      events.push_back(CommandEvent(sequence, who, "portfolio"));
    } else {
      events.push_back(CommandEvent(sequence, who, "richlist"));
    }
  }
  return events;
}

// What begins every gateway session.
std::vector<std::string> SessionStart() {
  return {
      R"({"op":10,"d":{"heartbeat_interval":3600000}})",
      R"({"op":0,"s":1,"t":"READY","d":{"session_id":"load",)"
      R"("resume_gateway_url":"ws://127.0.0.1:1",)"
      R"("user":{"id":"99","username":"gm_market","bot":true},)"
      R"("application":{"id":"77"}}})",
  };
}

// Discord's HTTP API, as far as the bot uses it, answered from memory. It
// notes how long each event waited for the call that answers it.
class AnsweringApi final : public http::Client {
 public:
  AnsweringApi(std::string gateway_url, const SendTimes* sent,
               LoadResult* result)
      : gateway_url_(std::move(gateway_url)), sent_(sent), result_(result) {}

  Task<absl::StatusOr<http::Response>> Send(
      const http::Request& request) override {
    const std::string_view url = request.url;

    if (absl::EndsWith(url, "/gateway/bot")) {
      co_return http::Response{
          .status = 200,
          .body = absl::StrCat(R"({"url":")", gateway_url_, R"("})"),
      };
    }
    if (request.method == http::Method::kGet) {
      co_return http::Response{
          .status = 200,
          .body = absl::StrCat(R"({"guild_id":")", kGuild, R"("})"),
      };
    }

    NoteAnswered(url);
    co_return http::Response{
        .status = 204,
    };
  }

 private:
  // If `url` is the reply to a command, which names the command's event in
  // its path, counts that event as answered and every event up to it as
  // dealt with.
  void NoteAnswered(std::string_view url) {
    constexpr std::string_view kMarker = "/interactions/";
    const size_t at = url.find(kMarker);
    if (at == std::string_view::npos) return;

    const std::string_view rest = url.substr(at + kMarker.size());
    int64_t sequence = 0;
    if (!absl::SimpleAtoi(rest.substr(0, rest.find('/')), &sequence)) return;

    const size_t event = static_cast<size_t>(sequence - 2);
    result_->latency.Record(sent_->Since(event));
    result_->elapsed = sent_->Since(0);
    result_->handled = event + 1;
  }

  std::string gateway_url_;
  const SendTimes* sent_;
  LoadResult* result_;
};

Task<absl::Status> ServeUntilTurnedAway(std::string url, bot::Config config,
                                        market::Exchange& exchange,
                                        gm::Record& record,
                                        const SendTimes& sent,
                                        LoadResult& result) {
  CO_ASSIGN_OR_RETURN(
      discord::Client client,
      co_await discord::Client::Connect(
          config.token, std::make_unique<AnsweringApi>(url, &sent, &result),
          &websocket::Connect));

  // The server ends the test by closing as Discord does on a bad token.
  const absl::Status stopped =
      co_await bot::Serve(client, exchange, record, config);
  if (absl::IsUnauthenticated(stopped)) co_return absl::OkStatus();
  co_return stopped;
}

market::Trader Member(int number) {
  return market::Trader{
      .id = static_cast<uint64_t>(number),
      .name = absl::StrCat("member", number),
  };
}

// Gives every member a market for the day after `traded`, with a few of
// the others in each, holding a little on alternate sides and a guess.
absl::Status OpenMarkets(market::Community community, int members,
                         absl::CivilDay traded) {
  for (int subject = 1; subject <= members; ++subject) {
    for (int i = 1; i <= kInEachMarket && i < members; ++i) {
      const market::Trader trader = Member((subject + i - 1) % members + 1);

      ABSL_RETURN_IF_ERROR(
          community
              .Buy(trader, static_cast<uint64_t>(subject),
                   i % 2 == 0 ? market::Outcome::kYes : market::Outcome::kNo,
                   market::Gmbux::Whole(1), traded)
              .status());
      ABSL_RETURN_IF_ERROR(
          community
              .Guess(trader, static_cast<uint64_t>(subject),
                     market::TimeOfDay::AfterMidnight(7 * 60 + 10 * i), traded)
              .status());
    }
  }
  return absl::OkStatus();
}

// Sets the exchange in `file` up for `options.scene`: the days of settled
// markets asked for, none of which saw a GM, and for a morning, the markets
// that were traded yesterday and are waiting on today.
absl::Status Prepare(const std::filesystem::path& file,
                     const LoadOptions& options) {
  ABSL_ASSIGN_OR_RETURN(market::Exchange exchange,
                        market::Exchange::Open(file));
  market::Community community = exchange.community(kGuild);
  const absl::CivilDay today =
      absl::ToCivilDay(absl::Now(), absl::UTCTimeZone());

  // The newest of them is for the day before yesterday, which leaves
  // yesterday and today to the scene.
  for (int back = options.history_days; back >= 1; --back) {
    const absl::CivilDay day = today - 1 - back;
    ABSL_RETURN_IF_ERROR(OpenMarkets(community, options.members, day - 1));
    ABSL_RETURN_IF_ERROR(
        community
            .Expire(day + 1,
                    [](uint64_t, absl::CivilDay) -> absl::StatusOr<bool> {
                      return false;
                    })
            .status());
  }

  if (options.scene == Scene::kMorning) {
    ABSL_RETURN_IF_ERROR(OpenMarkets(community, options.members, today - 1));
  }
  return absl::OkStatus();
}

// A ledger as gm_bot would have left it in `file`, with the usual phrases.
absl::Status WriteLedger(const std::filesystem::path& file) {
  ABSL_ASSIGN_OR_RETURN(gm::FakeLedger ledger, gm::FakeLedger::Create(file));
  for (const std::string_view phrase : gm::kDefaultPhrases) {
    ABSL_RETURN_IF_ERROR(ledger.AddPhrase(kGuild, phrase));
  }
  return absl::OkStatus();
}

// Runs the bot against a gateway that sends `events`, on this thread, until
// the gateway has sent everything.
absl::StatusOr<LoadResult> Run(const LoadOptions& options,
                               const std::filesystem::path& directory,
                               std::vector<std::string> events) {
  ABSL_RETURN_IF_ERROR(WriteLedger(directory / "gm.db"));
  ABSL_RETURN_IF_ERROR(Prepare(directory / "predictions.db", options));

  ABSL_ASSIGN_OR_RETURN(market::Exchange exchange,
                        market::Exchange::Open(directory / "predictions.db"));
  ABSL_ASSIGN_OR_RETURN(gm::Record record,
                        gm::Record::Open(directory / "gm.db"));
  ABSL_ASSIGN_OR_RETURN(const net::Listener listener,
                        net::Listener::OnLoopback());
  ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());

  SendTimes sent(events.size());
  LoadResult result;
  {
    const GatewayThread server(listener, SessionStart(), std::move(events),
                               options.interval, sent);
    ABSL_RETURN_IF_ERROR(loop.Run(ServeUntilTurnedAway(
        absl::StrCat("ws://127.0.0.1:", listener.address().port),
        bot::Config{
            .token = "load-test",
            .channels =
                {
                    discord::ChannelId{
                        .value = kGmChannel,
                    },
                },
            .time_zone = absl::UTCTimeZone(),
        },
        exchange, record, sent, result)));
  }
  return result;
}

}  // namespace

absl::StatusOr<LoadResult> RunBotLoad(const LoadOptions& options) {
  // On disk, as the bot's files are in production.
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      absl::StrCat("gm_market_load_", NowNanoseconds());
  std::filesystem::create_directories(directory);

  absl::StatusOr<LoadResult> result = Run(options, directory, Events(options));

  std::filesystem::remove_all(directory);
  return result;
}
