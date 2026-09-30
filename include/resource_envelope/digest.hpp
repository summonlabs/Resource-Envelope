#ifndef RESOURCE_ENVELOPE_DIGEST_HPP
#define RESOURCE_ENVELOPE_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "resource_envelope/bytes.hpp"
#include "resource_envelope/export.hpp"

namespace resource_envelope {

// SHA-256 over a canonical encoding. Resource Envelope uses SHA-256 wherever a
// digest protects a meaningful binding: envelope content digests, decision-record
// digests, request and evidence digests, manifest state digests and journal frame
// digests. Implemented in-tree so the repository carries no runtime dependency,
// and validated against published FIPS 180-4 known-answer vectors by the suite.
class RESOURCE_ENVELOPE_API Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  using Value = std::array<std::uint8_t, kDigestBytes>;

  Sha256() noexcept;

  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view text) noexcept;

  // Finalises and returns the digest. The instance must not be reused afterwards.
  [[nodiscard]] Value finish() noexcept;
  [[nodiscard]] Bytes finish_bytes() noexcept;

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_;
  std::size_t buffered_;
  std::uint64_t total_bytes_;
  bool finished_;
};

// Domain-separated hash. The ASCII domain tag is hashed first, so a digest
// computed for one purpose can never equal one computed for another purpose over
// identical bytes.
RESOURCE_ENVELOPE_API std::array<std::uint8_t, 32> sha256_domain(
    std::string_view domain, std::span<const std::uint8_t> data) noexcept;
RESOURCE_ENVELOPE_API std::array<std::uint8_t, 32> sha256_domain(std::string_view domain,
                                                                std::string_view data) noexcept;

RESOURCE_ENVELOPE_API std::string to_hex(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API std::string to_hex(std::uint64_t value);
// Parses exactly 2 * out.size() lowercase hexadecimal characters. Any other
// length, any non-hex character, or uppercase input is rejected.
RESOURCE_ENVELOPE_API bool from_hex(std::string_view text, std::span<std::uint8_t> out) noexcept;
RESOURCE_ENVELOPE_API bool from_hex_u64(std::string_view text, std::uint64_t& out) noexcept;

// A 32-byte SHA-256 digest with a stable textual form. An absent digest is
// modelled by an explicit flag rather than an all-zero value, so a zero digest can
// never be confused with "no digest recorded".
class RESOURCE_ENVELOPE_API Digest {
 public:
  using Value = std::array<std::uint8_t, 32>;

  Digest() noexcept = default;
  explicit Digest(Value value) noexcept : value_(value), known_(true) {}

  [[nodiscard]] static Digest unknown() noexcept { return Digest(); }

  [[nodiscard]] bool known() const noexcept { return known_; }
  [[nodiscard]] const Value& value() const noexcept { return value_; }
  // Lowercase hexadecimal, or the literal token "unknown" when absent.
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] friend bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.known_ == b.known_ && (!a.known_ || a.value_ == b.value_);
  }
  [[nodiscard]] friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  [[nodiscard]] friend bool operator<(const Digest& a, const Digest& b) noexcept {
    if (a.known_ != b.known_) return a.known_ < b.known_;
    return a.value_ < b.value_;
  }

 private:
  Value value_{};
  bool known_ = false;
};

RESOURCE_ENVELOPE_API std::string digest_hex(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API std::string digest_hex(std::string_view data);
RESOURCE_ENVELOPE_API std::string digest_hex_u64(std::uint64_t value);

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_DIGEST_HPP
