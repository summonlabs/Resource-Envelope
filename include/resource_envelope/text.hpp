#ifndef RESOURCE_ENVELOPE_TEXT_HPP
#define RESOURCE_ENVELOPE_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "resource_envelope/export.hpp"

namespace resource_envelope {

inline constexpr std::size_t kIdentifierMaxLength = 64;
inline constexpr std::size_t kLabelMaxLength = 128;
inline constexpr std::size_t kProvenanceMaxLength = 256;

enum class IdentifierUse : std::uint8_t {
  // Portable, filesystem-safe, canonical lowercase kebab-case:
  //   ^[a-z0-9]([a-z0-9-]*[a-z0-9])?$  with no consecutive hyphens, length <= 64.
  // Envelope, tenant, service and site identifiers must all satisfy this, which
  // makes every identifier usable as a durable path segment without escaping.
  Canonical = 0,
  // Free-form UTF-8 label for human-facing provenance text. Leading/trailing
  // whitespace and interior control characters are rejected.
  Label = 1,
};

// Strict UTF-8 validation: rejects overlong encodings, UTF-16 surrogate code
// points, code points above U+10FFFF, embedded NUL, and truncated sequences.
RESOURCE_ENVELOPE_API bool is_valid_utf8(std::string_view text) noexcept;

// Machine-readable identifier defect so that a refused request can name the exact
// rule that failed instead of reporting a generic parse error.
enum class TextDefect : std::uint8_t {
  None = 0,
  Empty = 1,
  TooLong = 2,
  InvalidUtf8 = 3,
  InvalidCharacter = 4,
  LeadingOrTrailingHyphen = 5,
  ConsecutiveHyphens = 6,
  ReservedDeviceName = 7,
  ControlCharacter = 8,
  SurroundingWhitespace = 9,
  TrailingDotOrSpace = 10,
};

RESOURCE_ENVELOPE_API TextDefect validate_identifier(std::string_view text, IdentifierUse use) noexcept;
RESOURCE_ENVELOPE_API const char* to_string(TextDefect defect) noexcept;

// Windows reserved device names (CON, PRN, AUX, NUL, COM0-9, LPT0-9) are rejected
// on every platform so that a store created on one system stays usable when copied
// to another and so that no durable path segment can become unopenable. The check
// is applied to the base name with any extension removed.
RESOURCE_ENVELOPE_API bool is_reserved_device_name(std::string_view text) noexcept;

RESOURCE_ENVELOPE_API std::string trim(std::string_view text);
RESOURCE_ENVELOPE_API bool starts_with(std::string_view text, std::string_view prefix) noexcept;
RESOURCE_ENVELOPE_API bool ends_with(std::string_view text, std::string_view suffix) noexcept;

// Splits a comma-separated argument, trimming each element and dropping empty
// elements. Never returns a partially-built element.
RESOURCE_ENVELOPE_API std::vector<std::string> split_commas(std::string_view text);

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_TEXT_HPP
