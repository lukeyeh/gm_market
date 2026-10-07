// The phrases that count as saying GM, and whether a message says one. This
// is gm_bot's rule, kept in step with it by hand: a message that gm_bot
// records as a GM is one that SaysGm here, given the same phrases.

#ifndef GM_PHRASE_H_
#define GM_PHRASE_H_

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace gm {

// A phrase that counts as a GM, and during which hours of the day.
struct Phrase {
  // Trimmed and in lower case, as gm_bot keeps it.
  std::string text;

  // It counts from the start of hour `opens` to the start of hour `closes`,
  // on a 24-hour clock, running past midnight if `closes` is not the later.
  // The same hour twice, as here, is all day.
  int opens = 0;
  int closes = 0;

  friend bool operator==(const Phrase&, const Phrase&) = default;
};

// The phrases of a server that has not changed them.
inline constexpr std::array<std::string_view, 3> kDefaultPhrases = {
    "gm",
    "good morning",
    "morning",
};

// Whether `message`, written during `hour` (0 to 23), is a GM given the
// `phrases` there are: it says one of them at an hour when that one counts.
// A message says a phrase in any capitalisation, with the phrase on its own
// or at the start or end of something longer ("gm everyone!", "ok, good
// morning"), and as words of its own ("gmail" does not say "gm").
bool SaysGm(std::string_view message, std::span<const Phrase> phrases,
            int hour);

}  // namespace gm

#endif  // GM_PHRASE_H_
