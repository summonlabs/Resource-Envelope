#ifndef RESOURCE_ENVELOPE_DECISION_HPP
#define RESOURCE_ENVELOPE_DECISION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "resource_envelope/digest.hpp"
#include "resource_envelope/dimension.hpp"
#include "resource_envelope/envelope.hpp"
#include "resource_envelope/export.hpp"
#include "resource_envelope/quantity.hpp"
#include "resource_envelope/status.hpp"
#include "resource_envelope/result.hpp"
#include "resource_envelope/time.hpp"

namespace resource_envelope {

// ---------------------------------------------------------------------------
// Evaluation outcome
// ---------------------------------------------------------------------------
// `Rejected` is a definite determination that the request must not proceed.
// `Indeterminate` is the absence of a determination: some required input is not
// known. The two are distinct outcomes because conflating them would let missing
// data read as an admission decision.
enum class Outcome : std::uint8_t {
  Granted = 0,
  Denied = 1,
  Indeterminate = 2,
};

RESOURCE_ENVELOPE_API const char* to_string(Outcome outcome) noexcept;

// Per-dimension determination. `NotDeclared` is distinct from `NotRequested`: the
// first means the envelope declares no constraint for a dimension the caller asked
// about, the second means the caller asked nothing about this dimension.
enum class DimensionOutcome : std::uint8_t {
  Satisfied = 0,
  Denied = 1,
  Indeterminate = 2,
  NotDeclared = 3,
  NotRequested = 4,
};

RESOURCE_ENVELOPE_API const char* to_string(DimensionOutcome outcome) noexcept;

// Canonical dimension report. Every field that the runtime could not determine is
// marked by its own explicit flag; none of them defaults to zero.
struct RESOURCE_ENVELOPE_API DimensionResult {
  DimensionKind kind = DimensionKind::CountRackPositions;
  DimensionOutcome outcome = DimensionOutcome::NotRequested;

  std::optional<Nanounits> limit;
  Nanounits reserved = 0;
  // Present means the committed figure used by the arithmetic is known. Absent
  // means it is unknown and no residual can be claimed.
  std::optional<Nanounits> committed;
  // The committed figure that was actually available to the arithmetic. It differs
  // from `committed` when a quantum floor applies, and it is always reported so the
  // arithmetic is reproducible from the record.
  std::optional<Nanounits> effective_committed;
  // Present only when the residual is exactly determined: a known limit and a known
  // effective committed quantity. `residual < 0` is impossible; an over-committed
  // dimension reports `over_committed` instead, because a negative residual would
  // silently look like available headroom.
  std::optional<Nanounits> residual;
  bool over_committed = false;
  Nanounits over_committed_by = 0;

  Nanounits requested = 0;
  std::uint32_t principals = 1;

  // Present only when the residual is known.
  std::optional<Nanounits> headroom_after;

  std::optional<Nanounits> observed;
  bool observation_present = false;

  // Exclusive dimensions.
  std::string required_class;
  std::string held_class;
  UnitCount holders = 0;

  // NonConsumable dimensions.
  std::optional<Nanounits> required_threshold;
  std::optional<Nanounits> declared_threshold;
  std::optional<Nanounits> shortfall;

  // Redundancy: desired level, declared level, and whether an operational spare was
  // actually evidenced.
  std::uint32_t desired_level = 0;
  std::uint32_t declared_level = 0;
  std::optional<bool> operational_spare;

  Nanounits quantum = 0;
  std::optional<Nanounits> quantized_quantity;

  // Secondary failures observed while evaluating this dimension. They never
  // displace the primary reason but are preserved as evidence.
  std::vector<StatusCode> secondary;
};

// ---------------------------------------------------------------------------
// Evaluation inputs
// ---------------------------------------------------------------------------
struct RESOURCE_ENVELOPE_API EvidenceSet {
  std::vector<Evidence> items;
};

// How a facility-scope envelope (when present) combines with the principal
// envelope.
enum class FacilityComposition : std::uint8_t {
  // Evaluate the principal envelope alone.
  PrincipalOnly = 0,
  // Evaluate principal and facility envelopes and take the most restrictive
  // outcome. Each is evaluated independently, so no dimension is ever re-derived
  // from a merged synthetic limit.
  ConjunctiveWithFacility = 1,
};

RESOURCE_ENVELOPE_API const char* to_string(FacilityComposition composition) noexcept;
RESOURCE_ENVELOPE_API bool facility_composition_from_string(std::string_view text,
                                                           FacilityComposition& out) noexcept;

struct RESOURCE_ENVELOPE_API EvaluationRequest {
  EnvelopeScope scope;
  std::string idempotency_key;

  Timestamp at = 0;
  std::optional<std::uint64_t> expected_envelope_revision;
  std::optional<Digest> expected_envelope_digest;

  IdentitySnapshot identity;
  ServiceClassResolution service_class;
  PolicyResolution policy;

  // Supported-class extension point. Resource Envelope matches compatibility
  // classes as opaque tokens; this field declares which classes are acceptable for
  // this request, and an empty value means "no extension requested".
  std::vector<std::string> accepted_classes;

  std::vector<DimensionRequest> dimensions;
  EvidenceSet evidence;

  FacilityComposition facility_composition = FacilityComposition::PrincipalOnly;
  // Required when facility_composition is ConjunctiveWithFacility.
  std::string facility_envelope_id;
};

// ---------------------------------------------------------------------------
// Evaluation result
// ---------------------------------------------------------------------------
// Refusal stages are ordered and fixed. When several failures coexist the earliest
// stage decides the primary reason; every later failure is kept as secondary
// evidence. Ordinal values are part of the contract.
enum class RefusalStage : std::uint8_t {
  None = 0,
  RequestValidation = 1,
  EnvelopePresence = 2,
  ScopeMatch = 3,
  LineageIntegrity = 4,
  EnvelopeComposition = 5,
  EnvelopeCurrent = 6,
  GenerationBinding = 7,
  EffectiveWindow = 8,
  RequestedRevision = 9,
  ContextBinding = 10,
  ProviderAvailability = 11,
  ReplayResolution = 12,
  Arithmetic = 13,
  DimensionConstraint = 14,
};

RESOURCE_ENVELOPE_API const char* to_string(RefusalStage stage) noexcept;
RESOURCE_ENVELOPE_API bool refusal_stage_less(RefusalStage a, RefusalStage b) noexcept;

struct RESOURCE_ENVELOPE_API EvaluationResult {
  Outcome outcome = Outcome::Indeterminate;
  StatusCode reason = StatusCode::InvalidArgument;
  RefusalStage stage = RefusalStage::RequestValidation;
  // The dimension that blocked or constrained the request, when the refusal was
  // attributable to one dimension.
  std::optional<DimensionKind> blocking_dimension;
  // Stable identity of the grant: the SHA-256 digest of the canonical decision
  // record. Callers fence on this value rather than on a timestamp.
  Digest decision_digest;
  std::uint64_t decision_sequence = 0;

  // Authority actually used for this determination.
  std::string envelope_id;
  std::uint64_t envelope_revision = 0;
  Digest envelope_digest;
  std::uint64_t control_epoch = 0;

  Digest request_digest;
  Digest evidence_digest;

  std::vector<DimensionResult> dimensions;
  std::vector<StatusCode> secondary;
};

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_DECISION_HPP
