#include "gm/phrase.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

#include "absl/strings/ascii.h"
#include "absl/strings/strip.h"

namespace gm {
namespace {

bool IsWordBreak(char c) { return !absl::ascii_isalnum(c); }

// Whether `phrase` counts during `hour`.
bool CountsAt(const Phrase& phrase, const int hour) {
  if (phrase.opens < phrase.closes) {
    return phrase.opens <= hour && hour < phrase.closes;
  }

  // Past midnight, or all day.
  return hour >= phrase.opens || hour < phrase.closes;
}

// Whether `text`, already trimmed and in lower case, says `phrase`: at its
// start or its end, as words of its own.
bool Says(const std::string_view text, const std::string_view phrase) {
  std::string_view after = text;
  if (absl::ConsumePrefix(&after, phrase) &&
      (after.empty() || IsWordBreak(after.front()))) {
    return true;
  }

  std::string_view before = text;
  return absl::ConsumeSuffix(&before, phrase) &&
         (before.empty() || IsWordBreak(before.back()));
}

}  // namespace

bool SaysGm(const std::string_view message,
            const std::span<const Phrase> phrases, const int hour) {
  const std::string text =
      absl::AsciiStrToLower(absl::StripAsciiWhitespace(message));

  return std::ranges::any_of(phrases, [&text, hour](const Phrase& phrase) {
    return CountsAt(phrase, hour) && Says(text, phrase.text);
  });
}

}  // namespace gm
