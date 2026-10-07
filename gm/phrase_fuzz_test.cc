// What counts as a GM, checked against a model of gm_bot's rule.
//
// SaysGm decides with arithmetic on hours and by trimming a phrase off
// either end of the message. The model keeps a phrase's hours as the set of
// hours found by going round the clock, and looks for the phrase by
// comparing pieces of the message, the way the rule is put in words. A fuzz
// test requires them to agree on any message, at any hour, with any phrases.
//
//   bazel test //gm:phrase_fuzz_test
//   bazel run --config=fuzz //gm:phrase_fuzz_test -- \
//       --fuzz=PhraseFuzzTest.AgreesWithTheModel --fuzz_for=60s

#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gm/phrase.h"
#include "gtest/gtest.h"

namespace {

// A phrase on a list: its text and the hours it counts between.
struct Listed {
  std::string text;
  int opens = 0;
  int closes = 0;
};

// The hours from `opens` round the clock until `closes`. From an hour round
// to the same hour is the whole day.
std::set<int> HoursBetween(int opens, int closes) {
  std::set<int> hours;
  int hour = opens;
  do {
    hours.insert(hour);
    hour = (hour + 1) % 24;
  } while (hour != closes);
  return hours;
}

bool IsLetterOrDigit(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

// `text` without the space around it, in lower case. Only the letters of
// English have another case here, and only what C calls space is space.
std::string Folded(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }

  std::string folded = text.substr(begin, end - begin);
  for (char& c : folded) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return folded;
}

// Whether `message` says `phrase`: it begins with it and what follows is
// not more of a word, or it ends with it and what comes before is not.
bool Says(const std::string& message, const std::string& phrase) {
  if (message.size() < phrase.size()) return false;

  const size_t rest = message.size() - phrase.size();
  const bool begins = message.compare(0, phrase.size(), phrase) == 0 &&
                      (rest == 0 || !IsLetterOrDigit(message[phrase.size()]));
  const bool ends = message.compare(rest, phrase.size(), phrase) == 0 &&
                    (rest == 0 || !IsLetterOrDigit(message[rest - 1]));

  return begins || ends;
}

void AgreesWithTheModel(const std::vector<Listed>& listed,
                        const std::string& message, int hour) {
  std::vector<gm::Phrase> phrases;
  bool expected = false;
  for (const Listed& entry : listed) {
    // As gm_bot keeps a phrase: folded, with hours 0 to 23.
    const std::string text = Folded(entry.text);
    phrases.push_back(gm::Phrase{
        .text = text,
        .opens = entry.opens,
        .closes = entry.closes % 24,
    });

    if (HoursBetween(entry.opens, entry.closes % 24).contains(hour) &&
        Says(Folded(message), text)) {
      expected = true;
    }
  }

  ASSERT_EQ(gm::SaysGm(message, phrases, hour), expected);
}

// Phrases and messages from a small vocabulary, so that messages say
// phrases, often several at once, with hours that overlap, do not, and run
// past midnight; and any bytes at all for either.
FUZZ_TEST(PhraseFuzzTest, AgreesWithTheModel)
    .WithDomains(fuzztest::VectorOf(
                     fuzztest::StructOf<Listed>(
                         fuzztest::OneOf(
                             fuzztest::Arbitrary<std::string>().WithMinSize(1),
                             fuzztest::ElementOf<std::string>({
                                 "gm",
                                 "good morning",
                                 "morning",
                                 "yo",
                                 "yo yo",
                                 "YO there",
                                 "gn",
                                 "good",
                             })),
                         fuzztest::InRange(0, 23), fuzztest::InRange(0, 24)))
                     .WithMaxSize(6),
                 fuzztest::OneOf(fuzztest::Arbitrary<std::string>(),
                                 fuzztest::ElementOf<std::string>({
                                     "gm",
                                     "GM everyone",
                                     "gmail",
                                     "ok gm",
                                     "ok, good morning",
                                     "good morning yo",
                                     "goodness",
                                     "yo",
                                     "Yo yo",
                                     "yoyo",
                                     "yo there yo",
                                     " gn! ",
                                     "a\tgm\n",
                                 })),
                 fuzztest::InRange(0, 23));

}  // namespace
