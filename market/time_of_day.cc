#include "market/time_of_day.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"

namespace market {
namespace {

constexpr int kMinutesInDay = 24 * 60;

absl::Status Unreadable() {
  return absl::InvalidArgumentError(
      "Give a time like `7:45`, `19:30` or `7:45pm`.");
}

}  // namespace

TimeOfDay TimeOfDay::AfterMidnight(const int minute) {
  TimeOfDay time;
  time.minute_ = std::clamp(minute, 0, kMinutesInDay - 1);
  return time;
}

absl::StatusOr<TimeOfDay> TimeOfDay::Parse(const std::string_view text) {
  const std::string folded =
      absl::AsciiStrToLower(absl::StripAsciiWhitespace(text));
  std::string_view clock = folded;

  // Which half of the day, if it says: then the hours are 1 to 12.
  const bool pm = absl::ConsumeSuffix(&clock, "pm");
  const bool am = !pm && absl::ConsumeSuffix(&clock, "am");
  clock = absl::StripAsciiWhitespace(clock);

  const std::pair<std::string_view, std::string_view> parts =
      absl::StrSplit(clock, absl::MaxSplits(':', 1));
  const bool has_minutes = clock.find(':') != std::string_view::npos;

  int hour = 0;
  int minute = 0;
  if (!absl::SimpleAtoi(parts.first, &hour)) return Unreadable();
  if (has_minutes &&
      (parts.second.size() != 2 || !absl::SimpleAtoi(parts.second, &minute))) {
    return Unreadable();
  }
  if (minute < 0 || minute > 59) return Unreadable();

  if (am || pm) {
    if (hour < 1 || hour > 12) return Unreadable();

    // Twelve is where each half starts: 12am is midnight and 12pm noon.
    hour = hour % 12 + (pm ? 12 : 0);
  } else if (hour < 0 || hour > 23) {
    return Unreadable();
  }

  return AfterMidnight(hour * 60 + minute);
}

std::string TimeOfDay::Describe() const {
  return absl::StrFormat("%02d:%02d", minute_ / 60, minute_ % 60);
}

}  // namespace market
