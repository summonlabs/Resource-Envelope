#include "support/testing.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/digest.hpp"
#include "resource_envelope/quantity.hpp"
#include "resource_envelope/record.hpp"
#include "resource_envelope/store.hpp"
#include "resource_envelope/text.hpp"

using namespace resource_envelope;

namespace {

// Every decoder in the runtime is fed hostile shapes: truncation at every prefix, a
// single flipped byte, and an over-declared length. A decoder that accepted any of
// these would let a damaged store or a forged record become authority.
template <typename Value>
void sweep_corruption(const Value& value, const std::function<std::vector<std::uint8_t>(const Value&)>& encode,
                      const std::function<Result<Value>(std::span<const std::uint8_t>)>& decode,
                      const char* what) {
  const std::vector<std::uint8_t> canonical = encode(value);
  RE_REQUIRE(!canonical.empty());
  // Truncation at every prefix must be refused.
  for (std::size_t length = 0; length < canonical.size(); ++length) {
    const std::vector<std::uint8_t> truncated(canonical.begin(),
                                              canonical.begin() + static_cast<std::ptrdiff_t>(length));
    if (decode(truncated).ok()) {
      ::testing::report_failure(__FILE__, __LINE__,
                               std::string(what) + " accepted a truncated payload of " +
                                   std::to_string(length) + " bytes");
      return;
    }
  }
  // A single flipped byte at every position must be refused or produce a different
  // value; producing the same value would mean the encoding is not injective.
  for (std::size_t index = 0; index < canonical.size(); ++index) {
    std::vector<std::uint8_t> flipped = canonical;
    flipped[index] = static_cast<std::uint8_t>(flipped[index] ^ 0xFFU);
    const auto decoded = decode(flipped);
    if (decoded.ok() && encode(decoded.value()) == canonical) {
      ::testing::report_failure(__FILE__, __LINE__,
                               std::string(what) + " accepted a corruption at byte " +
                                   std::to_string(index) + " without changing the value");
      return;
    }
  }
}

Envelope sample_envelope() {
  Envelope envelope;
  envelope.id = "tenant-a-envelope";
  envelope.revision = 3U;
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = "tenant-a";
  envelope.scope.identity.generation = 9U;
  envelope.site_id = "site-one";
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "operator";
  envelope.provenance.declared_at = 1767225600000000000LL;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = 100ULL * kNanounitsPerUnit;
  power.reserved = 5ULL * kNanounitsPerUnit;
  power.quantum = 5ULL * kNanounitsPerUnit;
  envelope.dimensions.push_back(power);
  return envelope;
}

}  // namespace

RE_TEST(envelope_decoder_survives_truncation_and_bit_flips) {
  sweep_corruption<Envelope>(
      sample_envelope(), [](const Envelope& value) { return canonical_envelope(value); },
      [](std::span<const std::uint8_t> bytes) { return decode_envelope(bytes); }, "decode_envelope");
}

RE_TEST(decision_record_decoder_survives_truncation_and_bit_flips) {
  DecisionRecord record;
  record.decision_id = "decision-1";
  record.idempotency_key = "authorize-1";
  record.outcome = Outcome::Denied;
  record.reason = StatusCode::RejectedInsufficientResidual;
  record.stage = RefusalStage::DimensionConstraint;
  record.envelope_id = "tenant-a-envelope";
  record.envelope_revision = 3U;
  record.evaluated_at = 1767225600000000000LL;
  record.sequence = 12U;
  record.decision_digest = compute_decision_digest(record);
  sweep_corruption<DecisionRecord>(
      record, [](const DecisionRecord& value) { return canonical_decision_record(value); },
      [](std::span<const std::uint8_t> bytes) { return decode_decision_record(bytes); },
      "decode_decision_record");
}

RE_TEST(usage_delta_decoder_survives_truncation_and_bit_flips) {
  UsageDelta delta;
  delta.idempotency_key = "usage-1";
  delta.envelope_id = "tenant-a-envelope";
  delta.kind = DimensionKind::PowerDrawWatts;
  delta.delta = 25;
  delta.source = "facility-capacity";
  delta.sequence = 4U;
  delta.entry_id = "usage-0004";
  sweep_corruption<UsageDelta>(
      delta, [](const UsageDelta& value) { return canonical_usage_delta(value); },
      [](std::span<const std::uint8_t> bytes) { return decode_usage_delta(bytes); }, "decode_usage_delta");
}

RE_TEST(journal_entry_decoder_survives_truncation_and_bit_flips) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::EnvelopeDeclaration;
  entry.sequence = 1U;
  EnvelopeDeclarationEntry declaration;
  declaration.envelope = sample_envelope();
  declaration.idempotency_key = "declare-1";
  declaration.declared_at = 1767225600000000000LL;
  entry.declaration = declaration;
  sweep_corruption<JournalEntry>(
      entry, [](const JournalEntry& value) { return canonical_journal_entry(value); },
      [](std::span<const std::uint8_t> bytes) { return decode_journal_entry(bytes); },
      "decode_journal_entry");
}

RE_TEST(manifest_decoder_refuses_impossible_combinations) {
  Manifest manifest;
  manifest.layout_version = kStoreLayoutVersion;
  manifest.epoch = 7U;
  manifest.state_epoch = 7U;
  manifest.snapshot_segment = 5U;
  manifest.snapshot_frames = 1U;
  manifest.frames_committed = 4U;
  manifest.state_sequence = 4U;
  const std::vector<std::uint8_t> canonical = canonical_manifest(manifest);
  RE_REQUIRE(decode_manifest(canonical).ok());
  const std::vector<std::uint8_t> trailing = [&canonical] {
    std::vector<std::uint8_t> copy = canonical;
    copy.push_back(0U);
    return copy;
  }();
  RE_CHECK(!decode_manifest(trailing).ok());
  for (std::size_t length = 0; length < canonical.size(); ++length) {
    const std::vector<std::uint8_t> truncated(canonical.begin(),
                                              canonical.begin() + static_cast<std::ptrdiff_t>(length));
    RE_CHECK(!decode_manifest(truncated).ok());
  }
}

RE_TEST(quantity_decoder_refuses_negative_and_absurd_values) {
  Nanounits value = 0;
  RE_CHECK(parse_quantity("18446744073709551615", value) == false);
  RE_CHECK(!parse_quantity("99999999999999999999999", value));
  RE_CHECK(!parse_quantity("0.0000000001", value));
  // 9007199254 is above kQuantityReasonableMax / 1e9, so the declaration is out of
  // range and is refused rather than saturated.
  // 9007199254 whole base units scaled by 1e9 is 9.007e18 nanounits, far above the
  // declared bound of 2^53 nanounits, so the declaration is refused rather than
  // saturated. The largest accepted whole part is floor(2^53 / 1e9) = 9007199.
  RE_CHECK(!parse_quantity("9007199254", value));
  RE_CHECK(!parse_quantity("9007199254.740993", value));
  RE_CHECK(parse_quantity("9007199.254740992", value));
  RE_CHECK_EQ(value, kQuantityReasonableMax);
  RE_CHECK(!parse_quantity("9007200", value));
  RE_CHECK(!make_quantity(std::numeric_limits<std::uint64_t>::max(), 0U, value));
}

RE_TEST(identifier_validation_is_total_for_hostile_text) {
  const std::string hostile[] = {"", "-", "--", "-a-", "a--b", "CON", "nul", "com1",
                                 "..", ".", "a.", "a ", " a", "a/b", "a\\b",
                                 std::string(1024, 'a'), std::string("\xff\xfe", 2),
                                 std::string("\xc0\xaf", 2), std::string("a\0b", 3)};
  for (const std::string& text : hostile) {
    const TextDefect defect = validate_identifier(text, IdentifierUse::Canonical);
    RE_CHECK(defect != TextDefect::None);
    RE_CHECK(std::string(to_string(defect)) != std::string("None"));
  }
}

RE_TEST(segment_file_names_are_generated_and_parsed_exactly) {
  for (const std::uint64_t epoch : {0ULL, 1ULL, 42ULL, 9999999999999999999ULL}) {
    const std::string name = segment_file_name(epoch);
    std::uint64_t parsed = 0U;
    RE_REQUIRE(parse_segment_file_name(name, parsed));
    RE_CHECK_EQ(parsed, epoch);
    RE_CHECK(!is_reserved_device_name(name));
  }
  std::uint64_t ignored = 0U;
  RE_CHECK(!parse_segment_file_name("../escape", ignored));
  RE_CHECK(!parse_segment_file_name("segment-000000000000000000000.renv", ignored));
  RE_CHECK(!parse_segment_file_name("segment-0000000000000000000x.renv", ignored));
  RE_CHECK(!parse_segment_file_name("segment-00000000000000000001.txt", ignored));
  RE_CHECK(!parse_segment_file_name("", ignored));
}

RE_TEST(crc32_matches_the_published_check_value) {
  // The IEEE 802.3 check value for "123456789".
  const std::string text = "123456789";
  const std::uint32_t checksum =
      crc32_ieee(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size()));
  RE_CHECK_EQ(checksum, 0xCBF43926U);
  const std::span<const std::uint8_t> empty;
  RE_CHECK_EQ(crc32_ieee(empty), 0U);
}
