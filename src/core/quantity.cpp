#include "resource_envelope/quantity.hpp"

#include <limits>

#include "resource_envelope/bytes.hpp"

namespace resource_envelope {
namespace {

// Digits of 2^64-1, which is the largest quantity the representation can hold.
constexpr Nanounits kMaxValue = std::numeric_limits<Nanounits>::max();

Nanounits pow10(unsigned exponent) noexcept {
  Nanounits value = 1;
  for (unsigned index = 0; index < exponent; ++index) value *= 10U;
  return value;
}

}  // namespace

CheckedQuantity checked_add(Nanounits a, Nanounits b) noexcept {
  if (a > kMaxValue - b) return CheckedQuantity{kMaxValue, true};
  return CheckedQuantity{a + b, false};
}

CheckedQuantity checked_sub(Nanounits a, Nanounits b) noexcept {
  if (b > a) return CheckedQuantity{0, true};
  return CheckedQuantity{a - b, false};
}

bool make_quantity(std::uint64_t whole_units, std::uint32_t nanounits, Nanounits& out) noexcept {
  if (nanounits >= static_cast<std::uint32_t>(kNanounitsPerUnit)) return false;
  // The whole part is bounded so that scaling it cannot overflow, and it is divided by one
  // base unit rather than by the declared bound: dividing the bound would truncate it.
  constexpr Nanounits kMaxWholeUnits = kQuantityMax / kNanounitsPerUnit;
  if (whole_units > kMaxWholeUnits) return false;
  const Nanounits scaled = whole_units * kNanounitsPerUnit;
  if (scaled > kQuantityReasonableMax) return false;
  // No subtraction here can underflow: scaled is at most kQuantityReasonableMax.
  if (nanounits > kQuantityReasonableMax - scaled) return false;
  out = scaled + nanounits;
  return true;
}

bool parse_quantity(std::string_view text, Nanounits& out) noexcept {
  if (text.empty() || text.size() > 40U) return false;
  std::size_t index = 0;
  if (text[0] == '+') {
    ++index;
  } else if (text[0] == '-') {
    // A negative quantity is never a valid envelope magnitude. It is refused here
    // rather than clamped, because clamping would silently change a sign.
    return false;
  }
  if (index >= text.size()) return false;

  // The whole part is accumulated while it still fits comfortably in 64 bits, and the
  // declared bound is applied to the final scaled value by make_quantity. Testing a
  // pre-divided ceiling inside the loop would truncate the bound and reject magnitudes
  // that are inside it.
  Nanounits whole = 0;
  std::size_t whole_digits = 0;
  constexpr Nanounits kAccumulationLimit = 0x0FFFFFFFFFFFFFFFULL;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    const Nanounits digit = static_cast<Nanounits>(text[index] - '0');
    if (whole > (kAccumulationLimit - digit) / 10U) return false;
    whole = whole * 10U + digit;
    ++whole_digits;
    ++index;
  }
  if (whole_digits == 0U) return false;

  std::uint32_t nanos = 0;
  std::size_t fraction_digits = 0;
  if (index < text.size()) {
    if (text[index] != '.') return false;
    ++index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
      if (fraction_digits >= 9U) return false;
      nanos = nanos * 10U + static_cast<std::uint32_t>(text[index] - '0');
      ++fraction_digits;
      ++index;
    }
    if (fraction_digits == 0U) return false;
  }
  if (index != text.size()) return false;
  for (std::size_t pad = fraction_digits; pad < 9U; ++pad) nanos *= 10U;
  return make_quantity(whole, nanos, out);
}

std::string format_quantity(Nanounits value) {
  const Nanounits whole = value / kNanounitsPerUnit;
  const Nanounits fraction = value % kNanounitsPerUnit;
  std::string whole_text = std::to_string(whole);
  if (fraction == 0) return whole_text;
  std::string fraction_text = std::to_string(fraction);
  // Zero-pad to nine digits, then strip trailing zeros so the form is canonical.
  fraction_text.insert(0, 9U - fraction_text.size(), '0');
  while (!fraction_text.empty() && fraction_text.back() == '0') fraction_text.pop_back();
  return whole_text + "." + fraction_text;
}

CheckedQuantity round_up_to_multiple(Nanounits value, Nanounits step) noexcept {
  if (step == 0) return CheckedQuantity{value, false};
  const Nanounits remainder = value % step;
  if (remainder == 0) return CheckedQuantity{value, false};
  const Nanounits increment = step - remainder;
  if (value > kMaxValue - increment) return CheckedQuantity{kMaxValue, true};
  return CheckedQuantity{value + increment, false};
}

bool is_multiple_of(Nanounits value, Nanounits step) noexcept {
  if (step == 0) return false;
  return value % step == 0;
}

CheckedQuantity rate_times_count(Nanounits rate, UnitCount count) noexcept {
  if (count == 0U || rate == 0U) return CheckedQuantity{0, false};
  const Nanounits units = rate / kNanounitsPerUnit;
  const Nanounits fraction = rate % kNanounitsPerUnit;
  if (units != 0 && static_cast<Nanounits>(count) > kMaxValue / units) {
    return CheckedQuantity{kMaxValue, true};
  }
  const Nanounits whole = units * static_cast<Nanounits>(count);
  if (fraction != 0) {
    // The fractional remainder is rounded up to one whole base unit. This is the
    // only non-exact conversion in the runtime and it is deliberately conservative,
    // because rounding down would understate a cooling budget.
    const Nanounits extra = fraction * static_cast<Nanounits>(count);
    const Nanounits rounded = (extra + kNanounitsPerUnit - 1U) / kNanounitsPerUnit;
    if (whole > kMaxValue - rounded) return CheckedQuantity{kMaxValue, true};
    return CheckedQuantity{whole + rounded, false};
  }
  return CheckedQuantity{whole, false};
}

}  // namespace resource_envelope
