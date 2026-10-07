// For tests: a ledger laid out as gm_bot lays out its own, to be filled in
// with what gm_bot would have written and then read through gm::Record.

#ifndef GM_FAKE_LEDGER_H_
#define GM_FAKE_LEDGER_H_

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"
#include "sqlite/database.h"

namespace gm {

class FakeLedger {
 public:
  // An empty ledger in memory, or in `file`, which is created.
  static absl::StatusOr<FakeLedger> InMemory();
  static absl::StatusOr<FakeLedger> Create(const std::filesystem::path& file);

  // Makes `phrase` count in `community` from hour `opens` to hour `closes`,
  // as /gmadd does. gm_bot has then met the community, so it no longer has
  // the default phrases.
  absl::Status AddPhrase(uint64_t community, std::string_view phrase,
                         int opens = 0, int closes = 0);

  // Has gm_bot meet `community` without leaving it a phrase, which is how a
  // server that has removed every one of them looks.
  absl::Status Meet(uint64_t community);

  // Records a GM from `member` of `community` on `day`.
  absl::Status RecordGm(uint64_t community, uint64_t member,
                        absl::CivilDay day);

  // Gives the ledger up, to be read: gm::Record::Over(std::move(fake).Take()).
  sqlite::Database Take() && { return std::move(database_); }

 private:
  explicit FakeLedger(sqlite::Database database)
      : database_(std::move(database)) {}

  static absl::StatusOr<FakeLedger> LaidOut(sqlite::Database database);

  sqlite::Database database_;
};

}  // namespace gm

#endif  // GM_FAKE_LEDGER_H_
