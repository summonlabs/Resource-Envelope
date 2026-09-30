#ifndef RESOURCE_ENVELOPE_TIME_HPP
#define RESOURCE_ENVELOPE_TIME_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "resource_envelope/export.hpp"

namespace resource_envelope {

// Nanoseconds since the Unix epoch, always UTC. Every authoritative decision uses
// a caller-supplied clock reading, so nothing on a decision path in this library
// reads the system clock and evaluation is reproducible from recorded inputs.
using Timestamp = std::int64_t;

inline constexpr Timestamp kUnixEpoch = 0;
// The largest representable timestamp. The model is intentionally bounded by the
// 64-bit nanosecond representation rather than by a calendar year, and formatting
// clamps into the RFC 3339 four-digit-year range so that output is always well
// formed while every stored value round-trips exactly.
inline constexpr Timestamp kTimestampMax = std::numeric_limits<Timestamp>::max();

[[nodiscard]] RESOURCE_ENVELOPE_API bool is_valid_timestamp(Timestamp value) noexcept;

// RFC 3339 / ISO 8601 UTC with nanosecond precision and a mandatory Z suffix, for
// example 2026-02-14T08:31:07.000000123Z. Parsing also accepts a numeric offset,
// which is converted to UTC before use.
RESOURCE_ENVELOPE_API std::string format_timestamp(Timestamp value);
RESOURCE_ENVELOPE_API bool parse_timestamp(std::string_view text, Timestamp& out) noexcept;

// Formats without the fractional part. Used only for human-facing summaries.
RESOURCE_ENVELOPE_API std::string format_timestamp_seconds(Timestamp value);

// Timestamp addition that fails instead of wrapping when the result would leave
// the representable RFC 3339 range.
[[nodiscard]] RESOURCE_ENVELOPE_API bool checked_add_timestamp(Timestamp base, Timestamp delta,
                                                              Timestamp& out) noexcept;

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_TIME_HPP
