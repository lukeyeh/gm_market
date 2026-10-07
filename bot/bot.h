// The prediction bot. It runs a market, in each server it serves, on who
// will say GM tomorrow and when:
//
//  - Its slash commands trade in those markets: /bet and /sell deal in
//    shares of whether a member will say GM, /guess stakes on the time, and
//    /market, /markets, /portfolio and /richlist show how things stand.
//  - It watches the GM channels alongside gm_bot. A GM there settles the
//    market on whoever said it, and the bot announces who was paid.
//  - A day that ends without a GM settles the other way. That is noticed
//    the next time anything happens in the server, not at midnight.
//
// What counts as a GM is gm_bot's to say: the phrases are read from its
// ledger. Each server has its own markets and balances.

#ifndef BOT_BOT_H_
#define BOT_BOT_H_

#include "absl/status/status.h"
#include "async/task.h"
#include "bot/config.h"
#include "discord/client.h"
#include "gm/record.h"
#include "market/exchange.h"

namespace bot {

// Runs the bot as configured, on the calling thread, until it cannot go on.
// Never returns OK: the status says why it stopped, which is either that it
// could not start (the exchange or gm_bot's ledger cannot be opened, Discord
// cannot be reached or does not accept the token) or that Discord has since
// turned it away.
absl::Status Run(const Config& config);

// The bot's work, given its connection to Discord, its exchange and what
// gm_bot has on record: registers its commands in each GM channel's server,
// then handles events from `client` until Discord turns the bot away, and
// evaluates to why.
//
// Once under way, nothing short of that stops it. A message or a command
// that cannot be handled is logged and passed over, since the next may fare
// better. Failing to register the commands at the start does stop it, which
// is also how a GM channel the bot cannot see comes to light.
Task<absl::Status> Serve(discord::Client& client, market::Exchange& exchange,
                         gm::Record& record, const Config& config);

}  // namespace bot

#endif  // BOT_BOT_H_
