#include "bot/arguments.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/strip.h"
#include "discord/model.h"
#include "market/lmsr.h"

namespace bot {

absl::StatusOr<discord::UserId> ParseMember(const std::string_view text) {
  std::string_view digits = absl::StripAsciiWhitespace(text);

  // A mention is the id between "<@" and ">". Older clients put a "!" after
  // the "@" for someone with a nickname.
  if (absl::ConsumePrefix(&digits, "<@")) {
    absl::ConsumePrefix(&digits, "!");
    if (!absl::ConsumeSuffix(&digits, ">")) digits = "";
  }

  uint64_t id = 0;
  if (!absl::SimpleAtoi(digits, &id) || id == 0) {
    return absl::InvalidArgumentError(
        "Say who by mentioning them, like `@luke`.");
  }

  return discord::UserId{
      .value = id,
  };
}

absl::StatusOr<market::Outcome> ParseOutcome(const std::string_view text) {
  const std::string folded =
      absl::AsciiStrToLower(absl::StripAsciiWhitespace(text));

  if (folded == "yes" || folded == "y") return market::Outcome::kYes;
  if (folded == "no" || folded == "n") return market::Outcome::kNo;

  return absl::InvalidArgumentError("The outcome is either `yes` or `no`.");
}

}  // namespace bot
