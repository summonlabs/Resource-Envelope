#include "resource_envelope/digest.hpp"

#include <cstring>

#include "resource_envelope/text.hpp"

namespace resource_envelope {
namespace {

// FIPS 180-4 round constants.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned amount) noexcept {
  return static_cast<std::uint32_t>((value >> amount) | (value << (32U - amount)));
}

constexpr char kHexDigits[] = "0123456789abcdef";

bool hex_value(char character, std::uint8_t& out) noexcept {
  if (character >= '0' && character <= '9') {
    out = static_cast<std::uint8_t>(character - '0');
    return true;
  }
  if (character >= 'a' && character <= 'f') {
    out = static_cast<std::uint8_t>(character - 'a' + 10);
    return true;
  }
  return false;
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU,
             0x5be0cd19U},
      buffer_{},
      buffered_(0),
      total_bytes_(0),
      finished_(false) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64] = {};
  for (unsigned index = 0; index < 16U; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24U) |
                      (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[index * 4U + 3U]);
  }
  for (unsigned index = 16U; index < 64U; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15U], 7U) ^ rotr(schedule[index - 15U], 18U) ^
                             (schedule[index - 15U] >> 3U);
    const std::uint32_t s1 = rotr(schedule[index - 2U], 17U) ^ rotr(schedule[index - 2U], 19U) ^
                             (schedule[index - 2U] >> 10U);
    schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (unsigned index = 0; index < 64U; ++index) {
    const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
  if (finished_) return;
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t take = (data.size() - offset) < (buffer_.size() - buffered_)
                                 ? (data.size() - offset)
                                 : (buffer_.size() - buffered_);
    std::memcpy(buffer_.data() + buffered_, data.data() + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == buffer_.size()) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Sha256::Value Sha256::finish() noexcept {
  Value digest{};
  if (finished_) return digest;
  finished_ = true;

  const std::uint64_t total_bits = total_bytes_ * 8ULL;
  buffer_[buffered_] = 0x80U;
  ++buffered_;
  if (buffered_ > 56U) {
    while (buffered_ < 64U) {
      buffer_[buffered_] = 0U;
      ++buffered_;
    }
    compress(buffer_.data());
    buffered_ = 0;
  }
  while (buffered_ < 56U) {
    buffer_[buffered_] = 0U;
    ++buffered_;
  }
  for (unsigned index = 0; index < 8U; ++index) {
    buffer_[56U + index] = static_cast<std::uint8_t>((total_bits >> (8U * (7U - index))) & 0xFFU);
  }
  compress(buffer_.data());

  for (std::size_t index = 0; index < 8U; ++index) {
    digest[index * 4U] = static_cast<std::uint8_t>((state_[index] >> 24U) & 0xFFU);
    digest[index * 4U + 1U] = static_cast<std::uint8_t>((state_[index] >> 16U) & 0xFFU);
    digest[index * 4U + 2U] = static_cast<std::uint8_t>((state_[index] >> 8U) & 0xFFU);
    digest[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index] & 0xFFU);
  }
  return digest;
}

Bytes Sha256::finish_bytes() noexcept {
  const Value digest = finish();
  return Bytes(digest.begin(), digest.end());
}

namespace {

std::array<std::uint8_t, 32> domain_hash(std::string_view domain, std::span<const std::uint8_t> body) noexcept {
  Sha256 hasher;
  // The tag is length-prefixed so that two different tags can never concatenate
  // into the same byte stream.
  std::uint32_t length = static_cast<std::uint32_t>(domain.size());
  std::uint8_t length_bytes[4] = {static_cast<std::uint8_t>(length & 0xFFU),
                                  static_cast<std::uint8_t>((length >> 8U) & 0xFFU),
                                  static_cast<std::uint8_t>((length >> 16U) & 0xFFU),
                                  static_cast<std::uint8_t>((length >> 24U) & 0xFFU)};
  hasher.update(std::span<const std::uint8_t>(length_bytes, 4));
  hasher.update(domain);
  hasher.update(body);
  return hasher.finish();
}

}  // namespace

std::array<std::uint8_t, 32> sha256_domain(std::string_view domain,
                                           std::span<const std::uint8_t> data) noexcept {
  return domain_hash(domain, data);
}

std::array<std::uint8_t, 32> sha256_domain(std::string_view domain, std::string_view data) noexcept {
  return domain_hash(domain,
                     std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()),
                                                   data.size()));
}

std::string to_hex(std::span<const std::uint8_t> data) {
  std::string out;
  out.reserve(data.size() * 2U);
  for (const std::uint8_t byte : data) {
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

std::string to_hex(std::uint64_t value) {
  std::string out(16, '0');
  for (unsigned index = 0; index < 16U; ++index) {
    out[15U - index] = kHexDigits[(value >> (4U * index)) & 0x0FU];
  }
  return out;
}

bool from_hex(std::string_view text, std::span<std::uint8_t> out) noexcept {
  if (text.size() != out.size() * 2U) return false;
  for (std::size_t index = 0; index < out.size(); ++index) {
    std::uint8_t high = 0;
    std::uint8_t low = 0;
    if (!hex_value(text[index * 2U], high) || !hex_value(text[index * 2U + 1U], low)) return false;
    out[index] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return true;
}

bool from_hex_u64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.size() != 16U) return false;
  std::uint64_t value = 0;
  for (const char character : text) {
    std::uint8_t digit = 0;
    if (!hex_value(character, digit)) return false;
    value = (value << 4U) | static_cast<std::uint64_t>(digit);
  }
  out = value;
  return true;
}

std::string Digest::to_string() const {
  if (!known_) return std::string("unknown");
  return to_hex(std::span<const std::uint8_t>(value_.data(), value_.size()));
}

std::string digest_hex(std::span<const std::uint8_t> data) {
  Sha256 hasher;
  hasher.update(data);
  const Sha256::Value value = hasher.finish();
  return to_hex(std::span<const std::uint8_t>(value.data(), value.size()));
}

std::string digest_hex(std::string_view data) {
  Sha256 hasher;
  hasher.update(data);
  const Sha256::Value value = hasher.finish();
  return to_hex(std::span<const std::uint8_t>(value.data(), value.size()));
}

std::string digest_hex_u64(std::uint64_t value) {
  Bytes body;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    body.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
  return digest_hex(std::span<const std::uint8_t>(body.data(), body.size()));
}

}  // namespace resource_envelope
