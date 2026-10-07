#include "market/exchange.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/civil_time.h"
#include "market/gmbux.h"
#include "market/lmsr.h"
#include "market/time_of_day.h"
#include "sqlite/database.h"

namespace market {
namespace {

// The layout of the database is numbered, and the number kept in the file
// (SQLite's user_version), so that a later version of this code can tell
// what it has been given and bring it up to date. An empty file says 0.
//
//   1  accounts, markets, positions and guesses, as created below.
constexpr int64_t kLayout = 1;

// The statements below are each written over several lines, as adjacent
// string literals that the compiler joins, in lists of statements. That is
// just what a forgotten comma between two list items looks like, which is
// what this check is for, so it is off for these lists.
// NOLINTBEGIN(bugprone-suspicious-missing-comma)

// Layout 1. Days are stored as ISO dates ("2026-10-05"), which compare as
// text the way they compare as dates; gmbux in hundredths; times of day in
// minutes after midnight.
//
// A market's `outcome` is NULL while it is open, then 1 for yes or 0 for no,
// and `said` the time of the GM if anyone saw it. Positions and guesses are
// kept after their market is settled, as the record of what was paid.
// Guesses have row ids, unlike the rest, because the order they were placed
// in decides who gets a pot's odd hundredths.
//
// The tables are laid out for the questions asked most:
//
//  - A day's markets are kept together, so listing tomorrow's reads one
//    stretch of the table.
//  - Settled markets pile up for ever, and what is over but not yet settled
//    has to be found among them, so the open ones have an index to
//    themselves. Queries that depend on it name it (INDEXED BY): left to
//    choose, SQLite goes through every market the community has had.
//  - A market's row says how many guesses it has and what they have staked,
//    which saves every quote from adding them up.
//  - Positions and guesses are kept by market, which is how they are
//    settled, and indexed by holder, which is how a portfolio finds them.
absl::Status CreateFirstLayout(sqlite::Database& database) {
  for (const std::string_view statement : {
           "CREATE TABLE accounts ("
           "  community INTEGER NOT NULL,"
           "  member INTEGER NOT NULL,"
           "  name TEXT NOT NULL,"
           "  balance INTEGER NOT NULL,"
           "  PRIMARY KEY (community, member)) WITHOUT ROWID",
           "CREATE TABLE markets ("
           "  community INTEGER NOT NULL,"
           "  subject INTEGER NOT NULL,"
           "  day TEXT NOT NULL,"
           "  yes INTEGER NOT NULL DEFAULT 0,"
           "  no INTEGER NOT NULL DEFAULT 0,"
           "  guesses INTEGER NOT NULL DEFAULT 0,"
           "  pot INTEGER NOT NULL DEFAULT 0,"
           "  outcome INTEGER,"
           "  said INTEGER,"
           "  PRIMARY KEY (community, day, subject)) WITHOUT ROWID",
           "CREATE INDEX open_markets ON markets (community, day, subject)"
           "  WHERE outcome IS NULL",
           "CREATE TABLE positions ("
           "  community INTEGER NOT NULL,"
           "  subject INTEGER NOT NULL,"
           "  day TEXT NOT NULL,"
           "  holder INTEGER NOT NULL,"
           "  yes INTEGER NOT NULL DEFAULT 0,"
           "  no INTEGER NOT NULL DEFAULT 0,"
           "  PRIMARY KEY (community, subject, day, holder)) WITHOUT ROWID",
           "CREATE INDEX positions_of_holder"
           "  ON positions (community, holder, day)",
           "CREATE TABLE guesses ("
           "  community INTEGER NOT NULL,"
           "  subject INTEGER NOT NULL,"
           "  day TEXT NOT NULL,"
           "  holder INTEGER NOT NULL,"
           "  minute INTEGER NOT NULL,"
           "  stake INTEGER NOT NULL,"
           "  UNIQUE (community, subject, day, holder))",
           "CREATE INDEX guesses_of_holder ON guesses (community, holder, day)",
       }) {
    ABSL_RETURN_IF_ERROR(database.Execute(statement));
  }
  return absl::OkStatus();
}

// NOLINTEND(bugprone-suspicious-missing-comma)

// `day` as the tables store it: an ISO date. Written out here rather than
// with absl::FormatCivilTime, which gives the same text several times more
// slowly, and this is done for every trade.
std::string StoredDay(const absl::CivilDay day) {
  return absl::StrFormat("%04d-%02d-%02d", day.year(), day.month(), day.day());
}

// Which market: the three things the tables know one by.
struct Key {
  int64_t community = 0;
  int64_t subject = 0;
  absl::CivilDay day;
  // The day as the tables store it.
  std::string date;
};

Key KeyOf(const int64_t community, const uint64_t subject,
          const absl::CivilDay day) {
  return Key{
      .community = community,
      .subject = static_cast<int64_t>(subject),
      .day = day,
      .date = StoredDay(day),
  };
}

// A market as the tables have it. One with no row is as good as one nobody
// has traded in, and reads as that.
struct State {
  Quote quote;
  bool settled = false;
};

Quote QuoteOf(const Key& key, const Shares sold, const int guesses,
              const Gmbux pot) {
  return Quote{
      .subject = static_cast<uint64_t>(key.subject),
      .day = key.day,
      .sold = sold,
      .chance = ChanceOfYes(sold),
      .guesses = guesses,
      .pot = pot,
  };
}

absl::StatusOr<State> ReadState(sqlite::Database& database, const Key& key) {
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      database.Query(
          "SELECT yes, no, guesses, pot, outcome IS NOT NULL FROM markets "
          "WHERE community = ?1 AND subject = ?2 AND day = ?3",
          {
              key.community,
              key.subject,
              key.date,
          }));
  if (rows.empty()) {
    return State{
        .quote = QuoteOf(key, Shares{}, 0, Gmbux()),
    };
  }

  const sqlite::Row& row = rows.front();
  return State{
      .quote =
          QuoteOf(key,
                  Shares{
                      .yes = row.Int(0),
                      .no = row.Int(1),
                  },
                  static_cast<int>(row.Int(2)), Gmbux::Hundredths(row.Int(3))),
      .settled = row.Int(4) != 0,
  };
}

// Records what the market `key` has now sold, making the market if this is
// the first anyone has done in it.
absl::Status WriteSold(sqlite::Database& database, const Key& key,
                       const Shares sold) {
  return database.Execute(
      "INSERT INTO markets (community, subject, day, yes, no) "
      "VALUES (?1, ?2, ?3, ?4, ?5) "
      "ON CONFLICT (community, subject, day) DO UPDATE SET"
      "  yes = excluded.yes, no = excluded.no",
      {
          key.community,
          key.subject,
          key.date,
          sold.yes,
          sold.no,
      });
}

absl::StatusOr<Shares> ReadHeld(sqlite::Database& database, const Key& key,
                                const int64_t holder) {
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      database.Query("SELECT yes, no FROM positions "
                     "WHERE community = ?1 AND subject = ?2 AND day = ?3"
                     "  AND holder = ?4",
                     {
                         key.community,
                         key.subject,
                         key.date,
                         holder,
                     }));
  if (rows.empty()) return Shares{};

  return Shares{
      .yes = rows.front().Int(0),
      .no = rows.front().Int(1),
  };
}

absl::Status WriteHeld(sqlite::Database& database, const Key& key,
                       const int64_t holder, const Shares held) {
  return database.Execute(
      "INSERT INTO positions (community, subject, day, holder, yes, no) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
      "ON CONFLICT (community, subject, day, holder) DO UPDATE SET"
      "  yes = excluded.yes, no = excluded.no",
      {
          key.community,
          key.subject,
          key.date,
          holder,
          held.yes,
          held.no,
      });
}

// Gives `trader` an account in `community` if they have none, with the
// starting balance, and has it go by the name they now have. Returns their
// balance.
absl::StatusOr<Gmbux> Enrol(sqlite::Database& database, const int64_t community,
                            const Trader& trader) {
  const int64_t member = static_cast<int64_t>(trader.id);

  // Nearly always they have one, by that name, and there is nothing to
  // write.
  ABSL_ASSIGN_OR_RETURN(const std::vector<sqlite::Row> known,
                        database.Query("SELECT balance, name FROM accounts "
                                       "WHERE community = ?1 AND member = ?2",
                                       {
                                           community,
                                           member,
                                       }));
  if (!known.empty() && known.front().Text(1) == trader.name) {
    return Gmbux::Hundredths(known.front().Int(0));
  }

  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      database.Query(
          "INSERT INTO accounts (community, member, name, balance) "
          "VALUES (?1, ?2, ?3, ?4) "
          "ON CONFLICT (community, member) DO UPDATE SET name = excluded.name "
          "RETURNING balance",
          {
              community,
              member,
              trader.name,
              kStartingBalance.hundredths(),
          }));

  return Gmbux::Hundredths(rows.empty() ? 0 : rows.front().Int(0));
}

// Adds `amount`, which may be negative, to the balance of `member`.
absl::Status Pay(sqlite::Database& database, const int64_t community,
                 const int64_t member, const Gmbux amount) {
  return database.Execute(
      "UPDATE accounts SET balance = balance + ?3 "
      "WHERE community = ?1 AND member = ?2",
      {
          community,
          member,
          amount.hundredths(),
      });
}

absl::Status AlreadySettled() {
  return absl::OutOfRangeError("That market has already been settled.");
}

std::string_view NameOf(const Outcome outcome) {
  return outcome == Outcome::kYes ? "YES" : "NO";
}

// Pays out the shares of the market `key`, which has ended in `outcome`, and
// fills in `settlement` with who got what.
absl::Status PayShares(sqlite::Database& database, const Key& key,
                       const Outcome outcome, Settlement& settlement) {
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> positions,
      database.Query("SELECT p.holder, a.name, p.yes, p.no "
                     "FROM positions p JOIN accounts a"
                     "  ON a.community = p.community AND a.member = p.holder "
                     "WHERE p.community = ?1 AND p.subject = ?2 AND p.day = ?3"
                     "  AND (p.yes > 0 OR p.no > 0) "
                     "ORDER BY p.holder",
                     {
                         key.community,
                         key.subject,
                         key.date,
                     }));

  settlement.holders = static_cast<int>(positions.size());
  for (const sqlite::Row& row : positions) {
    const int64_t winning = outcome == Outcome::kYes ? row.Int(2) : row.Int(3);
    if (winning == 0) continue;

    const Gmbux amount = Gmbux::Hundredths(winning * kSharePayout.hundredths());
    ABSL_RETURN_IF_ERROR(Pay(database, key.community, row.Int(0), amount));

    settlement.winnings.push_back(Payout{
        .trader =
            Trader{
                .id = static_cast<uint64_t>(row.Int(0)),
                .name = std::string(row.Text(1)),
            },
        .amount = amount,
    });
  }

  std::ranges::stable_sort(
      settlement.winnings,
      [](const Payout& a, const Payout& b) { return a.amount > b.amount; });
  return absl::OkStatus();
}

// Deals with the pot of the market `key`: to whoever guessed nearest to
// `said_at`, or back to everyone if there is no time to be near. Fills in
// `settlement` with where it went.
absl::Status PayPot(sqlite::Database& database, const Key& key,
                    const std::optional<TimeOfDay> said_at,
                    Settlement& settlement) {
  // In the order they were placed.
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> guesses,
      database.Query("SELECT g.holder, a.name, g.minute, g.stake "
                     "FROM guesses g JOIN accounts a"
                     "  ON a.community = g.community AND a.member = g.holder "
                     "WHERE g.community = ?1 AND g.subject = ?2 AND g.day = ?3 "
                     "ORDER BY g.rowid",
                     {
                         key.community,
                         key.subject,
                         key.date,
                     }));
  if (guesses.empty()) return absl::OkStatus();

  const auto guess_of = [](const sqlite::Row& row) {
    return TimeOfDay::AfterMidnight(static_cast<int>(row.Int(2)));
  };

  // How far off the nearest guess was, where there is something to be near.
  int nearest = std::numeric_limits<int>::max();
  for (const sqlite::Row& row : guesses) {
    settlement.pot += Gmbux::Hundredths(row.Int(3));
    if (said_at.has_value()) {
      nearest = std::min(nearest, guess_of(row).MinutesFrom(*said_at));
    }
  }

  // Who is paid: the nearest, or everyone.
  std::vector<const sqlite::Row*> paid;
  for (const sqlite::Row& row : guesses) {
    if (!said_at.has_value() ||
        guess_of(row).MinutesFrom(*said_at) == nearest) {
      paid.push_back(&row);
    }
  }

  settlement.pot_returned = !said_at.has_value();
  if (said_at.has_value()) settlement.nearest_guess = guess_of(*paid.front());

  // Winners who are level share the pot equally. It may not divide evenly:
  // the hundredths left over go one each to the earliest guesses.
  const int64_t winners = static_cast<int64_t>(paid.size());
  const int64_t share = settlement.pot.hundredths() / winners;
  const int64_t left_over = settlement.pot.hundredths() % winners;

  for (size_t i = 0; i < paid.size(); ++i) {
    const sqlite::Row& row = *paid[i];
    const Gmbux amount = Gmbux::Hundredths(
        settlement.pot_returned
            ? row.Int(3)
            : share + (static_cast<int64_t>(i) < left_over ? 1 : 0));
    ABSL_RETURN_IF_ERROR(Pay(database, key.community, row.Int(0), amount));

    settlement.pot_payouts.push_back(Payout{
        .trader =
            Trader{
                .id = static_cast<uint64_t>(row.Int(0)),
                .name = std::string(row.Text(1)),
            },
        .amount = amount,
    });
  }
  return absl::OkStatus();
}

// Settles the market `key` as `outcome`, the GM having been said at
// `said_at` if anyone knows. Returns nothing, having done nothing, if there
// is no such market still open. To be called within a transaction.
absl::StatusOr<std::optional<Settlement>> Settle(
    sqlite::Database& database, const Key& key, const Outcome outcome,
    const std::optional<TimeOfDay> said_at) {
  // RETURNING yields a row only if the market was there and open.
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> closed,
      database.Query(
          "UPDATE markets SET outcome = ?4, said = ?5 "
          "WHERE community = ?1 AND subject = ?2 AND day = ?3"
          "  AND outcome IS NULL "
          "RETURNING 1",
          {
              key.community,
              key.subject,
              key.date,
              int64_t{outcome == Outcome::kYes ? 1 : 0},
              said_at.has_value() ? sqlite::Value(int64_t{said_at->minute()})
                                  : sqlite::Value(),
          }));
  if (closed.empty()) return std::nullopt;

  Settlement settlement{
      .subject = static_cast<uint64_t>(key.subject),
      .day = key.day,
      .outcome = outcome,
      .said_at = said_at,
  };
  ABSL_RETURN_IF_ERROR(PayShares(database, key, outcome, settlement));
  ABSL_RETURN_IF_ERROR(PayPot(database, key, said_at, settlement));

  return settlement;
}

}  // namespace

bool IsRefusal(const absl::Status& status) {
  return absl::IsResourceExhausted(status) || absl::IsOutOfRange(status);
}

absl::StatusOr<Quote> Community::QuoteFor(const uint64_t subject,
                                          const absl::CivilDay today) {
  ABSL_ASSIGN_OR_RETURN(const State state,
                        ReadState(*database_, KeyOf(id_, subject, today + 1)));

  return state.quote;
}

absl::StatusOr<std::vector<Quote>> Community::Quotes(
    const absl::CivilDay today) {
  const absl::CivilDay day = today + 1;

  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      database_->Query("SELECT subject, yes, no, guesses, pot "
                       "FROM markets NOT INDEXED "
                       "WHERE community = ?1 AND day = ?2 AND outcome IS NULL "
                       "ORDER BY yes + no DESC, subject",
                       {
                           id_,
                           StoredDay(day),
                       }));

  std::vector<Quote> quotes;
  for (const sqlite::Row& row : rows) {
    const Shares sold = {
        .yes = row.Int(1),
        .no = row.Int(2),
    };
    const int guesses = static_cast<int>(row.Int(3));

    // Everyone who was in it has sold up again.
    if (sold == Shares{} && guesses == 0) continue;

    quotes.push_back(QuoteOf(KeyOf(id_, static_cast<uint64_t>(row.Int(0)), day),
                             sold, guesses, Gmbux::Hundredths(row.Int(4))));
  }

  return quotes;
}

absl::StatusOr<Fill> Community::Buy(const Trader& trader,
                                    const uint64_t subject,
                                    const Outcome outcome, const Gmbux budget,
                                    const absl::CivilDay today) {
  sqlite::Database& database = *database_;
  const Key key = KeyOf(id_, subject, today + 1);
  const int64_t holder = static_cast<int64_t>(trader.id);

  Fill fill;
  ABSL_RETURN_IF_ERROR(database.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(const Gmbux balance,
                          Enrol(database, key.community, trader));
    ABSL_ASSIGN_OR_RETURN(const State state, ReadState(database, key));

    if (state.settled) return AlreadySettled();
    if (trader.id == subject && outcome == Outcome::kNo) {
      return absl::OutOfRangeError(
          "You can back yourself to say GM, but you cannot bet against it.");
    }
    if (budget > balance) {
      return absl::ResourceExhaustedError(
          absl::StrCat("You only have ", balance.Describe(), " gmbux."));
    }

    const int64_t shares = SharesAffordable(state.quote.sold, outcome, budget);
    if (shares == 0) {
      return absl::OutOfRangeError("That is not enough to buy a single share.");
    }

    const Gmbux cost = CostOfBuying(state.quote.sold, outcome, shares);
    const Shares sold = state.quote.sold.With(outcome, shares);
    ABSL_ASSIGN_OR_RETURN(const Shares held_before,
                          ReadHeld(database, key, holder));
    const Shares held = held_before.With(outcome, shares);

    ABSL_RETURN_IF_ERROR(WriteSold(database, key, sold));
    ABSL_RETURN_IF_ERROR(WriteHeld(database, key, holder, held));
    ABSL_RETURN_IF_ERROR(Pay(database, key.community, holder, Gmbux() - cost));

    fill = Fill{
        .shares = shares,
        .amount = cost,
        .held = held.of(outcome),
        .balance = balance - cost,
        .quote = QuoteOf(key, sold, state.quote.guesses, state.quote.pot),
    };
    return absl::OkStatus();
  }));

  return fill;
}

absl::StatusOr<Fill> Community::Sell(const Trader& trader,
                                     const uint64_t subject,
                                     const Outcome outcome,
                                     const std::optional<int64_t> shares,
                                     const absl::CivilDay today) {
  sqlite::Database& database = *database_;
  const Key key = KeyOf(id_, subject, today + 1);
  const int64_t holder = static_cast<int64_t>(trader.id);

  Fill fill;
  ABSL_RETURN_IF_ERROR(database.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(const Gmbux balance,
                          Enrol(database, key.community, trader));
    ABSL_ASSIGN_OR_RETURN(const State state, ReadState(database, key));
    ABSL_ASSIGN_OR_RETURN(const Shares held_before,
                          ReadHeld(database, key, holder));

    if (state.settled) return AlreadySettled();

    const int64_t holding = held_before.of(outcome);
    if (holding == 0) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "You hold no ", NameOf(outcome), " shares in that market."));
    }

    const int64_t selling = shares.value_or(holding);
    if (selling < 1) {
      return absl::OutOfRangeError("Sell at least one share.");
    }
    if (selling > holding) {
      return absl::ResourceExhaustedError(
          absl::StrCat("You only hold ", holding, " ", NameOf(outcome),
                       " shares in that market."));
    }

    const Gmbux proceeds =
        ProceedsOfSelling(state.quote.sold, outcome, selling);
    const Shares sold = state.quote.sold.With(outcome, -selling);
    const Shares held = held_before.With(outcome, -selling);

    ABSL_RETURN_IF_ERROR(WriteSold(database, key, sold));
    ABSL_RETURN_IF_ERROR(WriteHeld(database, key, holder, held));
    ABSL_RETURN_IF_ERROR(Pay(database, key.community, holder, proceeds));

    fill = Fill{
        .shares = selling,
        .amount = proceeds,
        .held = held.of(outcome),
        .balance = balance + proceeds,
        .quote = QuoteOf(key, sold, state.quote.guesses, state.quote.pot),
    };
    return absl::OkStatus();
  }));

  return fill;
}

absl::StatusOr<Ticket> Community::Guess(const Trader& trader,
                                        const uint64_t subject,
                                        const TimeOfDay time,
                                        const absl::CivilDay today) {
  sqlite::Database& database = *database_;
  const Key key = KeyOf(id_, subject, today + 1);
  const int64_t holder = static_cast<int64_t>(trader.id);

  Ticket ticket;
  ABSL_RETURN_IF_ERROR(database.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(const Gmbux balance,
                          Enrol(database, key.community, trader));
    ABSL_ASSIGN_OR_RETURN(const State state, ReadState(database, key));

    if (state.settled) return AlreadySettled();
    if (trader.id == subject) {
      return absl::OutOfRangeError(
          "You cannot guess when you will say GM yourself: it is yours to "
          "choose.");
    }

    // A second guess takes the place of the first, whose stake stands.
    ABSL_ASSIGN_OR_RETURN(
        const std::vector<sqlite::Row> replaced,
        database.Query("UPDATE guesses SET minute = ?5 "
                       "WHERE community = ?1 AND subject = ?2 AND day = ?3"
                       "  AND holder = ?4 "
                       "RETURNING 1",
                       {
                           key.community,
                           key.subject,
                           key.date,
                           holder,
                           int64_t{time.minute()},
                       }));
    if (!replaced.empty()) {
      ticket = Ticket{
          .balance = balance,
          .quote = state.quote,
      };
      return absl::OkStatus();
    }

    if (kGuessStake > balance) {
      return absl::ResourceExhaustedError(
          absl::StrCat("A guess costs ", kGuessStake.Describe(),
                       " gmbux, and you only have ", balance.Describe(), "."));
    }

    // The market's row counts the guess, and has to be there for the guess
    // to be settled with it.
    ABSL_RETURN_IF_ERROR(database.Execute(
        "INSERT INTO markets (community, subject, day, guesses, pot) "
        "VALUES (?1, ?2, ?3, 1, ?4) "
        "ON CONFLICT (community, subject, day) DO UPDATE SET"
        "  guesses = guesses + 1, pot = pot + excluded.pot",
        {
            key.community,
            key.subject,
            key.date,
            kGuessStake.hundredths(),
        }));
    ABSL_RETURN_IF_ERROR(database.Execute(
        "INSERT INTO guesses (community, subject, day, holder, minute, stake) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        {
            key.community,
            key.subject,
            key.date,
            holder,
            int64_t{time.minute()},
            kGuessStake.hundredths(),
        }));
    ABSL_RETURN_IF_ERROR(
        Pay(database, key.community, holder, Gmbux() - kGuessStake));

    ticket = Ticket{
        .staked = kGuessStake,
        .balance = balance - kGuessStake,
        .quote = QuoteOf(key, state.quote.sold, state.quote.guesses + 1,
                         state.quote.pot + kGuessStake),
    };
    return absl::OkStatus();
  }));

  return ticket;
}

absl::StatusOr<Portfolio> Community::PortfolioOf(const Trader& trader) {
  const int64_t holder = static_cast<int64_t>(trader.id);

  ABSL_ASSIGN_OR_RETURN(const Gmbux balance, Enrol(*database_, id_, trader));

  // Both questions go through what the holder has from the earliest open
  // market's day onwards, which is little, and keep what is in a market
  // still open: not through everything they have ever held, nor through
  // every open market. CROSS JOIN is how SQLite is told to take the tables
  // in the order written.
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> positions,
      database_->Query(
          "SELECT p.day, p.subject, p.yes, p.no "
          "FROM positions p INDEXED BY positions_of_holder"
          "  CROSS JOIN markets m"
          "  ON m.community = p.community AND m.day = p.day"
          "  AND m.subject = p.subject "
          "WHERE p.community = ?1 AND p.holder = ?2"
          "  AND p.day >= (SELECT MIN(day) FROM markets INDEXED BY open_markets"
          "                WHERE community = ?1 AND outcome IS NULL)"
          "  AND m.outcome IS NULL AND (p.yes > 0 OR p.no > 0)",
          {
              id_,
              holder,
          }));
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> guesses,
      database_->Query(
          "SELECT g.day, g.subject, g.minute "
          "FROM guesses g INDEXED BY guesses_of_holder"
          "  CROSS JOIN markets m"
          "  ON m.community = g.community AND m.day = g.day"
          "  AND m.subject = g.subject "
          "WHERE g.community = ?1 AND g.holder = ?2"
          "  AND g.day >= (SELECT MIN(day) FROM markets INDEXED BY open_markets"
          "                WHERE community = ?1 AND outcome IS NULL)"
          "  AND m.outcome IS NULL",
          {
              id_,
              holder,
          }));

  // By market, in order of day and then of subject. Someone may hold shares
  // in a market, a guess in it, or both.
  // Subjects are ordered as the database has them, as everywhere else.
  std::map<std::pair<absl::CivilDay, int64_t>, Holding> by_market;
  const auto holding_of = [&by_market](const sqlite::Row& row) -> Holding& {
    absl::CivilDay day;
    absl::ParseCivilTime(row.Text(0), &day);

    Holding& holding = by_market[{day, row.Int(1)}];
    holding.subject = static_cast<uint64_t>(row.Int(1));
    holding.day = day;
    return holding;
  };

  for (const sqlite::Row& row : positions) {
    holding_of(row).shares = Shares{
        .yes = row.Int(2),
        .no = row.Int(3),
    };
  }
  for (const sqlite::Row& row : guesses) {
    holding_of(row).guess =
        TimeOfDay::AfterMidnight(static_cast<int>(row.Int(2)));
  }

  Portfolio portfolio{
      .balance = balance,
  };
  portfolio.holdings.reserve(by_market.size());
  for (const auto& [market, holding] : by_market) {
    portfolio.holdings.push_back(holding);
  }

  return portfolio;
}

absl::StatusOr<std::vector<Account>> Community::Richest(const int limit) {
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      database_->Query("SELECT member, name, balance FROM accounts "
                       "WHERE community = ?1 "
                       "ORDER BY balance DESC, name, member LIMIT ?2",
                       {
                           id_,
                           // To SQLite a negative limit is no limit at all.
                           int64_t{std::max(limit, 0)},
                       }));

  std::vector<Account> accounts;
  accounts.reserve(rows.size());
  for (const sqlite::Row& row : rows) {
    accounts.push_back(Account{
        .trader =
            Trader{
                .id = static_cast<uint64_t>(row.Int(0)),
                .name = std::string(row.Text(1)),
            },
        .balance = Gmbux::Hundredths(row.Int(2)),
    });
  }

  return accounts;
}

absl::StatusOr<std::optional<Settlement>> Community::Said(
    const uint64_t subject, const absl::CivilDay day, const TimeOfDay time) {
  sqlite::Database& database = *database_;
  const Key key = KeyOf(id_, subject, day);

  std::optional<Settlement> settlement;
  ABSL_RETURN_IF_ERROR(database.Transaction([&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(settlement,
                          Settle(database, key, Outcome::kYes, time));
    return absl::OkStatus();
  }));

  return settlement;
}

absl::StatusOr<std::vector<Settlement>> Community::Expire(
    const absl::CivilDay today,
    const absl::FunctionRef<absl::StatusOr<bool>(uint64_t subject,
                                                 absl::CivilDay day)>
        said_gm) {
  sqlite::Database& database = *database_;

  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> due,
      database.Query("SELECT subject, day "
                     "FROM markets INDEXED BY open_markets "
                     "WHERE community = ?1 AND outcome IS NULL AND day < ?2 "
                     "ORDER BY day, subject",
                     {
                         id_,
                         StoredDay(today),
                     }));

  std::vector<Settlement> settlements;
  for (const sqlite::Row& row : due) {
    const uint64_t subject = static_cast<uint64_t>(row.Int(0));
    absl::CivilDay day;
    absl::ParseCivilTime(row.Text(1), &day);

    ABSL_ASSIGN_OR_RETURN(const bool said, said_gm(subject, day));

    const Key key = KeyOf(id_, subject, day);
    std::optional<Settlement> settlement;
    ABSL_RETURN_IF_ERROR(database.Transaction([&]() -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(
          settlement, Settle(database, key, said ? Outcome::kYes : Outcome::kNo,
                             std::nullopt));
      return absl::OkStatus();
    }));
    if (settlement.has_value()) settlements.push_back(std::move(*settlement));
  }

  return settlements;
}

absl::StatusOr<Exchange> Exchange::Open(const std::filesystem::path& file) {
  ABSL_ASSIGN_OR_RETURN(sqlite::Database database,
                        sqlite::Database::Open(file));

  return Prepare(std::move(database));
}

absl::StatusOr<Exchange> Exchange::InMemory() {
  ABSL_ASSIGN_OR_RETURN(sqlite::Database database,
                        sqlite::Database::InMemory());

  return Prepare(std::move(database));
}

absl::StatusOr<Exchange> Exchange::Prepare(sqlite::Database database) {
  ABSL_ASSIGN_OR_RETURN(const std::vector<sqlite::Row> version,
                        database.Query("PRAGMA user_version"));

  const int64_t layout = version.empty() ? 0 : version.front().Int(0);
  if (layout > kLayout) {
    return absl::FailedPreconditionError(
        absl::StrCat("the exchange has layout ", layout,
                     ", and this version only knows up to ", kLayout));
  }

  if (layout == 0) {
    ABSL_RETURN_IF_ERROR(database.Transaction([&database]() -> absl::Status {
      ABSL_RETURN_IF_ERROR(CreateFirstLayout(database));

      return database.Execute(absl::StrCat("PRAGMA user_version = ", kLayout));
    }));
  }

  return Exchange(std::move(database));
}

}  // namespace market
