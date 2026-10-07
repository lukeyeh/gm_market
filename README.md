# gm_market

A Discord bot that runs a prediction market on who will say GM tomorrow, and
when. It is a companion to [gm_bot](https://github.com/lukeyeh/gm_bot_cc),
which tracks the GM streaks themselves, and like it is built on
[bedrock](https://github.com/lukeyeh/bedrock).

The currency is **gmbux**. They are not money and cannot be bought: everyone
starts with 1000 and wins or loses them by predicting.

## What it does

For every member and every day there is a market asking one question: will
they say GM that day? It trades the day before, and closes at midnight.

| Command | What it does |
| --- | --- |
| `/market <member>` | The odds on that member saying GM tomorrow, the shares held, and the time pot. |
| `/markets` | Every market open for tomorrow, busiest first. |
| `/bet <member> <yes\|no> <gmbux>` | Buys as many YES or NO shares as that many gmbux cover. |
| `/sell <member> <yes\|no> [shares]` | Sells shares back at the going price. All of them unless told how many. |
| `/guess <member> <time>` | Stakes 25 gmbux on when they will say it: `7:45`, `19:30`, `7:45pm`. |
| `/portfolio` | Your gmbux, and what you have riding. |
| `/richlist [limit]` | Who has the most gmbux. |

Name the member by mentioning them (`@luke`).

**Shares.** A YES share pays 1 gmbux if the member says GM and nothing if
they do not; a NO share the reverse. Shares are bought from, and sold back
to, an automated market maker (Hanson's
[LMSR](https://mason.gmu.edu/~rhanson/mktscore.pdf)), so there is always
someone to trade with. A market starts at 50%. Buying YES pushes the price of
YES up and buying NO pushes it down, so the price reads as the server's
estimate of the chance, and a large bet gets worse odds than a small one. The
maker can lose gmbux to whoever is right first, which is where new gmbux come
from, but never more than about 70 on one market.

**The time pot.** Each guess stakes the same 25 gmbux. When the member says
GM, the guess nearest the minute they said it takes the pot; guesses equally
near share it. A second guess replaces your first for free. If they do not
say GM, everyone gets their stake back.

**Settling.** A market is settled once, as soon as its answer is known:

- The member says GM in the GM channel. The bot announces it there, with who
  was paid and who took the pot.
- The day ends without one. NO pays. Nothing runs at midnight to do this:
  the bot settles what is over the next time anything happens in the server.

**Betting on yourself.** You may buy YES on yourself, which is a bet that you
will show up. You may not buy NO on yourself or guess your own time, since
both are yours to decide.

Each server has its own markets and balances.

## How fast it is

Measured on disk, with a month of settled markets behind it, a stand-in
Discord sending events over a real WebSocket:

| | |
| --- | --- |
| An evening of trading (two thirds `/bet`, `/guess` and `/sell`) | about 18,000 events a second |
| A morning of GMs, each settling a market the first time | about 45,000 events a second |
| A command answered, at a steady 200 events a second | 0.3 ms typically, 1.4 ms at the 99th percentile |
| A bet, in the exchange alone | 3 µs |
| Hearing a message that is nobody's GM | under 4 µs |

What keeps it there as the exchange fills up: settled markets pile up for
ever, and nothing the bot does on the way to answering looks through them.
The open markets have an index to themselves, a day's markets sit together
in the table, and whether any market's day is over is asked once a day
rather than on every message. Before that was so, with a season of history,
the check made on every message took 340 µs and `/portfolio` 4.5 ms; they
now take 0.7 µs and 0.1 ms.

## How it stays in step with gm_bot

What counts as a GM is gm_bot's to say, and the two must agree or this bot
would pay out on the difference. So:

- **Phrases come from gm_bot's ledger.** The bot reads the phrase list, and
  each phrase's hours, from `gm.db` every time someone speaks in a GM
  channel. A phrase added with gm_bot's `/gmadd` counts here from that
  moment. The file is only ever read.
- **A GM this bot did not see still counts.** If the bot was down when
  someone said GM, the market would otherwise settle as NO. Before settling
  a day that is over, it asks gm_bot's ledger whether a GM was recorded. If
  so the market settles as YES; the time is unknown, so the pot goes back.
- **The matching rule is copied**, in `gm/phrase.cc`, with gm_bot's own test
  cases. If gm_bot's rule changes, that file has to follow.
- **`TIMEZONE` must be the same** for both bots.

The bot refuses to start on a ledger whose layout it does not know (it reads
layout 5), rather than misread it.

## Architecture

```
main.cc
  bot/          the bot: configuration, commands, what it says, and the loop
  │             joining the two below to Discord
  ├─ market/    the market: gmbux, prices, and the exchange that keeps who
  │  │          holds what
  │  └─ bedrock sqlite
  ├─ gm/        what gm_bot knows: its phrases, and who said GM on a day
  │  └─ bedrock sqlite
  └─ bedrock discord, net, async
  perf/         load tests
```

Dependencies point downwards only. `market/` knows nothing of Discord or of
how a GM is recognised; `gm/` knows nothing of markets.

- `market/lmsr` is the only place that knows how shares are priced.
- `market/exchange` is the only place with SQL for `predictions.db`. Every
  trade is one transaction. Trades name `today` and always land in
  tomorrow's market, so a caller cannot trade in the wrong one.
- `gm/record` is the only place that knows the layout of gm_bot's ledger.
- Amounts are whole hundredths of a gmbux (`market::Gmbux`), never floating
  point, so balances do not drift. Costs round up and proceeds round down, so
  buying and selling back is never a profit.

## Running it

This is a second Discord application, with its own token. Create it in the
[Discord Developer Portal](https://discord.com/developers/applications):
under **Bot**, enable the **Message Content Intent** and copy the token; under
**OAuth2 → URL Generator**, pick the `bot` and `applications.commands` scopes
with the *View Channels* and *Send Messages* permissions, and invite it to
the servers gm_bot is in.

```
cp .env.example .env         # then fill it in
nix develop -c sh -c 'set -a; . ./.env; set +a; bazel run //:gm_market'
```

| Variable | Meaning |
| --- | --- |
| `DISCORD_TOKEN` | This bot's token. Required. |
| `GM_CHANNEL_IDS` | The GM channels, separated by commas: the same list as gm_bot's. Required. |
| `GM_DATABASE` | The path of gm_bot's `gm.db`. Required. Run gm_bot once first so that it exists. |
| `TIMEZONE` | Whose midnight separates one day from the next. Must be gm_bot's. UTC if unset. |
| `DATA_DIR` | Where to keep `predictions.db`. The current directory if unset. |

With direnv, `direnv allow` once and then `bazel run //:gm_market`. `bazel
run` starts the program inside Bazel's output tree, so give `DATA_DIR` and
`GM_DATABASE` as absolute paths. Bazel must be run inside the Nix shell.

## Tests

18 test targets. Every `foo.h` / `foo.cc` has a `foo_test.cc` beside it,
written to be read: each opens by saying what it demonstrates, and each test
says what behaviour it shows.

- **Nothing reaches the network or gm_bot's real ledger.** The bot is
  tested against `http::FakeClient` and `websocket::FakeServer`, and `gm/`
  against `gm::FakeLedger`, a database laid out as gm_bot lays out its own.
- **Both I/O backends.** `bazel test --config=epoll //...` runs everything
  on epoll instead of io_uring.
- **Fuzz tests against models**, using
  [FuzzTest](https://github.com/google/fuzztest).
  `market/exchange_fuzz_test.cc` gives the exchange and a deliberately
  simple model of it, kept in maps, the same arbitrary sequence of
  purchases, sales, guesses, GMs seen and unseen and days going by, across
  two communities, and requires them to agree on every answer.
  `market/lmsr_fuzz_test.cc` checks what must hold of prices whatever has
  been sold; `gm/phrase_fuzz_test.cc` checks what counts as a GM against a
  model of gm_bot's rule; two more read whatever anyone might type as a time
  or into a command. They have found three bugs so far: shares that cost
  nothing in a very lopsided market, a rich list with a negative limit
  listing everyone, and a portfolio ordered differently from every other
  list. Each has a regression test where it was fixed.
- **Benchmarks beside the tests.** They live at the bottom of the
  `_test.cc` file for the code they measure and run from the same binary.
  Under each file's "Benchmarks" heading are the results of the last
  recorded run, with the date and the machine.
  `perf/benchmarks.sh recorded && perf/record_results.py recorded` refreshes
  them.
- **Load tests.** `perf/` runs the whole bot against a gateway on another
  thread, over loopback, with the exchange and the ledger on disk: an
  evening of trading and a morning of GMs, each flat out and at a steady
  rate.

## Everyday commands

| To | Run |
| --- | --- |
| Run all tests | `bazel test //...` |
| Run them on the epoll backend | `bazel test --config=epoll //...` |
| Run every benchmark | `perf/benchmarks.sh <label>`, which saves to `perf-reports/raw/<label>/` |
| Run one test file's benchmarks | `bazel run -c opt //market:exchange_test -- --benchmark_filter=all` |
| Run the load tests | `bazel run -c opt //perf:bench` |
| Fuzz the exchange for a minute | `bazel run --config=fuzz //market:exchange_fuzz_test -- --fuzz=ExchangeFuzzTest.AgreesWithTheModel --fuzz_for=60s` |
| Generate `compile_commands.json` for clangd | `bazel run :compile_commands` |
| Format | `clang-format -i main.cc */*.cc */*.h` and `buildifier -r .` |
| Lint | `clang-tidy main.cc */*.cc` (after generating compile commands) |
| Build the deployable package | `nix build`, giving `result/bin/gm-market` |

Libraries come through Nix at the versions `flake.lock` pins, exactly as in
gm_bot: `nix/deps.nix` is the one list, used by both Bazel and `nix build`.
FuzzTest, which nixpkgs does not package, is built by `nix/fuzztest.nix`.

## Not done yet

- Members are named in a text option, since bedrock's Discord client has no
  user-picker option type yet.
- Nobody earns gmbux except by trading, so someone who loses everything is
  out.
- Markets open at 50% for everyone, whatever the member's streak.
- `/portfolio` and `/markets` cost a little for every market open, and
  asking gm_bot's ledger whether someone said GM on a day goes through that
  member's whole history there (60 µs at a year of GMs). Both are far below
  anything a person would notice, and the second is asked once per market
  per day.
