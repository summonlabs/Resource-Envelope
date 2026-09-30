#include "resource_envelope/text.hpp"

#include <algorithm>
#include <array>

namespace resource_envelope {
namespace {

bool is_continuation(unsigned char byte) noexcept { return (byte & 0xC0U) == 0x80U; }

bool is_control(unsigned char byte) noexcept { return byte < 0x20U || byte == 0x7FU; }

constexpr std::array<std::string_view, 22> kReservedNames = {
    "con", "prn", "aux", "nul", "com0", "com1", "com2", "com3", "com4", "com5", "com6",
    "com7", "com8", "com9", "lpt0", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7"};

constexpr std::array<std::string_view, 4> kReservedNamesExtra = {"lpt8", "lpt9", "conin$", "conout$"};

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead == 0x00U) return false;
    if (lead < 0x80U) {
      ++index;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
      code_point = static_cast<std::uint32_t>(lead & 0x1FU);
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
      code_point = static_cast<std::uint32_t>(lead & 0x0FU);
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
      code_point = static_cast<std::uint32_t>(lead & 0x07U);
    } else {
      // 0x80-0xC1 are continuation or overlong leads, 0xF5-0xFF are invalid leads.
      return false;
    }
    if (index + length > text.size()) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const unsigned char byte = static_cast<unsigned char>(text[index + offset]);
      if (!is_continuation(byte)) return false;
      code_point = (code_point << 6U) | static_cast<std::uint32_t>(byte & 0x3FU);
    }
    // Overlong, surrogate, and out-of-range code points are all rejected.
    if (length == 3U && code_point < 0x800U) return false;
    if (length == 4U && code_point < 0x10000U) return false;
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) return false;
    if (code_point > 0x10FFFFU) return false;
    index += length;
  }
  return true;
}

bool is_reserved_device_name(std::string_view text) noexcept {
  if (text.empty()) return false;
  const std::size_t dot = text.find('.');
  std::string_view base = dot == std::string_view::npos ? text : text.substr(0, dot);
  // Trailing spaces are treated as part of the base name by the Windows path parser.
  while (!base.empty() && base.back() == ' ') base.remove_suffix(1);
  std::string lowered;
  lowered.reserve(base.size());
  for (const char character : base) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  for (const std::string_view reserved : kReservedNames) {
    if (lowered == reserved) return true;
  }
  for (const std::string_view reserved : kReservedNamesExtra) {
    if (lowered == reserved) return true;
  }
  return false;
}

TextDefect validate_identifier(std::string_view text, IdentifierUse use) noexcept {
  if (text.empty()) return TextDefect::Empty;
  if (!is_valid_utf8(text)) return TextDefect::InvalidUtf8;

  if (use == IdentifierUse::Label) {
    if (text.size() > kLabelMaxLength) return TextDefect::TooLong;
    for (const char character : text) {
      if (is_control(static_cast<unsigned char>(character))) return TextDefect::ControlCharacter;
    }
    if (text.front() == ' ' || text.back() == ' ') return TextDefect::SurroundingWhitespace;
    if (text.back() == '.' || text.back() == ' ') return TextDefect::TrailingDotOrSpace;
    return TextDefect::None;
  }

  if (text.size() > kIdentifierMaxLength) return TextDefect::TooLong;
  for (const char character : text) {
    const bool allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                         character == '-';
    if (!allowed) return TextDefect::InvalidCharacter;
  }
  if (text.find("--") != std::string_view::npos) return TextDefect::ConsecutiveHyphens;
  if (text.front() == '-' || text.back() == '-') return TextDefect::LeadingOrTrailingHyphen;
  if (text.back() == '.' || text.back() == ' ') return TextDefect::TrailingDotOrSpace;
  if (is_reserved_device_name(text)) return TextDefect::ReservedDeviceName;
  return TextDefect::None;
}

const char* to_string(TextDefect defect) noexcept {
  switch (defect) {
    case TextDefect::None: return "None";
    case TextDefect::Empty: return "Empty";
    case TextDefect::TooLong: return "TooLong";
    case TextDefect::InvalidUtf8: return "InvalidUtf8";
    case TextDefect::InvalidCharacter: return "InvalidCharacter";
    case TextDefect::LeadingOrTrailingHyphen: return "LeadingOrTrailingHyphen";
    case TextDefect::ConsecutiveHyphens: return "ConsecutiveHyphens";
    case TextDefect::ReservedDeviceName: return "ReservedDeviceName";
    case TextDefect::ControlCharacter: return "ControlCharacter";
    case TextDefect::SurroundingWhitespace: return "SurroundingWhitespace";
    case TextDefect::TrailingDotOrSpace: return "TrailingDotOrSpace";
  }
  return "UnknownTextDefect";
}

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  const auto is_space = [](char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
  };
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1U])) --end;
  return std::string(text.substr(begin, end - begin));
}

bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
  if (text.size() < suffix.size()) return false;
  return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split_commas(std::string_view text) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t comma = text.find(',', begin);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    std::string piece = trim(text.substr(begin, end - begin));
    if (!piece.empty()) parts.push_back(std::move(piece));
    if (comma == std::string_view::npos) break;
    begin = comma + 1U;
  }
  return parts;
}

}  // namespace resource_envelope
