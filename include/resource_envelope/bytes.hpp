#ifndef RESOURCE_ENVELOPE_BYTES_HPP
#define RESOURCE_ENVELOPE_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "resource_envelope/export.hpp"

namespace resource_envelope {

using Bytes = std::vector<std::uint8_t>;

// Canonical binary encoding helper. Every digested or persisted representation in
// this repository is produced by ByteWriter, which fixes endianness (little
// endian), declares every field width explicitly, and length-prefixes every
// variable-size field. An encoding is therefore exact, bounded, and independent of
// host layout, padding, or container iteration order.
class RESOURCE_ENVELOPE_API ByteWriter {
 public:
  void u8(std::uint8_t value);
  void bool_value(bool value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  // Length-prefixed byte string: u32 length followed by exactly that many bytes.
  void blob(std::span<const std::uint8_t> value);
  // Length-prefixed UTF-8 text, recorded exactly as supplied.
  void text(std::string_view value);
  void raw(std::span<const std::uint8_t> value);

  [[nodiscard]] const Bytes& bytes() const noexcept { return buffer_; }
  [[nodiscard]] Bytes take() noexcept { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  Bytes buffer_;
};

// Strict decoder. Every read is bounds-checked; a short read, an over-long length
// prefix, or trailing unconsumed bytes (via require_exhausted) is a hard failure
// rather than a value that happens to fit.
class RESOURCE_ENVELOPE_API ByteReader {
 public:
  explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool empty() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  std::uint8_t u8();
  bool bool_value();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int64_t i64();
  Bytes blob(std::size_t max_length);
  std::string text(std::size_t max_length);
  Bytes raw(std::size_t count);

  // True only when the reader is still valid and every byte was consumed.
  [[nodiscard]] bool require_exhausted() const noexcept { return ok_ && offset_ == data_.size(); }

  void fail() noexcept { ok_ = false; }

 private:
  [[nodiscard]] bool need(std::size_t count) noexcept;

  std::span<const std::uint8_t> data_;
  std::size_t offset_ = 0;
  bool ok_ = true;
};

// Stable 64-bit content digest used for canonical-encoding invariance and for
// cheap equality of long canonical forms. SHA-256 remains the digest that protects
// identity bindings, persistence and decision authority.
RESOURCE_ENVELOPE_API std::uint64_t fnv1a64(std::span<const std::uint8_t> data) noexcept;
RESOURCE_ENVELOPE_API std::uint64_t fnv1a64_extend(std::uint64_t seed, std::string_view text) noexcept;

RESOURCE_ENVELOPE_API bool constant_time_equal(std::span<const std::uint8_t> a,
                                               std::span<const std::uint8_t> b) noexcept;

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_BYTES_HPP
