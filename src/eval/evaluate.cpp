#include "resource_envelope/service.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <vector>

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/text.hpp"

namespace resource_envelope {
namespace {

// Absolute safety ceiling for the internal arithmetic. Every declared input is
// already bounded by kQuantityReasonableMax, so this guard can only be reached by
// repeated accumulation, and reaching it produces a refusal rather than a silent
// wrap.
constexpr Nanounits kArithmeticCeiling = std::numeric_limits<Nanounits>::max();

bool saturated_add(Nanounits left, Nanounits right, Nanounits& out) noexcept {
  if (left > kArithmeticCeiling - right) return false;
  out = left + right;
  return true;
}

Nanounits saturated_sum(Nanounits left, Nanounits right) noexcept {
  Nanounits out = 0;
  if (!saturated_add(left, right, out)) return kArithmeticCeiling;
  return out;
}

bool all_zero(const std::vector<std::uint64_t>& counts) noexcept {
  for (const std::uint64_t count : counts) {
    if (count != 0) return false;
  }
  return true;
}

std::uint64_t first_non_zero_index(const std::vector<std::uint64_t>& counts) noexcept {
  for (std::size_t index = 0; index < counts.size(); ++index) {
    if (counts[index] != 0) return static_cast<std::uint64_t>(index);
  }
  return 0;
}

// Committed usage for one dimension, resolved from durable state and then from
// caller evidence. Observation is never consulted here: a measurement cannot become
// a consumption figure merely because it exists.
struct CommittedView {
  bool known = false;
  Nanounits value = 0;
  bool overflow = false;
};

// One evaluable envelope together with the authority it is being read from.
struct Target {
  const Envelope* envelope = nullptr;
  bool is_current = true;
  std::uint64_t stored_sequence = 0;
  Digest record_digest;
};

struct Issue {
  RefusalStage stage = RefusalStage::RequestValidation;
  StatusCode code = StatusCode::InvalidArgument;
  std::optional<DimensionKind> dimension;
  std::string detail;
};

struct EvaluationBuilder {
  EvaluationResult result;
  std::vector<Issue> issues;
  std::vector<Target> targets;
  const EvaluationRequest* request = nullptr;
  const CommittedMap* committed = nullptr;
  std::string committed_envelope_id;

  void note(RefusalStage stage, StatusCode code, std::optional<DimensionKind> dimension,
            std::string detail) {
    issues.push_back(Issue{stage, code, dimension, std::move(detail)});
  }
};

std::string issue_detail(const Issue& issue) { return issue.detail; }

void merge_secondary(EvaluationResult& target, const EvaluationResult& source) {
  for (const StatusCode code : source.secondary) {
    if (std::find(target.secondary.begin(), target.secondary.end(), code) == target.secondary.end()) {
      target.secondary.push_back(code);
    }
  }
}

// Validates the request structure before anything else is consulted, so a malformed
// input can never reach the arithmetic or produce a partial determination.
void validate_request(EvaluationBuilder& builder) {
  const EvaluationRequest& request = *builder.request;
  if (validate_identifier(request.scope.identity.id, IdentifierUse::Canonical) != TextDefect::None) {
    builder.note(RefusalStage::RequestValidation, StatusCode::InvalidIdentifier, std::nullopt,
                 "request scope identifier is not a canonical identifier");
  }
  if (!is_valid_timestamp(request.at)) {
    builder.note(RefusalStage::RequestValidation, StatusCode::OutOfRange, std::nullopt,
                 "evaluation timestamp is outside the representable RFC 3339 range");
  }
  if (request.expected_envelope_revision.has_value() && *request.expected_envelope_revision == 0) {
    builder.note(RefusalStage::RequestValidation, StatusCode::OutOfRange, std::nullopt,
                 "expected envelope revision zero is not a revision an envelope can carry");
  }
  if (request.idempotency_key.size() > kIdempotencyKeyMaxLength) {
    builder.note(RefusalStage::RequestValidation, StatusCode::OutOfRange, std::nullopt,
                 "idempotency key exceeds the permitted length");
  }
  if (request.dimensions.size() > kMaxDimensionsPerEnvelope) {
    builder.note(RefusalStage::RequestValidation, StatusCode::OutOfRange, std::nullopt,
                 "request declares more dimensions than permitted");
  }
  if (request.evidence.items.size() > kMaxEvidencePerRequest) {
    builder.note(RefusalStage::RequestValidation, StatusCode::OutOfRange, std::nullopt,
                 "request carries more evidence items than permitted");
  }
  if (request.facility_composition == FacilityComposition::ConjunctiveWithFacility &&
      request.facility_envelope_id.empty()) {
    builder.note(RefusalStage::RequestValidation, StatusCode::InvalidArgument, std::nullopt,
                 "conjunctive facility composition requires a facility envelope identifier");
  }

  std::set<DimensionKind> seen_dimensions;
  for (const DimensionRequest& item : request.dimensions) {
    const std::optional<DimensionKind> kind = item.kind;
    if (!is_valid(item.kind)) {
      builder.note(RefusalStage::RequestValidation, StatusCode::InvalidEnumValue, std::nullopt,
                   "request names a dimension kind that is not defined");
      continue;
    }
    if (!seen_dimensions.insert(item.kind).second) {
      builder.note(RefusalStage::RequestValidation, StatusCode::DuplicateDimension, kind,
                   "request names the same dimension twice");
    }
    if (item.principals == 0U || item.principals > kMaxRequestPrincipals) {
      builder.note(RefusalStage::RequestValidation, StatusCode::OutOfRange, kind,
                   "requested principal count is outside the permitted range");
    }
    if (item.quantity > kQuantityReasonableMax) {
      builder.note(RefusalStage::RequestValidation, StatusCode::OutOfRange, kind,
                   "requested quantity exceeds the representable envelope magnitude");
    }
    if (!item.compatibility_class.empty() &&
        validate_identifier(item.compatibility_class, IdentifierUse::Canonical) != TextDefect::None) {
      builder.note(RefusalStage::RequestValidation, StatusCode::InvalidIdentifier, kind,
                   "requested compatibility class is not a canonical identifier");
    }
    if (!item.principal.empty() &&
        validate_identifier(item.principal, IdentifierUse::Canonical) != TextDefect::None) {
      builder.note(RefusalStage::RequestValidation, StatusCode::InvalidIdentifier, kind,
                   "requested principal token is not a canonical identifier");
    }
  }

  std::set<DimensionKind> seen_evidence;
  for (const Evidence& item : request.evidence.items) {
    if (!is_valid(item.kind)) {
      builder.note(RefusalStage::RequestValidation, StatusCode::InvalidEnumValue, std::nullopt,
                   "evidence names a dimension kind that is not defined");
      continue;
    }
    if (!seen_evidence.insert(item.kind).second) {
      builder.note(RefusalStage::RequestValidation, StatusCode::DuplicateDimension, item.kind,
                   "request carries two evidence items for the same dimension");
    }
    if (!item.observation.has_value() && !item.committed.has_value() && !item.exclusive.has_value() &&
        !item.threshold.has_value()) {
      builder.note(RefusalStage::RequestValidation, StatusCode::InvalidArgument, item.kind,
                   "evidence item carries no observation, commitment, declaration or threshold");
    }
  }
  for (const std::string& token : request.accepted_classes) {
    if (validate_identifier(token, IdentifierUse::Canonical) != TextDefect::None) {
      builder.note(RefusalStage::RequestValidation, StatusCode::InvalidIdentifier, std::nullopt,
                   "accepted compatibility class is not a canonical identifier");
    }
  }
}

// Resolves committed usage for one dimension. Durable state is authoritative. A
// commitment declared on the envelope itself is used only when durable state holds
// no entry for the dimension at all, and a caller-supplied commitment is applied
// afterwards at the dimension level, so a later claim can never silently displace a
// recorded figure.
CommittedView resolve_committed(EvaluationBuilder& builder, const Target& target, DimensionKind kind) {
  CommittedView view;
  const std::string& envelope_id =
      builder.committed_envelope_id.empty() ? target.envelope->id : builder.committed_envelope_id;

  if (builder.committed != nullptr) {
    const auto lower =
        builder.committed->lower_bound(CommittedKey{envelope_id, kind, std::string(), std::string()});
    Nanounits total = 0;
    bool any = false;
    bool overflow = false;
    bool negative = false;
    for (auto iterator = lower; iterator != builder.committed->end(); ++iterator) {
      if (iterator->first.envelope_id != envelope_id || iterator->first.kind != kind) break;
      any = true;
      if (iterator->second.value < 0) {
        negative = true;
        continue;
      }
      if (!saturated_add(total, static_cast<Nanounits>(iterator->second.value), total)) overflow = true;
    }
    if (negative) {
      // A negative aggregate means recorded commitments were released below zero.
      // That is a defect in recorded evidence, not a value to clamp to zero.
      builder.note(RefusalStage::Arithmetic, StatusCode::IndeterminateUnknownCommitted, kind,
                   "recorded committed usage aggregates below zero and cannot be interpreted");
      return view;
    }
    if (overflow) {
      builder.note(RefusalStage::Arithmetic, StatusCode::RejectedAbsoluteBound, kind,
                   "recorded committed usage exceeds the representable magnitude");
      view.known = true;
      view.value = kArithmeticCeiling;
      view.overflow = true;
      return view;
    }
    if (any) {
      view.known = true;
      view.value = total;
      return view;
    }
  }

  const DimensionSpec* spec = target.envelope->find(kind);
  if (spec != nullptr && spec->committed.has_value()) {
    view.known = true;
    view.value = spec->committed->value;
  }
  return view;
}

// Quantization. The quantum is an alignment requirement on the consumption figures,
// and a request must itself be an exact multiple. On a misaligned input the runtime
// reports the aligned quantity instead of rounding silently, because rounding would
// either overstate or understate the authority being granted.
void apply_quantum(EvaluationBuilder& builder, DimensionKind kind, Nanounits quantum,
                   const std::optional<Nanounits>& effective_committed, const Nanounits* requested,
                   DimensionResult& item) {
  if (quantum == 0U) {
    item.effective_committed = effective_committed;
    if (requested != nullptr) item.quantized_quantity = *requested;
    return;
  }

  if (requested != nullptr) {
    const CheckedQuantity aligned = round_up_to_multiple(*requested, quantum);
    item.quantized_quantity = aligned.value;
    if (aligned.overflow) {
      builder.note(RefusalStage::Arithmetic, StatusCode::RejectedAbsoluteBound, kind,
                   "rounding the requested quantity to the declared alignment is not representable");
      item.secondary.push_back(StatusCode::RejectedAbsoluteBound);
    } else if (!is_multiple_of(*requested, quantum)) {
      item.outcome = DimensionOutcome::Denied;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedQuantization, kind,
                   "requested quantity is not an exact multiple of the dimension alignment");
    }
  }

  if (!effective_committed.has_value()) {
    item.effective_committed = std::nullopt;
    return;
  }
  const CheckedQuantity aligned = round_up_to_multiple(*effective_committed, quantum);
  if (aligned.overflow) {
    builder.note(RefusalStage::Arithmetic, StatusCode::RejectedAbsoluteBound, kind,
                 "rounding committed usage to the declared alignment is not representable");
    item.secondary.push_back(StatusCode::RejectedAbsoluteBound);
    item.effective_committed = std::nullopt;
    return;
  }
  item.effective_committed = aligned.value;
}

// Exclusive dimensions constrain which principal may hold the dimension, and a
// holder consumes the whole capacity. The class comparison is exact: Resource
// Envelope stores and compares compatibility classes and never ranks or normalises
// them, so it cannot decide that one class is a substitute for another.
void constrain_exclusive(EvaluationBuilder& builder, const DimensionRequest& request, const DimensionSpec& spec,
                         DimensionResult& item) {
  const DimensionKind kind = request.kind;
  item.required_class = request.compatibility_class;

  if (!spec.hard_limit.has_value()) {
    item.outcome = DimensionOutcome::Indeterminate;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::IndeterminateUnknownLimit, kind,
                 "the envelope bound for this exclusive dimension is unknown");
    return;
  }

  // The held class and the holder count come from evidence, because the class is
  // held by a principal outside this envelope.
  const Evidence* evidence = nullptr;
  for (const Evidence& candidate : builder.request->evidence.items) {
    if (candidate.kind == kind) {
      evidence = &candidate;
      break;
    }
  }
  bool held_known = false;
  std::string held_class;
  UnitCount holders = 0;
  if (evidence != nullptr && evidence->exclusive.has_value() && evidence->exclusive->known()) {
    held_known = true;
    held_class = evidence->exclusive->compatibility_class;
    holders = evidence->exclusive->holders;
  }
  item.held_class = held_class;
  item.holders = holders;

  if (request.quantity == 0U) {
    // A request for nothing asks for no change. It is satisfiable only when no
    // incompatible holder is already recorded, which is decidable from evidence.
    if (!held_known) {
      item.outcome = DimensionOutcome::Indeterminate;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::MissingObservation, kind,
                   "no exclusive-holder declaration was supplied, so the current holder is unknown");
      return;
    }
    if (holders == 0U) return;
    const bool compatible = !held_class.empty() && held_class == spec.compatibility_class;
    if (!compatible) {
      item.outcome = DimensionOutcome::Denied;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedExclusiveConflict, kind,
                   "an incompatible principal already holds this exclusive dimension");
    }
    return;
  }

  if (request.compatibility_class.empty()) {
    item.outcome = DimensionOutcome::Denied;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::InvalidArgument, kind,
                 "an exclusive dimension can only be requested with an explicit compatibility class");
    return;
  }
  if (!spec.compatibility_class.empty() && request.compatibility_class != spec.compatibility_class) {
    item.outcome = DimensionOutcome::Denied;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedExposureClass, kind,
                 "the requested compatibility class is not the class the envelope permits");
    return;
  }
  if (!builder.request->accepted_classes.empty() &&
      std::find(builder.request->accepted_classes.begin(), builder.request->accepted_classes.end(),
                request.compatibility_class) == builder.request->accepted_classes.end()) {
    item.outcome = DimensionOutcome::Denied;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedExposureClass, kind,
                 "the requested compatibility class is outside the caller's accepted class list");
    return;
  }
  if (held_known && holders != 0U) {
    const bool compatible = !held_class.empty() && held_class == request.compatibility_class;
    if (!compatible) {
      item.outcome = DimensionOutcome::Denied;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedExclusiveConflict, kind,
                   "the dimension is already held by an incompatible principal");
    }
    return;
  }
  if (!held_known) {
    // Without a holder declaration the runtime cannot tell whether the exclusive
    // dimension is free. Absence of evidence is not evidence that it is free.
    item.outcome = DimensionOutcome::Indeterminate;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::MissingObservation, kind,
                 "no exclusive-holder declaration was supplied, so the dimension cannot be granted");
  }
}


// NonConsumable dimensions declare a threshold that must be met. They yield no
// headroom, so no residual is reported for them, and a requirement that cannot be
// evidenced is Indeterminate rather than unmet.
void constrain_non_consumable(EvaluationBuilder& builder, const DimensionSpec& spec,
                             const DimensionRequest& request, DimensionResult& item) {
  const DimensionKind kind = request.kind;
  const Evidence* evidence = nullptr;
  for (const Evidence& candidate : builder.request->evidence.items) {
    if (candidate.kind == kind) {
      evidence = &candidate;
      break;
    }
  }

  std::optional<Nanounits> required;
  if (spec.hard_limit.has_value()) {
    required = spec.hard_limit;
  }
  if (evidence != nullptr && evidence->threshold.has_value() && evidence->threshold->known()) {
    required = evidence->threshold->required;
  }
  item.required_threshold = required;

  std::optional<Nanounits> declared;
  if (evidence != nullptr && evidence->committed.has_value() && evidence->committed->known()) {
    declared = evidence->committed->value;
  }
  item.declared_threshold = declared;

  if (kind == DimensionKind::RedundancyLevel) {
    item.desired_level = request.level;
    if (declared.has_value() && *declared <= static_cast<Nanounits>(kUnitCountMax)) {
      item.declared_level = static_cast<std::uint32_t>(*declared);
    }
    if (request.operational_spare.has_value()) item.operational_spare = *request.operational_spare;

    if (!required.has_value()) {
      item.outcome = DimensionOutcome::Indeterminate;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::IndeterminateUnknownLimit, kind,
                   "the envelope declares no required redundancy level, so the request cannot be checked");
      return;
    }
    if (!declared.has_value()) {
      item.outcome = DimensionOutcome::Indeterminate;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::IndeterminateUnknownCommitted, kind,
                   "the declared redundancy level was not supplied, so the requirement cannot be checked");
      return;
    }
    if (*declared < *required) {
      item.outcome = DimensionOutcome::Denied;
      item.shortfall = *required - *declared;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedRedundancy, kind,
                   "the declared redundancy level is below the level the envelope requires");
      return;
    }
    if (!request.operational_spare.has_value()) {
      // A declared level is not by itself proof of redundancy: the spare must be
      // operational, and an unmeasured spare is unknown rather than present.
      item.outcome = DimensionOutcome::Indeterminate;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::MissingObservation, kind,
                   "no operational-spare declaration was supplied, so redundancy is unproven");
      return;
    }
    if (!*request.operational_spare) {
      item.outcome = DimensionOutcome::Denied;
      builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedRedundancy, kind,
                   "the declared spare is not operational, so the requirement is unmet");
    }
    return;
  }

  // Cooling energy budget and any other non-consumable threshold.
  if (!required.has_value()) {
    item.outcome = DimensionOutcome::Indeterminate;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::IndeterminateUnknownLimit, kind,
                 "the envelope declares no threshold for this dimension");
    return;
  }
  if (!declared.has_value()) {
    item.outcome = DimensionOutcome::Indeterminate;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::IndeterminateUnknownCommitted, kind,
                 "the declared threshold was not supplied, so the requirement cannot be checked");
    return;
  }
  if (*declared < *required) {
    item.outcome = DimensionOutcome::Denied;
    item.shortfall = *required - *declared;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedHardLimit, kind,
                 "the declared threshold is below the level the envelope requires");
  }
}

// Evaluates one requested dimension against one envelope. The order of the checks
// below is the contract: an exclusive conflict, an unknown bound, contradictory
// reservation, an unknown commitment, an over-committed dimension, and finally an
// insufficient residual.
DimensionResult evaluate_dimension(EvaluationBuilder& builder, const Target& target,
                                   const DimensionRequest& request, const DimensionSpec* spec,
                                   bool have_call_committed, const CommittedUsage& call_committed) {
  DimensionResult item;
  item.kind = request.kind;
  item.requested = request.quantity;
  item.principals = request.principals;
  if (spec == nullptr) {
    item.outcome = DimensionOutcome::NotDeclared;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::OpaqueLimit, request.kind,
                 "the envelope declares no constraint for this dimension, so no authority exists for it");
    return item;
  }
  // The envelope declares this dimension, so the request is being determined against a
  // constraint. Every path below either returns a stronger outcome or leaves this one.
  item.outcome = DimensionOutcome::Satisfied;

  // Observation is recorded for explainability and never feeds the arithmetic.
  for (const Evidence& candidate : builder.request->evidence.items) {
    if (candidate.kind != request.kind) continue;
    if (candidate.observation.has_value()) {
      item.observation_present = true;
      if (candidate.observation->known()) item.observed = candidate.observation->value;
    }
    break;
  }

  item.limit = spec->hard_limit;
  item.reserved = spec->reserved;
  item.quantum = spec->quantum;

  if (cardinality_of(request.kind) == Cardinality::Exclusive) {
    constrain_exclusive(builder, request, *spec, item);
    return item;
  }
  if (cardinality_of(request.kind) == Cardinality::NonConsumable) {
    constrain_non_consumable(builder, *spec, request, item);
    return item;
  }

  if (!spec->hard_limit.has_value()) {
    // An unknown bound is neither zero nor unlimited. Nothing can be determined.
    item.outcome = DimensionOutcome::Indeterminate;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::IndeterminateUnknownLimit, request.kind,
                 "the envelope bound for this dimension is unknown, so no residual can be claimed");
    return item;
  }
  if (spec->reserved > *spec->hard_limit) {
    item.outcome = DimensionOutcome::Indeterminate;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::InsufficientReserved, request.kind,
                 "reserved capacity exceeds the declared bound; the envelope is internally contradictory");
    return item;
  }

  const Nanounits available = *spec->hard_limit - spec->reserved;
  CommittedView committed = resolve_committed(builder, target, request.kind);
  if (!committed.known && have_call_committed && call_committed.known()) {
    committed.known = true;
    committed.value = call_committed.value;
  }
  if (committed.known) item.committed = committed.value;

  // Consumption figures are aligned upwards to the declared quantum. That is the
  // conservative direction: it can only reduce the claimed residual.
  apply_quantum(builder, request.kind, spec->quantum,
                committed.known ? std::optional<Nanounits>(committed.value) : std::nullopt,
                &request.quantity, item);

  const bool over_committed = committed.known && committed.value > available;
  item.over_committed = over_committed;
  item.over_committed_by = over_committed ? committed.value - available : 0U;

  std::optional<Nanounits> residual;
  if (committed.known) {
    const Nanounits effective = item.effective_committed.value_or(committed.value);
    residual = effective >= available ? 0U : available - effective;
  }
  item.residual = residual;

  std::optional<Nanounits> aligned_request;
  if (spec->quantum == 0U) {
    aligned_request = request.quantity;
  } else {
    const CheckedQuantity aligned = round_up_to_multiple(request.quantity, spec->quantum);
    aligned_request = aligned.overflow ? std::optional<Nanounits>(kArithmeticCeiling)
                                       : std::optional<Nanounits>(aligned.value);
  }
  if (residual.has_value() && aligned_request.has_value()) {
    const CheckedQuantity after = checked_sub(*residual, *aligned_request);
    // A request larger than the residual leaves exactly zero headroom. Reporting a
    // saturated value would hide the deficit that over_committed_by already makes
    // explicit.
    item.headroom_after = after.overflow ? 0U : after.value;
  }

  const bool request_none = request.quantity == 0U && request.principals <= 1U;
  if (!committed.known) {
    if (request_none) return item;
    item.outcome = DimensionOutcome::Indeterminate;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::IndeterminateUnknownCommitted, request.kind,
                 "committed usage for this dimension is not known, so the residual cannot be determined");
    return item;
  }
  if (over_committed) {
    if (request_none) return item;
    item.outcome = DimensionOutcome::Denied;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedHardLimit, request.kind,
                 "committed usage already meets or exceeds the capacity available to this envelope");
    return item;
  }
  if (request_none) return item;
  if (aligned_request.has_value() && *aligned_request > *residual) {
    item.outcome = DimensionOutcome::Denied;
    builder.note(RefusalStage::DimensionConstraint, StatusCode::RejectedInsufficientResidual, request.kind,
                 "the requested quantity exceeds the residual capacity of this dimension");
  }
  return item;
}

DimensionResult evaluate_dimension(EvaluationBuilder& builder, const Target& target,
                                   const DimensionRequest& request, const DimensionSpec* spec,
                                   bool have_call_committed, const CommittedUsage& call_committed);
void evaluate_target(EvaluationBuilder& builder, const Target& target);

// Aggregation order. Refusal stages are compared first and, within a stage, a
// dimension constraint outranks an arithmetic overflow because the constraint is
// the reason the request cannot proceed while the overflow is a property of the
// numbers. Anything before the dimension stage always decides first.
bool issue_precedes(const Issue& left, const Issue& right) noexcept {
  const auto rank = [](const Issue& issue) {
    const std::uint8_t stage = static_cast<std::uint8_t>(issue.stage);
    if (stage < static_cast<std::uint8_t>(RefusalStage::DimensionConstraint)) return stage;
    if (issue.stage == RefusalStage::DimensionConstraint) return static_cast<std::uint8_t>(200);
    return static_cast<std::uint8_t>(201);
  };
  return rank(left) < rank(right);
}

Outcome outcome_for(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::IndeterminateUnknownLimit:
    case StatusCode::IndeterminateUnknownCommitted:
    case StatusCode::MissingObservation:
    case StatusCode::RequiredBindingUnavailable: return Outcome::Indeterminate;
    default: return Outcome::Denied;
  }
}

// Resolves the request against one envelope and appends the per-dimension results.
void evaluate_target(EvaluationBuilder& builder, const Target& target) {
  const EvaluationRequest& request = *builder.request;
  std::set<DimensionKind> requested;
  for (const DimensionRequest& dimension : request.dimensions) {
    requested.insert(dimension.kind);
    bool have_call_committed = false;
    CommittedUsage call_committed;
    for (const Evidence& evidence : request.evidence.items) {
      if (evidence.kind != dimension.kind) continue;
      if (evidence.committed.has_value()) {
        have_call_committed = true;
        call_committed = *evidence.committed;
      }
      break;
    }
    const DimensionSpec* spec = target.envelope->find(dimension.kind);
    builder.result.dimensions.push_back(
        evaluate_dimension(builder, target, dimension, spec, have_call_committed, call_committed));
  }
  // Evidence for a dimension the caller did not request is recorded but never
  // evaluated: an envelope is never consulted about dimensions nobody asked for.
  for (const Evidence& evidence : request.evidence.items) {
    if (requested.count(evidence.kind) == 0U) {
      builder.note(RefusalStage::RequestValidation, StatusCode::UnexpectedObservation, evidence.kind,
                   "evidence was supplied for a dimension the request does not ask about");
    }
  }
}

// Combines the independent determinations of two envelopes for one dimension by
// taking the most restrictive of each reported field. Doing this per field rather
// than per envelope is what guarantees that no dimension is ever re-derived from a
// synthetic merged limit.
DimensionResult combine_dimension(const DimensionResult& left, const DimensionResult& right) {
  DimensionResult combined = left;
  const auto severity = [](DimensionOutcome outcome) {
    switch (outcome) {
      case DimensionOutcome::Denied: return 3;
      case DimensionOutcome::Indeterminate: return 2;
      case DimensionOutcome::NotDeclared: return 1;
      case DimensionOutcome::Satisfied: return 0;
      case DimensionOutcome::NotRequested: return -1;
    }
    return -1;
  };
  if (severity(right.outcome) > severity(left.outcome)) combined.outcome = right.outcome;
  if (right.limit.has_value() &&
      (!combined.limit.has_value() || *right.limit < *combined.limit)) {
    combined.limit = right.limit;
  }
  if (right.reserved > combined.reserved) combined.reserved = right.reserved;
  if (right.residual.has_value() &&
      (!combined.residual.has_value() || *right.residual < *combined.residual)) {
    combined.residual = right.residual;
  }
  if (right.over_committed) {
    combined.over_committed = true;
    combined.over_committed_by = std::max(combined.over_committed_by, right.over_committed_by);
  }
  if (right.headroom_after.has_value() &&
      (!combined.headroom_after.has_value() || *right.headroom_after < *combined.headroom_after)) {
    combined.headroom_after = right.headroom_after;
  }
  if (!combined.committed.has_value() || !right.committed.has_value()) {
    // A consumption figure that is unknown in either envelope leaves the combined
    // figure unknown. That is the conservative direction, and it is also why an
    // unknown capacity can never be composed away by a second envelope.
    combined.committed.reset();
    combined.effective_committed.reset();
    combined.residual.reset();
    combined.headroom_after.reset();
  }
  if (combined.quantum == 0U) combined.quantum = right.quantum;
  if (right.quantized_quantity.has_value() &&
      (!combined.quantized_quantity.has_value() ||
       *right.quantized_quantity > *combined.quantized_quantity)) {
    combined.quantized_quantity = right.quantized_quantity;
  }
  if (!right.required_class.empty()) combined.required_class = right.required_class;
  if (!right.held_class.empty()) combined.held_class = right.held_class;
  combined.holders = std::max(combined.holders, right.holders);
  if (right.required_threshold.has_value() &&
      (!combined.required_threshold.has_value() ||
       *right.required_threshold > *combined.required_threshold)) {
    combined.required_threshold = right.required_threshold;
  }
  if (right.declared_threshold.has_value()) combined.declared_threshold = right.declared_threshold;
  if (right.shortfall.has_value()) combined.shortfall = right.shortfall;
  combined.desired_level = std::max(combined.desired_level, right.desired_level);
  combined.declared_level = std::max(combined.declared_level, right.declared_level);
  if (right.operational_spare.has_value()) combined.operational_spare = right.operational_spare;
  for (const StatusCode code : right.secondary) {
    if (std::find(combined.secondary.begin(), combined.secondary.end(), code) == combined.secondary.end()) {
      combined.secondary.push_back(code);
    }
  }
  return combined;
}

}  // namespace

// The public evaluation entry points. They are members of the library namespace because
// they are part of the surface an owner with its own storage uses; everything they call
// is internal to this translation unit.
EvaluationResult evaluate_with_context(const EvaluationContext& context, const EvaluationRequest& request) {
  EvaluationBuilder builder;
  builder.request = &request;
  builder.committed = context.committed;
  builder.committed_envelope_id = context.committed_envelope_id;
  builder.result.control_epoch = context.control_epoch;
  builder.result.request_digest = request_digest(request);
  builder.result.evidence_digest = evidence_digest(request.evidence);
  builder.result.decision_sequence = context.decision_sequence;

  validate_request(builder);

  // Stage: envelope presence.
  if (context.envelope == nullptr) {
    builder.note(RefusalStage::EnvelopePresence, StatusCode::EnvelopeNotFound, std::nullopt,
                 "no envelope is available for this evaluation");
  } else {
    builder.result.envelope_id = context.envelope->id;
    builder.result.envelope_revision = context.envelope->revision;
    builder.result.envelope_digest = record_digest(*context.envelope);

    // Stage: scope match.
    if (context.envelope->scope != request.scope) {
      builder.note(RefusalStage::ScopeMatch, StatusCode::ForeignScope, std::nullopt,
                   "the request scope does not match the scope of the resolved envelope");
    }

    // Stage: lineage integrity. Lineage is a claim about other envelopes, and a
    // claim that points at the envelope making it is self-referential. That is
    // refused rather than ignored, because it would make the authority graph cyclic.
    if (context.envelope->supersedes.has_value() && *context.envelope->supersedes == context.envelope->id) {
      builder.note(RefusalStage::LineageIntegrity, StatusCode::InvalidArgument, std::nullopt,
                   "the envelope supersedes itself");
    }
    for (const EnvelopeRef& reference : context.envelope->merged_from) {
      if (reference.envelope_id == context.envelope->id) {
        builder.note(RefusalStage::LineageIntegrity, StatusCode::InvalidArgument, std::nullopt,
                     "the envelope lists itself as a composition source");
        break;
      }
    }

    // Stage: envelope composition.
    const bool conjunctive = request.facility_composition == FacilityComposition::ConjunctiveWithFacility;
    if (conjunctive) {
      if (context.facility_envelope == nullptr) {
        builder.note(RefusalStage::EnvelopeComposition, StatusCode::EnvelopeNotFound, std::nullopt,
                     "conjunctive composition was requested but no facility envelope was resolved");
      } else if (context.facility_envelope->scope.kind != EnvelopeScopeKind::Facility) {
        builder.note(RefusalStage::EnvelopeComposition, StatusCode::InvalidArgument, std::nullopt,
                     "the composed envelope is not scoped to the facility");
      } else if (context.facility_envelope->id == context.envelope->id) {
        // The facility envelope stands in for the principal envelope rather than
        // composing with it. Evaluating it twice would double-count its constraints.
      } else if (context.envelope->merge_policy == MergePolicy::Strict) {
        for (const DimensionSpec& spec : context.facility_envelope->dimensions) {
          if (context.envelope->find(spec.kind) != nullptr) {
            builder.note(RefusalStage::EnvelopeComposition, StatusCode::AmbiguousOverlap, spec.kind,
                         "strict composition refuses an overlapping dimension in the facility envelope");
            break;
          }
        }
      }
    }

    // Stage: envelope currency. A detached envelope is evaluated on its own terms
    // only when the caller states that no durable authority stands behind it.
    if (!context.envelope_is_current) {
      builder.note(RefusalStage::EnvelopeCurrent, StatusCode::EnvelopeNotCurrent, std::nullopt,
                   "the envelope revision under evaluation is not the current revision");
    }

    // Stage: generation binding. A declared binding must be confirmed; absent
    // confirmation is never treated as agreement.
    const bool identity_declared = context.envelope->identity_digest.known();
    const bool identity_supplied = !request.identity.identity.id.empty();
    if (identity_declared && identity_supplied) {
      if (request.identity.identity != context.envelope->scope.identity) {
        builder.note(RefusalStage::GenerationBinding, StatusCode::IdentityBindingMismatch, std::nullopt,
                     "the supplied identity generation is not the generation the envelope is bound to");
      } else if (context.envelope->identity_digest != request.identity.digest) {
        builder.note(RefusalStage::GenerationBinding, StatusCode::IdentityBindingMismatch, std::nullopt,
                     "the supplied identity digest is not the digest the envelope is bound to");
      }
    } else if (identity_declared && context.envelope->require_binding_confirmation) {
      builder.note(RefusalStage::GenerationBinding, StatusCode::IdentityBindingMismatch, std::nullopt,
                   "the envelope requires identity confirmation and none was supplied");
    }

    // Stage: effective window. Both ends are half-open, so the declared start is
    // authoritative and the declared end has already stopped being authoritative.
    if (context.envelope->window.ended_at(request.at)) {
      builder.note(RefusalStage::EffectiveWindow, StatusCode::ExpiredAuthority, std::nullopt,
                   "the envelope effective window has ended at the evaluation timestamp");
    } else if (!context.envelope->window.started_at(request.at)) {
      builder.note(RefusalStage::EffectiveWindow, StatusCode::EnvelopeNotCurrent, std::nullopt,
                   "the envelope effective window has not started at the evaluation timestamp");
    }

    // Stage: requested revision.
    if (request.expected_envelope_revision.has_value() &&
        *request.expected_envelope_revision != context.envelope->revision) {
      builder.note(RefusalStage::RequestedRevision, StatusCode::StaleRevision, std::nullopt,
                   "the expected envelope revision is not the revision under evaluation");
    }
    if (request.expected_envelope_digest.has_value() &&
        *request.expected_envelope_digest != builder.result.envelope_digest) {
      builder.note(RefusalStage::RequestedRevision, StatusCode::StaleRevision, std::nullopt,
                   "the expected envelope digest is not the digest under evaluation");
    }

    // Stage: context binding for the externally owned service class and policy.
    if (context.envelope->service_class_digest.known()) {
      if (request.service_class.state != ResolutionState::Provided) {
        builder.note(RefusalStage::ContextBinding, StatusCode::ServiceClassBindingMismatch, std::nullopt,
                     "the envelope is bound to a service class revision that the caller did not supply");
      } else if (request.service_class.digest != context.envelope->service_class_digest) {
        builder.note(RefusalStage::ContextBinding, StatusCode::ServiceClassBindingMismatch, std::nullopt,
                     "the supplied service class digest is not the digest the envelope is bound to");
      }
    }
    if (context.envelope->policy_digest.known()) {
      if (request.policy.state != ResolutionState::Provided) {
        builder.note(RefusalStage::ContextBinding, StatusCode::PolicyBindingMismatch, std::nullopt,
                     "the envelope is bound to a policy revision that the caller did not supply");
      } else if (request.policy.digest != context.envelope->policy_digest) {
        builder.note(RefusalStage::ContextBinding, StatusCode::PolicyBindingMismatch, std::nullopt,
                     "the supplied policy digest is not the digest the envelope is bound to");
      }
    }

    // Stage: provider availability. An unavailable authority is never read as
    // agreement, and an optional but unavailable one is indeterminate rather than
    // granted.
    if (identity_declared && !identity_supplied &&
        request.identity.digest.known()) {
      builder.note(RefusalStage::ProviderAvailability, StatusCode::RequiredBindingUnavailable, std::nullopt,
                   "an identity digest was supplied without an identity identifier");
    }
    if (context.envelope->service_class_digest.known() &&
        request.service_class.state == ResolutionState::Unavailable) {
      builder.note(RefusalStage::ProviderAvailability, StatusCode::RequiredBindingUnavailable, std::nullopt,
                   "the service class authority could not be consulted");
    }
    if (context.envelope->policy_digest.known() && request.policy.state == ResolutionState::Unavailable) {
      builder.note(RefusalStage::ProviderAvailability, StatusCode::RequiredBindingUnavailable, std::nullopt,
                   "the policy authority could not be consulted");
    }
  }

  // The primary reason is chosen before any dimension is evaluated, so a request
  // that cannot reach the arithmetic never performs it.
  std::vector<Issue> blocking;
  for (const Issue& issue : builder.issues) {
    if (issue.code == StatusCode::UnexpectedObservation) {
      continue;
    }
    if (static_cast<std::uint8_t>(issue.stage) <
        static_cast<std::uint8_t>(RefusalStage::DimensionConstraint)) {
      blocking.push_back(issue);
    }
  }
  if (!blocking.empty()) {
    std::stable_sort(blocking.begin(), blocking.end(), issue_precedes);
    const Issue& primary = blocking.front();
    builder.result.outcome = outcome_for(primary.code);
    builder.result.reason = primary.code;
    builder.result.stage = primary.stage;
    builder.result.blocking_dimension = primary.dimension;
    for (const Issue& issue : builder.issues) {
      if (issue.code != primary.code &&
          std::find(builder.result.secondary.begin(), builder.result.secondary.end(), issue.code) ==
              builder.result.secondary.end()) {
        builder.result.secondary.push_back(issue.code);
      }
    }
    builder.result.decision_digest =
        Digest(sha256_domain(kDomainDecision, canonical_evaluation_result(builder.result)));
    return builder.result;
  }

  // Stage: dimension constraints and arithmetic. Reaching this point means the
  // request is structurally valid and the authority is current, so every remaining
  // failure is attributable to a dimension.
  if (context.envelope != nullptr) {
    Target primary;
    primary.envelope = context.envelope;
    primary.is_current = context.envelope_is_current;
    primary.stored_sequence = context.envelope_stored_sequence;
    primary.record_digest = builder.result.envelope_digest;
    const std::size_t primary_begin = builder.result.dimensions.size();
    evaluate_target(builder, primary);
    const std::size_t primary_end = builder.result.dimensions.size();

    if (request.facility_composition == FacilityComposition::ConjunctiveWithFacility &&
        context.facility_envelope != nullptr && context.facility_envelope->id != context.envelope->id) {
      Target facility;
      facility.envelope = context.facility_envelope;
      facility.is_current = true;
      facility.record_digest = record_digest(*context.facility_envelope);
      const std::string saved_id = builder.committed_envelope_id;
      builder.committed_envelope_id = context.facility_envelope->id;
      std::vector<DimensionResult> facility_results;
      const auto swap_sink = [&builder, &facility_results]() {
        facility_results = std::move(builder.result.dimensions);
        builder.result.dimensions.clear();
      };
      evaluate_target(builder, facility);
      swap_sink();
      builder.committed_envelope_id = saved_id;

      // Combine per dimension: the principal determination is the base and the
      // facility determination tightens it field by field.
      std::vector<DimensionResult> combined;
      combined.reserve(primary_end - primary_begin);
      for (std::size_t index = primary_begin; index < primary_end; ++index) {
        DimensionResult merged = builder.result.dimensions[index - primary_begin];
        for (const DimensionResult& facility_result : facility_results) {
          if (facility_result.kind != merged.kind) continue;
          if (facility_result.outcome == DimensionOutcome::NotDeclared) break;
          merged = combine_dimension(merged, facility_result);
          break;
        }
        combined.push_back(std::move(merged));
      }
      builder.result.dimensions.clear();
      for (DimensionResult& entry : combined) builder.result.dimensions.push_back(std::move(entry));
    }
  }

  // The verdict is derived from every issue the evaluation collected, excluding the
  // advisory notice that evidence was supplied for a dimension nobody asked about. Reaching
  // this point means no earlier stage refused, so a request with no collected issue at all
  // was satisfied in every dimension it named and is Granted.
  std::vector<Issue> decisive;
  for (const Issue& issue : builder.issues) {
    if (issue.code == StatusCode::UnexpectedObservation) continue;
    decisive.push_back(issue);
  }
  if (decisive.empty()) {
    builder.result.outcome = Outcome::Granted;
    builder.result.reason = StatusCode::Ok;
    builder.result.stage = RefusalStage::None;
    builder.result.blocking_dimension.reset();
  } else {
    std::stable_sort(decisive.begin(), decisive.end(), issue_precedes);
    const Issue& primary = decisive.front();
    builder.result.outcome = outcome_for(primary.code);
    builder.result.reason = primary.code;
    builder.result.stage = primary.stage;
    builder.result.blocking_dimension = primary.dimension;
  }
  for (const Issue& issue : builder.issues) {
    if (issue.code == StatusCode::UnexpectedObservation) continue;
    if (std::find(builder.result.secondary.begin(), builder.result.secondary.end(), issue.code) ==
        builder.result.secondary.end()) {
      builder.result.secondary.push_back(issue.code);
    }
  }
  for (const DimensionResult& entry : builder.result.dimensions) {
    for (const StatusCode code : entry.secondary) {
      if (std::find(builder.result.secondary.begin(), builder.result.secondary.end(), code) ==
          builder.result.secondary.end()) {
        builder.result.secondary.push_back(code);
      }
    }
  }
  builder.result.decision_digest =
      Digest(sha256_domain(kDomainDecision, canonical_evaluation_result(builder.result)));
  return builder.result;
}

EvaluationResult evaluate(const Envelope& envelope, const EvaluationRequest& request) {
  EvaluationContext context;
  context.envelope = &envelope;
  return evaluate_with_context(context, request);
}

}  // namespace resource_envelope
