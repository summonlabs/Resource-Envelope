#ifndef RESOURCE_ENVELOPE_RECORD_HPP
#define RESOURCE_ENVELOPE_RECORD_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "resource_envelope/bytes.hpp"
#include "resource_envelope/decision.hpp"
#include "resource_envelope/digest.hpp"
#include "resource_envelope/envelope.hpp"
#include "resource_envelope/export.hpp"
#include "resource_envelope/result.hpp"
#include "resource_envelope/status.hpp"
#include "resource_envelope/time.hpp"

namespace resource_envelope {

inline constexpr std::size_t kDecisionRefMaxLength = 64;
inline constexpr std::size_t kIdempotencyKeyMaxLength = 64;

// ---------------------------------------------------------------------------
// Durable decision record
// ---------------------------------------------------------------------------
// The record captures exactly the authority the determination consumed: the
// envelope revision and digest, the store control epoch, the state digest of the
// authority that answered, the request digest, the evidence digest, and the
// identity and context bindings that were checked.
//
// A later change to any of those values makes the record stale, and staleness is
// detectable from the record alone. Nothing in the record is derived from a clock
// or from process state.
struct RESOURCE_ENVELOPE_API DecisionRecord {
  std::string decision_id;
  std::string idempotency_key;
  Digest request_digest;
  Digest evidence_digest;

  Outcome outcome = Outcome::Indeterminate;
  StatusCode reason = StatusCode::InvalidArgument;
  RefusalStage stage = RefusalStage::RequestValidation;
  std::optional<DimensionKind> blocking_dimension;

  std::string envelope_id;
  std::uint64_t envelope_revision = 0;
  Digest envelope_digest;
  std::uint64_t control_epoch = 0;
  Digest authority_state_digest;

  // Bound identity and context. `identity_checked` distinguishes "the caller
  // confirmed this generation" from "no confirmation was required".
  EnvelopeScope scope;
  IdentityRef identity;
  bool identity_checked = false;
  Digest identity_digest;
  Digest service_class_digest;
  Digest policy_digest;

  Timestamp evaluated_at = 0;
  std::vector<DimensionResult> dimensions;
  std::vector<StatusCode> secondary;

  // Present only for a granted decision. A grant is a bounded statement about the
  // authority that produced it and carries no expiry of its own: it is invalidated
  // by a change to the authority it names.
  Digest grant_digest;

  // SHA-256 over the canonical decision-record encoding, excluding this field.
  Digest decision_digest;
  std::uint64_t sequence = 0;
};

RESOURCE_ENVELOPE_API Bytes canonical_decision_record(const DecisionRecord& record);
RESOURCE_ENVELOPE_API Result<DecisionRecord> decode_decision_record(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Digest compute_decision_digest(const DecisionRecord& record) noexcept;
RESOURCE_ENVELOPE_API void encode_decision_record(ByteWriter& writer, const DecisionRecord& record);

// Authority staleness classification. `Superseded` is reported for an ordinary
// newer revision; `Fenced` is reported when the record's control epoch is behind the
// store's current epoch, which means the authority it relied on was retired by an
// epoch advance rather than by an ordinary revision.
enum class AuthorityState : std::uint8_t {
  Valid = 0,
  EnvelopeMissing = 1,
  EnvelopeRevisionAdvanced = 2,
  EnvelopeDigestChanged = 3,
  EnvelopeExpired = 4,
  ControlEpochFenced = 5,
  AuthorityStateChanged = 6,
  IdentityBindingChanged = 7,
  ContextBindingChanged = 8,
  NotAGrant = 9,
};

RESOURCE_ENVELOPE_API const char* to_string(AuthorityState state) noexcept;

struct RESOURCE_ENVELOPE_API AuthorityVerdict {
  AuthorityState state = AuthorityState::Valid;
  std::string detail;
  std::string envelope_id;
  std::uint64_t granted_revision = 0;
  std::uint64_t current_revision = 0;
  std::uint64_t granted_control_epoch = 0;
  std::uint64_t current_control_epoch = 0;

  [[nodiscard]] bool valid() const noexcept { return state == AuthorityState::Valid; }
};

RESOURCE_ENVELOPE_API void encode_authority_verdict(ByteWriter& writer, const AuthorityVerdict& verdict);
RESOURCE_ENVELOPE_API Bytes canonical_authority_verdict(const AuthorityVerdict& verdict);

// ---------------------------------------------------------------------------
// Durable usage evidence
// ---------------------------------------------------------------------------
// Committed usage is persisted as an append-only journal of deltas rather than as
// an absolute counter. Replaying the journal is then a pure fold, so recovery
// reproduces the exact committed figures, and idempotent replay of a delta cannot
// double-count because each delta carries the key and payload digest that first
// accepted it.
struct RESOURCE_ENVELOPE_API UsageDelta {
  std::string idempotency_key;
  Digest payload_digest;
  std::string envelope_id;
  std::uint64_t envelope_revision = 0;
  std::string principal;
  DimensionKind kind = DimensionKind::CountRackPositions;
  std::int64_t delta = 0;
  std::string compatibility_class;
  Timestamp recorded_at = 0;
  std::string source;
  std::uint64_t sequence = 0;
  std::string entry_id;
};

RESOURCE_ENVELOPE_API Bytes canonical_usage_delta(const UsageDelta& delta);
RESOURCE_ENVELOPE_API Result<UsageDelta> decode_usage_delta(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Digest usage_payload_digest(const UsageDelta& delta) noexcept;

// Observed measurements are recorded for explainability and trend reporting only.
// They are never an input to admission arithmetic and never become committed usage.
struct RESOURCE_ENVELOPE_API ObservationEntry {
  std::string envelope_id;
  std::uint64_t envelope_revision = 0;
  DimensionKind kind = DimensionKind::CountRackPositions;
  MeasureStatus status = MeasureStatus::Unknown;
  std::uint64_t value = 0;
  std::string principal;
  Timestamp observed_at = 0;
  std::string source;
  std::uint64_t sequence = 0;
  std::string entry_id;
};

RESOURCE_ENVELOPE_API Bytes canonical_observation_entry(const ObservationEntry& entry);
RESOURCE_ENVELOPE_API Result<ObservationEntry> decode_observation_entry(std::span<const std::uint8_t> data);

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_RECORD_HPP
