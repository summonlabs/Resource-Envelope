#include "support/testing.hpp"

#include "resource_envelope/digest.hpp"

using namespace resource_envelope;

namespace {
std::string sha256_hex(std::string_view text) {
  Sha256 hasher;
  hasher.update(text);
  const Sha256::Value value = hasher.finish();
  return to_hex(std::span<const std::uint8_t>(value.data(), value.size()));
}
}  // namespace

// FIPS 180-4 known-answer vectors. A digest implementation that is not checked
// against published vectors would make every binding in the runtime unverifiable.
RE_TEST(sha256_matches_published_known_answers) {
  RE_CHECK_EQ(sha256_hex(""),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  RE_CHECK_EQ(sha256_hex("abc"),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  RE_CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // A 63-byte input exercises the second padding block, which the published vectors
  // above never reach. The published vectors are what anchor this implementation to
  // FIPS 180-4; this case guards the padding path against regression.
  RE_CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopqabcdefgh"),
              std::string("684bec8a7d8fce7aea7758a984122085af34fa0ae77ad99906b66a7e95cfeb7f"));
}

RE_TEST(sha256_streams_are_equivalent_to_one_shot) {
  Sha256 split;
  split.update("abc");
  split.update("");
  const Sha256::Value first = split.finish();
  Sha256 whole;
  whole.update("abc");
  const Sha256::Value second = whole.finish();
  RE_CHECK(std::equal(first.begin(), first.end(), second.begin()));
}

RE_TEST(hex_round_trip_rejects_malformed_input) {
  Digest::Value value{};
  const std::string text(64, '0');
  RE_CHECK(from_hex(text, value));
  RE_CHECK_EQ(to_hex(std::span<const std::uint8_t>(value.data(), value.size())), text);
  RE_CHECK(!from_hex(std::string(63, '0'), value));
  RE_CHECK(!from_hex(std::string(65, '0'), value));
  RE_CHECK(!from_hex(std::string(64, 'G'), value));
  std::string mixed = text;
  mixed[0] = 'A';
  RE_CHECK(!from_hex(mixed, value));
  std::uint64_t number = 0;
  RE_CHECK(from_hex_u64("0123456789abcdef", number));
  RE_CHECK_EQ(number, 0x0123456789abcdefULL);
  RE_CHECK(!from_hex_u64("0123456789abcde", number));
}

RE_TEST(digest_unknown_is_distinct_from_zero) {
  const Digest unknown = Digest::unknown();
  Digest::Value zeros{};
  const Digest zero(zeros);
  RE_CHECK(!unknown.known());
  RE_CHECK(zero.known());
  RE_CHECK(unknown != zero);
  RE_CHECK_EQ(unknown.to_string(), std::string("unknown"));
  RE_CHECK_EQ(zero.to_string(), std::string(64, '0'));
}

RE_TEST(domain_separation_changes_the_digest) {
  const std::array<std::uint8_t, 32> first = sha256_domain("resource-envelope/a", std::string_view("payload"));
  const std::array<std::uint8_t, 32> second = sha256_domain("resource-envelope/b", std::string_view("payload"));
  RE_CHECK(first != second);
  const std::array<std::uint8_t, 32> repeated = sha256_domain("resource-envelope/a", std::string_view("payload"));
  RE_CHECK(first == repeated);
}

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
