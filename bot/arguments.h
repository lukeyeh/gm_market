// What people type into the bot's commands, turned into what they meant.
//
// Every failure here is InvalidArgument, with a message that is a sentence
// fit to show to whoever typed it.

#ifndef BOT_ARGUMENTS_H_
#define BOT_ARGUMENTS_H_

#include <string_view>

#include "absl/status/statusor.h"
#include "discord/model.h"
#include "market/lmsr.h"

namespace bot {

// The member that `text` names: a mention, which is what Discord makes of
// "@luke" typed into a command ("<@44>"), or the bare id.
absl::StatusOr<discord::UserId> ParseMember(std::string_view text);

// Which side `text` picks: "yes" or "no", in any capitalisation, or just
// the first letter.
absl::StatusOr<market::Outcome> ParseOutcome(std::string_view text);

}  // namespace bot

#endif  // BOT_ARGUMENTS_H_
