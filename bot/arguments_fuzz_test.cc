// What people type into commands, checked on whatever they might type: it
// is read or refused, never anything else, and every member's mention reads
// back as that member.
//
//   bazel test //bot:arguments_fuzz_test

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "bot/arguments.h"
#include "discord/model.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "market/lmsr.h"

namespace {

// Any text names a member, who is then a real one, or is refused as
// InvalidArgument.
void ReadsAMemberOrRefuses(const std::string& text) {
  const absl::StatusOr<discord::UserId> member = bot::ParseMember(text);
  if (!member.ok()) {
    ASSERT_TRUE(absl::IsInvalidArgument(member.status())) << member.status();
    return;
  }

  // No member has the id 0.
  ASSERT_NE(member->value, 0);
}

FUZZ_TEST(ArgumentsFuzzTest, ReadsAMemberOrRefuses)
    .WithDomains(
        fuzztest::OneOf(fuzztest::Arbitrary<std::string>(),
                        fuzztest::InRegexp("<?@?[!&]?-?[0-9]{0,22}>?")));

// Every member there could be is named by their mention, in either of the
// forms Discord writes one, and by their id alone.
void EveryMemberReadsBack(uint64_t id) {
  const discord::UserId member = {
      .value = id,
  };
  const std::string digits = std::to_string(id);

  for (const std::string& written : {
           discord::Mention(member),
           "<@!" + digits + ">",
           digits,
           " " + discord::Mention(member) + " ",
       }) {
    const absl::StatusOr<discord::UserId> read = bot::ParseMember(written);
    ASSERT_TRUE(read.ok()) << written << ": " << read.status();
    ASSERT_EQ(*read, member) << written;
  }
}

FUZZ_TEST(ArgumentsFuzzTest, EveryMemberReadsBack)
    .WithDomains(fuzztest::Positive<uint64_t>());

// Any text picks a side or is refused as InvalidArgument.
void ReadsAnOutcomeOrRefuses(const std::string& text) {
  const absl::StatusOr<market::Outcome> outcome = bot::ParseOutcome(text);
  if (!outcome.ok()) {
    ASSERT_TRUE(absl::IsInvalidArgument(outcome.status())) << outcome.status();
  }
}

FUZZ_TEST(ArgumentsFuzzTest, ReadsAnOutcomeOrRefuses)
    .WithDomains(
        fuzztest::OneOf(fuzztest::Arbitrary<std::string>(),
                        fuzztest::InRegexp("[ ]?[yYnN][eEoO]?[sS]?[ ]?")));

}  // namespace
