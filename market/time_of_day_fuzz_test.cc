// Times of day, checked on whatever anyone might type: reading one never
// goes wrong in a way other than saying so, and every minute of the day
// survives being written out and read back, however it is written.
//
//   bazel test //market:time_of_day_fuzz_test

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "market/time_of_day.h"

namespace {

using market::TimeOfDay;

// Any text is either a time within the day or refused as InvalidArgument.
// What is read reads back the same from how it is shown.
void ReadsOrRefusesAnyText(const std::string& text) {
  const absl::StatusOr<TimeOfDay> time = TimeOfDay::Parse(text);
  if (!time.ok()) {
    ASSERT_TRUE(absl::IsInvalidArgument(time.status())) << time.status();
    return;
  }

  ASSERT_GE(time->minute(), 0);
  ASSERT_LT(time->minute(), 24 * 60);

  const absl::StatusOr<TimeOfDay> again = TimeOfDay::Parse(time->Describe());
  ASSERT_TRUE(again.ok()) << again.status();
  ASSERT_EQ(*again, *time);
}

// Arbitrary bytes, and things that are nearly times.
FUZZ_TEST(TimeOfDayFuzzTest, ReadsOrRefusesAnyText)
    .WithDomains(fuzztest::OneOf(
        fuzztest::Arbitrary<std::string>(),
        fuzztest::InRegexp("[ ]?-?[0-9]{0,3}(:[0-9]{0,3})?[ ]?([aApP][mM])?")));

// Every minute of the day reads back from each way of writing it: on the
// 24-hour clock with and without a leading zero, and with am or pm.
void EveryMinuteReadsBack(int hour, int minute) {
  const TimeOfDay time = TimeOfDay::AfterMidnight(hour * 60 + minute);
  const int twelve_hour = hour % 12 == 0 ? 12 : hour % 12;
  const char* const half = hour < 12 ? "am" : "PM";

  for (const std::string& written : {
           absl::StrFormat("%d:%02d", hour, minute),
           absl::StrFormat("%02d:%02d", hour, minute),
           absl::StrFormat("%d:%02d%s", twelve_hour, minute, half),
           absl::StrFormat(" %d:%02d %s ", twelve_hour, minute, half),
       }) {
    const absl::StatusOr<TimeOfDay> read = TimeOfDay::Parse(written);
    ASSERT_TRUE(read.ok()) << written << ": " << read.status();
    ASSERT_EQ(*read, time) << written;
  }

  ASSERT_EQ(time.Describe(), absl::StrFormat("%02d:%02d", hour, minute));
}

FUZZ_TEST(TimeOfDayFuzzTest, EveryMinuteReadsBack)
    .WithDomains(fuzztest::InRange(0, 23), fuzztest::InRange(0, 59));

}  // namespace
