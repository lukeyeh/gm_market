#include "gm/record.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "gm/phrase.h"
#include "sqlite/database.h"

namespace gm {
namespace {

// The layout of gm_bot's ledger that the queries below are written for,
// which the file records as SQLite's user_version. See gm/ledger.cc in
// gm_bot, which numbers them. Of layout 5 this reads:
//
//   communities (id)                          the servers given their phrases
//   phrases (community, phrase, opens, closes)
//   gms (community, member_id, life, day)     day as an ISO date
constexpr int64_t kLayout = 5;

}  // namespace

absl::StatusOr<Record> Record::Open(const std::filesystem::path& file) {
  // Opening a file that is not there would create it, and leave gm_bot an
  // empty ledger it did not make.
  if (!std::filesystem::exists(file)) {
    return absl::NotFoundError(
        absl::StrCat("there is no ledger at ", file.string()));
  }

  ABSL_ASSIGN_OR_RETURN(sqlite::Database ledger, sqlite::Database::Open(file));

  return Over(std::move(ledger));
}

absl::StatusOr<Record> Record::Over(sqlite::Database ledger) {
  ABSL_ASSIGN_OR_RETURN(const std::vector<sqlite::Row> version,
                        ledger.Query("PRAGMA user_version"));

  const int64_t layout = version.empty() ? 0 : version.front().Int(0);
  if (layout != kLayout) {
    return absl::FailedPreconditionError(
        absl::StrCat("the ledger has layout ", layout, ", and this reads ",
                     kLayout, ": run a matching gm_bot on it first"));
  }

  return Record(std::move(ledger));
}

absl::StatusOr<std::vector<Phrase>> Record::Phrases(const uint64_t community) {
  const int64_t id = static_cast<int64_t>(community);

  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      ledger_.Query("SELECT phrase, opens, closes FROM phrases "
                    "WHERE community = ?1 ORDER BY phrase",
                    {
                        id,
                    }));

  std::vector<Phrase> phrases;
  phrases.reserve(rows.size());
  for (const sqlite::Row& row : rows) {
    phrases.push_back(Phrase{
        .text = std::string(row.Text(0)),
        .opens = static_cast<int>(row.Int(1)),
        .closes = static_cast<int>(row.Int(2)),
    });
  }
  if (!phrases.empty()) return phrases;

  // None, which is either because the server removed every one or because
  // gm_bot has yet to meet it.
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> known,
      ledger_.Query("SELECT 1 FROM communities WHERE id = ?1", {
                                                                   id,
                                                               }));
  if (!known.empty()) return phrases;

  for (const std::string_view text : kDefaultPhrases) {
    phrases.push_back(Phrase{
        .text = std::string(text),
    });
  }

  return phrases;
}

absl::StatusOr<bool> Record::SaidGm(const uint64_t community,
                                    const uint64_t member,
                                    const absl::CivilDay day) {
  ABSL_ASSIGN_OR_RETURN(
      const std::vector<sqlite::Row> rows,
      ledger_.Query("SELECT 1 FROM gms "
                    "WHERE community = ?1 AND member_id = ?2 AND day = ?3 "
                    "LIMIT 1",
                    {
                        static_cast<int64_t>(community),
                        static_cast<int64_t>(member),
                        absl::FormatCivilTime(day),
                    }));

  return !rows.empty();
}

}  // namespace gm
