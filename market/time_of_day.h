// A time of day to the minute, without a date: what someone guesses a GM
// will be said at, and what it turns out to have been said at.

#ifndef MARKET_TIME_OF_DAY_H_
#define MARKET_TIME_OF_DAY_H_

#include <compare>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "absl/time/civil_time.h"

namespace market {

class TimeOfDay {
 public:
  // Midnight, at the start of the day.
  constexpr TimeOfDay() = default;

  // The time of day that `moment` is at.
  explicit TimeOfDay(absl::CivilMinute moment)
      : minute_(moment.hour() * 60 + moment.minute()) {}

  // The time `minute` minutes after midnight, which is how one is stored.
  // Anything outside the day is brought to its nearer end.
  static TimeOfDay AfterMidnight(int minute);

  // A time as a person writes one: "7:45", "07:45", "7", "7:45pm", "7 PM".
  // Without am or pm it is on the 24-hour clock. Fails with InvalidArgument,
  // in words fit to show whoever wrote it, if `text` is none of those.
  static absl::StatusOr<TimeOfDay> Parse(std::string_view text);

  // Minutes after midnight, 0 to 1439.
  constexpr int minute() const { return minute_; }

  // How many minutes apart the two are, within the one day: 23:59 and 0:01
  // are nearly a day apart, not two minutes.
  constexpr int MinutesFrom(TimeOfDay other) const {
    return minute_ < other.minute_ ? other.minute_ - minute_
                                   : minute_ - other.minute_;
  }

  // As shown to people, on the 24-hour clock: "07:45".
  std::string Describe() const;

  friend constexpr auto operator<=>(TimeOfDay, TimeOfDay) = default;

 private:
  int minute_ = 0;
};

}  // namespace market

#endif  // MARKET_TIME_OF_DAY_H_
