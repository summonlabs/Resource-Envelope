#include "resource_envelope/record.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "resource_envelope/bytes.hpp"
#include "resource_envelope/canonical.hpp"
#include "resource_envelope/dimension.hpp"
#include "resource_envelope/envelope.hpp"
#include "resource_envelope/service.hpp"
#include "resource_envelope/status.hpp"
#include "resource_envelope/text.hpp"

namespace resource_envelope {
namespace {

// Source tokens and reasons are bounded so that no decoded record can allocate an
// unbounded string from a hostile payload.
constexpr std::size_t kMaxSourceTokenBytes = 256U;

void encode_optional_kind(ByteWriter& writer, const std::optional<DimensionKind>& kind) {
  writer.bool_value(kind.has_value());
  if (kind.has_value()) writer.u8(static_cast<std::uint8_t>(*kind));
}

bool decode_optional_kind(ByteReader& reader, std::optional<DimensionKind>& out) {
  const bool present = reader.bool_value();
  if (!reader.ok()) return false;
  if (!present) {
    out.reset();
    return true;
  }
  const std::uint8_t raw = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<DimensionKind>(raw))) return false;
  out = static_cast<DimensionKind>(raw);
  return true;
}

void encode_status_codes(ByteWriter& writer, const std::vector<StatusCode>& codes) {
  writer.u32(static_cast<std::uint32_t>(codes.size()));
  for (const StatusCode code : codes) writer.u32(static_cast<std::uint32_t>(code));
}

bool decode_status_codes(ByteReader& reader, std::vector<StatusCode>& out) {
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) return false;
  if (count > kMaxEvidencePerRequest) return false;
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint32_t raw = reader.u32();
    if (!reader.ok()) return false;
    out.push_back(static_cast<StatusCode>(raw));
  }
  return true;
}

void encode_identity(ByteWriter& writer, const IdentityRef& identity) {
  writer.text(identity.id);
  writer.u64(identity.generation);
}

bool decode_identity(ByteReader& reader, IdentityRef& out) {
  out.id = reader.text(kIdentifierMaxLength);
  out.generation = reader.u64();
  return reader.ok();
}

}  // namespace

void encode_decision_record(ByteWriter& writer, const DecisionRecord& record) {
  writer.u16(kCanonicalFormatVersion);
  writer.text(record.decision_id);
  writer.text(record.idempotency_key);
  encode_digest(writer, record.request_digest);
  encode_digest(writer, record.evidence_digest);
  writer.u8(static_cast<std::uint8_t>(record.outcome));
  writer.u32(static_cast<std::uint32_t>(record.reason));
  writer.u8(static_cast<std::uint8_t>(record.stage));
  encode_optional_kind(writer, record.blocking_dimension);
  writer.text(record.envelope_id);
  writer.u64(record.envelope_revision);
  encode_digest(writer, record.envelope_digest);
  writer.u64(record.control_epoch);
  encode_digest(writer, record.authority_state_digest);
  writer.u8(static_cast<std::uint8_t>(record.scope.kind));
  encode_identity(writer, record.scope.identity);
  encode_identity(writer, record.identity);
  writer.bool_value(record.identity_checked);
  encode_digest(writer, record.identity_digest);
  encode_digest(writer, record.service_class_digest);
  encode_digest(writer, record.policy_digest);
  writer.i64(record.evaluated_at);
  writer.u32(static_cast<std::uint32_t>(record.dimensions.size()));
  for (const DimensionResult& item : record.dimensions) encode_dimension_result(writer, item);
  encode_status_codes(writer, record.secondary);
  encode_digest(writer, record.grant_digest);
  writer.u64(record.sequence);
}

Bytes canonical_decision_record(const DecisionRecord& record) {
  ByteWriter writer;
  encode_decision_record(writer, record);
  return writer.take();
}

Digest compute_decision_digest(const DecisionRecord& record) noexcept {
  // The digest covers the record with its own digest field cleared, so it is stable
  // under re-computation and changes whenever any authority input changes.
  DecisionRecord copy = record;
  copy.decision_digest = Digest::unknown();
  return Digest(sha256_domain(kDomainDecision, canonical_decision_record(copy)));
}

Bytes canonical_authority_verdict(const AuthorityVerdict& verdict) {
  ByteWriter writer;
  writer.u16(kCanonicalFormatVersion);
  encode_authority_verdict(writer, verdict);
  return writer.take();
}

void encode_authority_verdict(ByteWriter& writer, const AuthorityVerdict& verdict) {
  writer.u8(static_cast<std::uint8_t>(verdict.state));
  writer.text(verdict.detail);
  writer.text(verdict.envelope_id);
  writer.u64(verdict.granted_revision);
  writer.u64(verdict.current_revision);
  writer.u64(verdict.granted_control_epoch);
  writer.u64(verdict.current_control_epoch);
}

const char* to_string(AuthorityState state) noexcept {
  switch (state) {
    case AuthorityState::Valid: return "Valid";
    case AuthorityState::EnvelopeMissing: return "EnvelopeMissing";
    case AuthorityState::EnvelopeRevisionAdvanced: return "EnvelopeRevisionAdvanced";
    case AuthorityState::EnvelopeDigestChanged: return "EnvelopeDigestChanged";
    case AuthorityState::EnvelopeExpired: return "EnvelopeExpired";
    case AuthorityState::ControlEpochFenced: return "ControlEpochFenced";
    case AuthorityState::AuthorityStateChanged: return "AuthorityStateChanged";
    case AuthorityState::IdentityBindingChanged: return "IdentityBindingChanged";
    case AuthorityState::ContextBindingChanged: return "ContextBindingChanged";
    case AuthorityState::NotAGrant: return "NotAGrant";
  }
  return "UnknownAuthorityState";
}

Result<DecisionRecord> decode_decision_record(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalDecisionBytes) {
    return Status(StatusCode::OutOfRange, "decision record exceeds the canonical size bound");
  }
  ByteReader reader(data);
  DecisionRecord record;
  const std::uint16_t version = reader.u16();
  if (!reader.ok() || version != kCanonicalFormatVersion) {
    return Status(StatusCode::UnsupportedVersion, "decision record carries an unsupported format version");
  }
  record.decision_id = reader.text(kDecisionRefMaxLength);
  record.idempotency_key = reader.text(kIdempotencyKeyMaxLength);
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "decision record header is truncated");
  if (!decode_digest(reader, record.request_digest)) return Status(StatusCode::TruncatedPayload, "request digest is truncated");
  if (!decode_digest(reader, record.evidence_digest)) return Status(StatusCode::TruncatedPayload, "evidence digest is truncated");
  const std::uint8_t outcome = reader.u8();
  if (!reader.ok() || outcome > static_cast<std::uint8_t>(Outcome::Indeterminate)) {
    return Status(StatusCode::InvalidEnumValue, "decision outcome is not a defined value");
  }
  record.outcome = static_cast<Outcome>(outcome);
  record.reason = static_cast<StatusCode>(reader.u32());
  const std::uint8_t stage = reader.u8();
  if (!reader.ok() || stage > static_cast<std::uint8_t>(RefusalStage::DimensionConstraint)) {
    return Status(StatusCode::InvalidEnumValue, "decision refusal stage is not a defined value");
  }
  record.stage = static_cast<RefusalStage>(stage);
  if (!decode_optional_kind(reader, record.blocking_dimension)) {
    return Status(StatusCode::InvalidEnumValue, "blocking dimension is not a defined value");
  }
  record.envelope_id = reader.text(kIdentifierMaxLength);
  record.envelope_revision = reader.u64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "decision envelope reference is truncated");
  if (!decode_digest(reader, record.envelope_digest)) return Status(StatusCode::TruncatedPayload, "envelope digest is truncated");
  record.control_epoch = reader.u64();
  if (!decode_digest(reader, record.authority_state_digest)) {
    return Status(StatusCode::TruncatedPayload, "authority state digest is truncated");
  }
  const std::uint8_t scope_kind = reader.u8();
  if (!reader.ok() || scope_kind > static_cast<std::uint8_t>(EnvelopeScopeKind::Facility)) {
    return Status(StatusCode::InvalidEnumValue, "decision scope kind is not a defined value");
  }
  record.scope.kind = static_cast<EnvelopeScopeKind>(scope_kind);
  if (!decode_identity(reader, record.scope.identity)) {
    return Status(StatusCode::TruncatedPayload, "decision scope identity is truncated");
  }
  if (!decode_identity(reader, record.identity)) {
    return Status(StatusCode::TruncatedPayload, "decision identity is truncated");
  }
  record.identity_checked = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "identity confirmation flag is truncated");
  if (!decode_digest(reader, record.identity_digest)) return Status(StatusCode::TruncatedPayload, "identity digest is truncated");
  if (!decode_digest(reader, record.service_class_digest)) return Status(StatusCode::TruncatedPayload, "service class digest is truncated");
  if (!decode_digest(reader, record.policy_digest)) return Status(StatusCode::TruncatedPayload, "policy digest is truncated");
  record.evaluated_at = reader.i64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "decision bindings are truncated");

  const std::uint32_t dimension_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "decision dimension count is truncated");
  if (dimension_count > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "decision carries more dimensions than permitted");
  }
  for (std::uint32_t index = 0; index < dimension_count; ++index) {
    DimensionResult item;
    if (!decode_dimension_result(reader, item)) {
      return Status(StatusCode::TruncatedPayload, "decision dimension result is truncated or invalid");
    }
    record.dimensions.push_back(std::move(item));
  }
  if (!decode_status_codes(reader, record.secondary)) {
    return Status(StatusCode::TruncatedPayload, "decision secondary codes are truncated");
  }
  if (!decode_digest(reader, record.grant_digest)) return Status(StatusCode::TruncatedPayload, "grant digest is truncated");
  record.sequence = reader.u64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "decision sequence is truncated");

  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "decision record did not decode exactly");
  }
  // The digest is not carried in the payload: it is a pure function of the record, so it is
  // restored here rather than written twice. Every reader of a decoded record - a snapshot, a
  // journal frame, a history view, a lookup or a verification - therefore reports the digest that
  // identifies the record, instead of a reader having to know which of them recompute it.
  record.decision_digest = compute_decision_digest(record);
  return record;
}

Bytes canonical_usage_delta(const UsageDelta& delta) {
  ByteWriter writer;
  writer.u16(kCanonicalFormatVersion);
  writer.text(delta.idempotency_key);
  encode_digest(writer, delta.payload_digest);
  writer.text(delta.envelope_id);
  writer.u64(delta.envelope_revision);
  writer.text(delta.principal);
  writer.u8(static_cast<std::uint8_t>(delta.kind));
  writer.i64(delta.delta);
  writer.text(delta.compatibility_class);
  writer.i64(delta.recorded_at);
  writer.text(delta.source);
  writer.u64(delta.sequence);
  writer.text(delta.entry_id);
  return writer.take();
}

Result<UsageDelta> decode_usage_delta(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalRecordBytes) {
    return Status(StatusCode::OutOfRange, "usage delta exceeds the canonical size bound");
  }
  ByteReader reader(data);
  UsageDelta delta;
  const std::uint16_t version = reader.u16();
  if (!reader.ok() || version != kCanonicalFormatVersion) {
    return Status(StatusCode::UnsupportedVersion, "usage delta carries an unsupported format version");
  }
  delta.idempotency_key = reader.text(kIdempotencyKeyMaxLength);
  if (!decode_digest(reader, delta.payload_digest)) return Status(StatusCode::TruncatedPayload, "payload digest is truncated");
  delta.envelope_id = reader.text(kIdentifierMaxLength);
  delta.envelope_revision = reader.u64();
  delta.principal = reader.text(kMaxPrincipalTokenLength);
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
    return Status(StatusCode::InvalidEnumValue, "usage delta dimension is not a defined value");
  }
  delta.kind = static_cast<DimensionKind>(kind);
  delta.delta = reader.i64();
  delta.compatibility_class = reader.text(kCompatibilityClassMaxLength);
  delta.recorded_at = reader.i64();
  delta.source = reader.text(kMaxSourceTokenBytes);
  delta.sequence = reader.u64();
  delta.entry_id = reader.text(kDecisionRefMaxLength);
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "usage delta is truncated");
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "usage delta did not decode exactly");
  }
  return delta;
}

Digest usage_payload_digest(const UsageDelta& delta) noexcept {
  // The payload digest covers the substance of the claim and not the transport
  // fields, so a retry that carries the same claim is recognised as a replay while
  // a retry that changes the amount is recognised as a conflict.
  UsageDelta copy = delta;
  copy.idempotency_key.clear();
  copy.sequence = 0;
  copy.entry_id.clear();
  copy.payload_digest = Digest::unknown();
  return Digest(sha256_domain(kDomainIdempotency, canonical_usage_delta(copy)));
}

Digest usage_claim_digest(const UsageDelta& delta) noexcept {
  UsageDelta copy = delta;
  copy.idempotency_key.clear();
  copy.envelope_revision = 0;
  copy.sequence = 0;
  copy.entry_id.clear();
  copy.payload_digest = Digest::unknown();
  return Digest(sha256_domain(kDomainIdempotency, canonical_usage_delta(copy)));
}

Bytes canonical_observation_entry(const ObservationEntry& entry) {
  ByteWriter writer;
  writer.u16(kCanonicalFormatVersion);
  writer.text(entry.envelope_id);
  writer.u64(entry.envelope_revision);
  writer.u8(static_cast<std::uint8_t>(entry.kind));
  writer.u8(static_cast<std::uint8_t>(entry.status));
  writer.u64(entry.value);
  writer.text(entry.principal);
  writer.i64(entry.observed_at);
  writer.text(entry.source);
  writer.u64(entry.sequence);
  writer.text(entry.entry_id);
  return writer.take();
}

Result<ObservationEntry> decode_observation_entry(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalRecordBytes) {
    return Status(StatusCode::OutOfRange, "observation entry exceeds the canonical size bound");
  }
  ByteReader reader(data);
  ObservationEntry entry;
  const std::uint16_t version = reader.u16();
  if (!reader.ok() || version != kCanonicalFormatVersion) {
    return Status(StatusCode::UnsupportedVersion, "observation entry carries an unsupported format version");
  }
  entry.envelope_id = reader.text(kIdentifierMaxLength);
  entry.envelope_revision = reader.u64();
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
    return Status(StatusCode::InvalidEnumValue, "observation dimension is not a defined value");
  }
  entry.kind = static_cast<DimensionKind>(kind);
  const std::uint8_t status = reader.u8();
  if (!reader.ok() || status > static_cast<std::uint8_t>(MeasureStatus::Measured)) {
    return Status(StatusCode::InvalidEnumValue, "observation status is not a defined value");
  }
  entry.status = static_cast<MeasureStatus>(status);
  entry.value = reader.u64();
  entry.principal = reader.text(kMaxPrincipalTokenLength);
  entry.observed_at = reader.i64();
  entry.source = reader.text(kMaxSourceTokenBytes);
  entry.sequence = reader.u64();
  entry.entry_id = reader.text(kDecisionRefMaxLength);
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "observation entry is truncated");
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "observation entry did not decode exactly");
  }
  return entry;
}

}  // namespace resource_envelope
