// Gmbux: what the market is played for. They are not money and cannot be
// bought; everyone starts with the same number and wins or loses them by
// predicting.
//
// An amount is exact to a hundredth of a gmbux, which is as finely as shares
// are priced, so sums of them never drift the way sums of fractions do.

#ifndef MARKET_GMBUX_H_
#define MARKET_GMBUX_H_

#include <compare>
#include <cstdint>
#include <string>

namespace market {

class Gmbux {
 public:
  // No gmbux.
  constexpr Gmbux() = default;

  // `whole` gmbux, or `hundredths` hundredths of one.
  static constexpr Gmbux Whole(int64_t whole) { return Gmbux(whole * 100); }
  static constexpr Gmbux Hundredths(int64_t hundredths) {
    return Gmbux(hundredths);
  }

  // The amount in hundredths of a gmbux, which is how to store it.
  constexpr int64_t hundredths() const { return hundredths_; }

  // As shown to people: "12" for a whole number of gmbux, otherwise to two
  // places, as in "12.50" and "-0.05".
  std::string Describe() const;

  friend constexpr auto operator<=>(Gmbux, Gmbux) = default;

  friend constexpr Gmbux operator+(Gmbux a, Gmbux b) {
    return Gmbux(a.hundredths_ + b.hundredths_);
  }
  friend constexpr Gmbux operator-(Gmbux a, Gmbux b) {
    return Gmbux(a.hundredths_ - b.hundredths_);
  }
  constexpr Gmbux& operator+=(Gmbux other) { return *this = *this + other; }
  constexpr Gmbux& operator-=(Gmbux other) { return *this = *this - other; }

 private:
  explicit constexpr Gmbux(int64_t hundredths) : hundredths_(hundredths) {}

  int64_t hundredths_ = 0;
};

}  // namespace market

#endif  // MARKET_GMBUX_H_
