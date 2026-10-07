// What gm_bot has on record, read from the ledger file it keeps: the phrases
// that count as a GM in each server, and who said GM on which day.
//
// The file is gm_bot's. Nothing here changes it, and it may change under
// this at any time: each question is answered from the file as it is then.

#ifndef GM_RECORD_H_
#define GM_RECORD_H_

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "gm/phrase.h"
#include "sqlite/database.h"

namespace gm {

class Record {
 public:
  // Opens the ledger gm_bot keeps in `file`. Fails with NotFound if there is
  // no such file, and with FailedPrecondition if it is not a ledger laid out
  // the way this code knows: gm_bot has not yet run on it, or is a version
  // this bot is too old or too new for.
  static absl::StatusOr<Record> Open(const std::filesystem::path& file);

  // The same over a database that is already open: see fake_ledger.h.
  static absl::StatusOr<Record> Over(sqlite::Database ledger);

  // The phrases that count as a GM in the server with this id, in
  // alphabetical order. A server gm_bot has not met yet has kDefaultPhrases,
  // which is what gm_bot will give it.
  absl::StatusOr<std::vector<Phrase>> Phrases(uint64_t community);

  // Whether gm_bot recorded a GM on `day` from the member with this id of
  // that server.
  absl::StatusOr<bool> SaidGm(uint64_t community, uint64_t member,
                              absl::CivilDay day);

 private:
  explicit Record(sqlite::Database ledger) : ledger_(std::move(ledger)) {}

  sqlite::Database ledger_;
};

}  // namespace gm

#endif  // GM_RECORD_H_
