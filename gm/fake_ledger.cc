#include "gm/fake_ledger.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "sqlite/database.h"

namespace gm {

absl::StatusOr<FakeLedger> FakeLedger::InMemory() {
  ABSL_ASSIGN_OR_RETURN(sqlite::Database database,
                        sqlite::Database::InMemory());

  return LaidOut(std::move(database));
}

absl::StatusOr<FakeLedger> FakeLedger::Create(
    const std::filesystem::path& file) {
  ABSL_ASSIGN_OR_RETURN(sqlite::Database database,
                        sqlite::Database::Open(file));

  return LaidOut(std::move(database));
}

// The tables of gm_bot's layout 5, as its gm/ledger.cc leaves them.
absl::StatusOr<FakeLedger> FakeLedger::LaidOut(sqlite::Database database) {
  for (const std::string_view statement : {
           "CREATE TABLE communities (id INTEGER PRIMARY KEY)",
           "CREATE TABLE members ("
           "  community INTEGER NOT NULL,"
           "  id INTEGER NOT NULL,"
           "  name TEXT NOT NULL,"
           "  life INTEGER NOT NULL DEFAULT 0,"
           "  PRIMARY KEY (community, id)) WITHOUT ROWID",
           "CREATE TABLE gms ("
           "  community INTEGER NOT NULL,"
           "  member_id INTEGER NOT NULL,"
           "  life INTEGER NOT NULL,"
           "  day TEXT NOT NULL,"
           "  PRIMARY KEY (community, member_id, life, day)) WITHOUT ROWID",
           "CREATE TABLE phrases ("
           "  community INTEGER NOT NULL,"
           "  phrase TEXT NOT NULL,"
           "  opens INTEGER NOT NULL DEFAULT 0,"
           "  closes INTEGER NOT NULL DEFAULT 0,"
           "  PRIMARY KEY (community, phrase)) WITHOUT ROWID",
           "PRAGMA user_version = 5",
       }) {
    ABSL_RETURN_IF_ERROR(database.Execute(statement));
  }

  return FakeLedger(std::move(database));
}

absl::Status FakeLedger::AddPhrase(const uint64_t community,
                                   const std::string_view phrase,
                                   const int opens, const int closes) {
  ABSL_RETURN_IF_ERROR(Meet(community));

  return database_.Execute(
      "INSERT INTO phrases (community, phrase, opens, closes) "
      "VALUES (?1, ?2, ?3, ?4)",
      {
          static_cast<int64_t>(community),
          std::string(phrase),
          int64_t{opens},
          int64_t{closes},
      });
}

absl::Status FakeLedger::Meet(const uint64_t community) {
  return database_.Execute(
      "INSERT INTO communities (id) VALUES (?1) ON CONFLICT DO NOTHING",
      {
          static_cast<int64_t>(community),
      });
}

absl::Status FakeLedger::RecordGm(const uint64_t community,
                                  const uint64_t member,
                                  const absl::CivilDay day) {
  return database_.Execute(
      "INSERT INTO gms (community, member_id, life, day) "
      "VALUES (?1, ?2, 0, ?3)",
      {
          static_cast<int64_t>(community),
          static_cast<int64_t>(member),
          absl::FormatCivilTime(day),
      });
}

}  // namespace gm
