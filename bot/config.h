// What the bot needs to be told before it can run, and where it is told:
// environment variables, so that the token never appears on a command line.

#ifndef BOT_CONFIG_H_
#define BOT_CONFIG_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "discord/model.h"

namespace bot {

struct Config {
  // The bot's Discord token.
  std::string token;

  // The GM channels: where gm_bot counts GMs, and so where this bot watches
  // for them and announces how markets ended. At least one. Each server
  // they are in has its own markets and balances.
  std::vector<discord::ChannelId> channels;

  // Whose midnight divides one day from the next. It has to be gm_bot's, or
  // the two will disagree about which day a GM was said on.
  absl::TimeZone time_zone;

  // The file the markets are kept in.
  std::filesystem::path database;

  // gm_bot's ledger, which says what counts as a GM. Only ever read.
  std::filesystem::path ledger;
};

// Looks up a variable by name; nothing if it is not set.
using Variables =
    std::function<std::optional<std::string>(const std::string& name)>;

// The variables of the process's environment.
std::optional<std::string> Environment(const std::string& name);

// Reads the configuration from `variables`:
//
//   DISCORD_TOKEN   required
//   GM_CHANNEL_IDS  required; the ids of the GM channels, as Discord's "Copy
//                   Channel ID" gives them, separated by commas
//   GM_DATABASE     required; the gm.db that gm_bot keeps
//   TIMEZONE        optional; a name such as "America/New_York". UTC if unset
//   DATA_DIR        optional; the directory to keep predictions.db in. The
//                   current directory if unset
//
// Fails with InvalidArgument, naming the variable, if one is missing or
// makes no sense.
absl::StatusOr<Config> LoadConfig(const Variables& variables = &Environment);

}  // namespace bot

#endif  // BOT_CONFIG_H_
