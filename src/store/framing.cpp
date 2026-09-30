#include "resource_envelope/store.hpp"

#include <algorithm>
#include <array>

#include "resource_envelope/canonical.hpp"

namespace resource_envelope {
namespace {

// Magic values are fixed literals, so a foreign or truncated file is rejected on its
// first eight bytes instead of being parsed as a damaged store.
constexpr std::array<std::uint8_t, 8> kFrameMagic = {'R', 'E', 'N', 'V', 'F', 'R', 'M', '1'};
constexpr std::array<std::uint8_t, 8> kSegmentMagic = {'R', 'E', 'N', 'V', 'S', 'E', 'G', '1'};

constexpr std::array<std::uint32_t, 256> build_crc32_table() noexcept {
  // The reflected IEEE 802.3 polynomial, built once at compile time so that no run-time
  // initialisation order can affect the checksum.
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t value = index;
    for (unsigned bit = 0; bit < 8U; ++bit) {
      value = (value & 1U) != 0U ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32Table = build_crc32_table();

}  // namespace

std::uint32_t crc32_ieee_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept {
  std::uint32_t value = seed;
  for (const std::uint8_t byte : data) {
    value = kCrc32Table[(value ^ static_cast<std::uint32_t>(byte)) & 0xFFU] ^ (value >> 8U);
  }
  return value;
}

std::uint32_t crc32_ieee(std::span<const std::uint8_t> data) noexcept {
  return crc32_ieee_extend(0xFFFFFFFFU, data) ^ 0xFFFFFFFFU;
}

const char* to_string(JournalEntryKind kind) noexcept {
  switch (kind) {
    case JournalEntryKind::Snapshot: return "snapshot";
    case JournalEntryKind::EnvelopeDeclaration: return "envelope-declaration";
    case JournalEntryKind::EnvelopeRevision: return "envelope-revision";
    case JournalEntryKind::UsageCommit: return "usage-commit";
    case JournalEntryKind::Decision: return "decision";
    case JournalEntryKind::Observation: return "observation";
    case JournalEntryKind::EnvelopeTombstone: return "envelope-tombstone";
  }
  return "unknown-entry-kind";
}

bool is_valid(JournalEntryKind kind) noexcept {
  const std::uint16_t raw = static_cast<std::uint16_t>(kind);
  return raw >= static_cast<std::uint16_t>(JournalEntryKind::Snapshot) &&
         raw <= static_cast<std::uint16_t>(JournalEntryKind::EnvelopeTombstone);
}

std::string segment_file_name(std::uint64_t epoch) {
  // The name is generated from a validated numeric identifier only. Nothing a caller
  // supplies can reach the filesystem, so traversal, alternate-data-stream syntax,
  // reserved device names and reparse-point tricks cannot arise here.
  std::string digits = std::to_string(epoch);
  while (digits.size() < 20U) digits.insert(0U, 1U, '0');
  return "segment-" + digits + ".renv";
}

bool parse_segment_file_name(std::string_view name, std::uint64_t& epoch) noexcept {
  constexpr std::string_view kPrefix = "segment-";
  constexpr std::string_view kSuffix = ".renv";
  if (name.size() != kPrefix.size() + 20U + kSuffix.size()) return false;
  if (name.substr(0, kPrefix.size()) != kPrefix) return false;
  if (name.substr(name.size() - kSuffix.size()) != kSuffix) return false;
  std::uint64_t value = 0U;
  for (std::size_t index = kPrefix.size(); index < kPrefix.size() + 20U; ++index) {
    const char character = name[index];
    if (character < '0' || character > '9') return false;
    value = value * 10U + static_cast<std::uint64_t>(character - '0');
  }
  epoch = value;
  return true;
}

}  // namespace resource_envelope
