#include "resource_envelope/bytes.hpp"

#include <cstring>

namespace resource_envelope {
namespace {

constexpr std::size_t kBlobLengthBytes = 4;
// A blob or text field may not describe more bytes than the decoder was handed.
constexpr std::size_t kMaxBlobBytes = 0xFFFFFFFFUL;

void append_u16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32(Bytes& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void append_u64(Bytes& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

}  // namespace

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(value); }

void ByteWriter::bool_value(bool value) { buffer_.push_back(value ? 1U : 0U); }

void ByteWriter::u16(std::uint16_t value) { append_u16(buffer_, value); }

void ByteWriter::u32(std::uint32_t value) { append_u32(buffer_, value); }

void ByteWriter::u64(std::uint64_t value) { append_u64(buffer_, value); }

void ByteWriter::i64(std::int64_t value) {
  // Two's-complement reinterpretation of the same bit pattern, which is what the
  // canonical encoding fixes for signed values.
  append_u64(buffer_, static_cast<std::uint64_t>(value));
}

void ByteWriter::blob(std::span<const std::uint8_t> value) {
  u32(static_cast<std::uint32_t>(value.size()));
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void ByteWriter::text(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void ByteWriter::raw(std::span<const std::uint8_t> value) {
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

bool ByteReader::need(std::size_t count) noexcept {
  if (!ok_) return false;
  if (count > remaining()) {
    ok_ = false;
    return false;
  }
  return true;
}

std::uint8_t ByteReader::u8() {
  if (!need(1)) return 0;
  const std::uint8_t value = data_[offset_];
  offset_ += 1;
  return value;
}

bool ByteReader::bool_value() {
  const std::uint8_t value = u8();
  if (!ok_) return false;
  if (value > 1U) {
    ok_ = false;
    return false;
  }
  return value == 1U;
}

std::uint16_t ByteReader::u16() {
  if (!need(2)) return 0;
  const std::uint16_t value = static_cast<std::uint16_t>(data_[offset_]) |
                              static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8U);
  offset_ += 2;
  return value;
}

std::uint32_t ByteReader::u32() {
  if (!need(4)) return 0;
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(data_[offset_ + index]) << (8U * index);
  }
  offset_ += 4;
  return value;
}

std::uint64_t ByteReader::u64() {
  if (!need(8)) return 0;
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(data_[offset_ + index]) << (8U * index);
  }
  offset_ += 8;
  return value;
}

std::int64_t ByteReader::i64() { return static_cast<std::int64_t>(u64()); }

Bytes ByteReader::blob(std::size_t max_length) {
  const std::uint32_t declared = u32();
  if (!ok_) return {};
  if (static_cast<std::size_t>(declared) > max_length || static_cast<std::size_t>(declared) > remaining()) {
    ok_ = false;
    return {};
  }
  Bytes value(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
              data_.begin() + static_cast<std::ptrdiff_t>(offset_ + declared));
  offset_ += declared;
  return value;
}

std::string ByteReader::text(std::size_t max_length) {
  const std::uint32_t declared = u32();
  if (!ok_) return {};
  if (static_cast<std::size_t>(declared) > max_length || static_cast<std::size_t>(declared) > remaining()) {
    ok_ = false;
    return {};
  }
  std::string value(reinterpret_cast<const char*>(data_.data() + offset_), static_cast<std::size_t>(declared));
  offset_ += declared;
  return value;
}

Bytes ByteReader::raw(std::size_t count) {
  if (!need(count)) return {};
  Bytes value(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
              data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
  offset_ += count;
  return value;
}

std::uint64_t fnv1a64(std::span<const std::uint8_t> data) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::uint8_t byte : data) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint64_t fnv1a64_extend(std::uint64_t seed, std::string_view text) noexcept {
  std::uint64_t hash = seed;
  for (const char character : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool constant_time_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept {
  if (a.size() != b.size()) return false;
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < a.size(); ++index) {
    difference = static_cast<std::uint8_t>(difference | (a[index] ^ b[index]));
  }
  return difference == 0;
}

}  // namespace resource_envelope
