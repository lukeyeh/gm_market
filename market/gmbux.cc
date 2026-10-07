#include "market/gmbux.h"

#include <cstdint>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace market {

std::string Gmbux::Describe() const {
  const int64_t magnitude = hundredths_ < 0 ? -hundredths_ : hundredths_;
  const int64_t whole = magnitude / 100;
  const int64_t fraction = magnitude % 100;

  const char* const sign = hundredths_ < 0 ? "-" : "";
  if (fraction == 0) return absl::StrCat(sign, whole);

  return absl::StrFormat("%s%d.%02d", sign, whole, fraction);
}

}  // namespace market
