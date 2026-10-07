// Load tests: how the whole bot keeps up when events arrive faster than one
// at a time.
//
// A stand-in for Discord's gateway runs on a thread of its own and sends
// events over a real (loopback) WebSocket as a server would. The exchange
// and gm_bot's ledger are files, as in production; only Discord's HTTP API
// is absent, answered from memory. Unlike the benchmarks in the _test.cc
// files, which time one operation in isolation, this includes the kernel,
// the event loop, the disk, and the queueing that happens when events come
// in bursts.

#ifndef PERF_LOAD_H_
#define PERF_LOAD_H_

#include <chrono>
#include <cstdint>

#include "absl/status/statusor.h"
#include "perf/histogram.h"

// What the events sent are like.
enum class Scene {
  // People trading in tomorrow's markets: mostly /bet, with /guess, /sell
  // and the commands that only look, and the talk in other channels that
  // the bot is told of too.
  kEvening,

  // The next day: people saying GM, each of whom has a market that settles
  // and is announced the first time they do, among the same other talk and
  // a few commands.
  kMorning,
};

struct LoadOptions {
  Scene scene = Scene::kEvening;

  // How many events to send.
  int events = 2000;

  // How long the server waits between one event and the next. Zero sends
  // them as fast as the connection takes them, which measures throughput;
  // anything longer measures how long each waits at that rate.
  std::chrono::microseconds interval{0};

  // How many different people the events come from, each of whom is also
  // someone there can be a market on.
  int members = 200;

  // How many days of settled markets the exchange holds when the test
  // starts, one a day on every member with a few people in each. More
  // means more for the exchange to keep out of the way.
  int history_days = 30;
};

struct LoadResult {
  // How many events were dealt with: all of them, unless something broke.
  uint64_t handled = 0;

  // From the first event being sent to the last being dealt with.
  std::chrono::nanoseconds elapsed{0};

  // For each event the bot answers, from its being sent to its being
  // answered. An event that needs no answer, which is most messages, has
  // no moment to time: it is dealt with by the time the next answer goes
  // out, since events are handled in order.
  LatencyHistogram latency;

  double events_per_second() const {
    return elapsed.count() == 0 ? 0
                                : static_cast<double>(handled) * 1e9 /
                                      static_cast<double>(elapsed.count());
  }
};

// Sends the events of `options.scene` to the whole bot and measures how it
// keeps up. An event is answered when the bot makes the API call that
// replies to it.
absl::StatusOr<LoadResult> RunBotLoad(const LoadOptions& options);

#endif  // PERF_LOAD_H_
