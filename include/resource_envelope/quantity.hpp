#ifndef RESOURCE_ENVELOPE_QUANTITY_HPP
#define RESOURCE_ENVELOPE_QUANTITY_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "resource_envelope/export.hpp"

namespace resource_envelope {

// A non-negative scaled quantity: a count of 1e-9 base units held in an unsigned
// 64-bit integer. Every physically meaningful envelope magnitude (up to 9.22e9
// base units) is exactly representable without floating point or a bignum, which
// is what makes the residual arithmetic in eval/evaluate.cpp exact.
using Nanounits = std::uint64_t;
using DimensionIndex = std::uint8_t;

inline constexpr Nanounits kNanounitsPerUnit = 1000000000ULL;
// Absolute representable bound. Reaching it is reported as an overflow, never as
// a silent saturation.
inline constexpr Nanounits kQuantityMax = 0xFFFFFFFFFFFFFFFFULL;
// Declared envelope quantities above this magnitude are refused as out of range
// rather than being accepted and then arithmetically clipped later.
inline constexpr Nanounits kQuantityReasonableMax = 1ULL << 53U;

// Rack-unit style discrete counts are small and bounded.
using UnitCount = std::uint32_t;
inline constexpr UnitCount kUnitCountMax = 1000000U;

// Result of an arithmetic primitive. `overflow` means the exact result is not
// representable; `value` is then saturated at the relevant bound and callers must
// report the overflow instead of using the saturated value as if it were exact.
struct RESOURCE_ENVELOPE_API CheckedQuantity {
  Nanounits value = 0;
  bool overflow = false;

  [[nodiscard]] constexpr bool ok() const noexcept { return !overflow; }
};

[[nodiscard]] RESOURCE_ENVELOPE_API CheckedQuantity checked_add(Nanounits a, Nanounits b) noexcept;
[[nodiscard]] RESOURCE_ENVELOPE_API CheckedQuantity checked_sub(Nanounits a, Nanounits b) noexcept;

// Converts a base-unit and nanounit pair into a scaled quantity, rejecting any
// magnitude above kQuantityReasonableMax and any nanounit remainder above 1e9-1.
[[nodiscard]] RESOURCE_ENVELOPE_API bool make_quantity(std::uint64_t whole_units, std::uint32_t nanounits,
                                                       Nanounits& out) noexcept;

// Parses a canonical decimal quantity: optional sign (negative is rejected),
// decimal digits, and at most nine fractional digits. Whitespace, exponents,
// digit separators, and any other form are rejected.
[[nodiscard]] RESOURCE_ENVELOPE_API bool parse_quantity(std::string_view text, Nanounits& out) noexcept;

// Canonical decimal form: trailing fractional zeros removed, no decimal point when
// there is no fractional part. Zero always renders as "0".
RESOURCE_ENVELOPE_API std::string format_quantity(Nanounits value);

// Rounds up to the next multiple of `step`, reporting overflow when the rounded
// value is not representable. step == 0 means the dimension declares no alignment
// requirement and the input is returned unchanged.
[[nodiscard]] RESOURCE_ENVELOPE_API CheckedQuantity round_up_to_multiple(Nanounits value,
                                                                        Nanounits step) noexcept;

// True when value is an exact multiple of step. step == 0 is never aligned.
[[nodiscard]] RESOURCE_ENVELOPE_API bool is_multiple_of(Nanounits value, Nanounits step) noexcept;

// Rounds a per-unit rate multiplied by a count up to the next whole base unit.
// This is the deterministic conversion used for cooling energy budgets, and it is
// the only place in the runtime where a non-exact conversion is permitted.
[[nodiscard]] RESOURCE_ENVELOPE_API CheckedQuantity rate_times_count(Nanounits rate,
                                                                     UnitCount count) noexcept;

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_QUANTITY_HPP
