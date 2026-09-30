#include "support/testing.hpp"

#include "resource_envelope/service.hpp"
#include "resource_envelope/canonical.hpp"

using namespace resource_envelope;

namespace {

constexpr Nanounits kWatt = kNanounitsPerUnit;

Envelope tenant_envelope() {
  Envelope envelope;
  envelope.id = "tenant-a-envelope";
  envelope.revision = 1U;
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = "tenant-a";
  envelope.scope.identity.generation = 7U;
  envelope.site_id = "site-one";
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "operator";
  envelope.provenance.declared_at = 1767225600000000000LL;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = 100ULL * kWatt;
  power.reserved = 10ULL * kWatt;
  envelope.dimensions.push_back(power);
  DimensionSpec space;
  space.kind = DimensionKind::SpaceRackUnits;
  space.hard_limit = 40ULL * kNanounitsPerUnit;
  envelope.dimensions.push_back(space);
  return envelope;
}

// The same envelope with an explicit committed figure. A residual is only exact when both
// the bound and the committed usage are known, so tests that assert an exact residual
// declare the commitment rather than relying on a default of zero.
Envelope tenant_envelope_with_committed(Nanounits committed_watts) {
  Envelope envelope = tenant_envelope();
  DimensionSpec* power = envelope.find(DimensionKind::PowerDrawWatts);
  CommittedUsage committed;
  committed.status = MeasureStatus::Measured;
  committed.value = committed_watts;
  committed.observed_at = 1767225600000000000LL;
  committed.source = "facility-capacity";
  power->committed = committed;
  return envelope;
}

EvaluationRequest request_for(std::uint64_t envelope_revision, Nanounits watts) {
  EvaluationRequest request;
  request.scope.kind = EnvelopeScopeKind::Tenant;
  request.scope.identity.id = "tenant-a";
  request.scope.identity.generation = 7U;
  request.at = 1767225600000000000LL;
  DimensionRequest dimension;
  dimension.kind = DimensionKind::PowerDrawWatts;
  dimension.quantity = watts;
  request.dimensions.push_back(dimension);
  static_cast<void>(envelope_revision);
  return request;
}

}  // namespace

// KNOWN GAP: the exact residual arithmetic is correct and is asserted below, but the
// overall verdict for a request that a known residual satisfies is reported as
// Indeterminate rather than Granted. The cause is under investigation in
// eval/evaluate.cpp's verdict aggregation; the per-dimension arithmetic is unaffected.
RE_TEST(evaluation_uses_reserved_capacity_from_the_residual) {
  const Envelope envelope = tenant_envelope_with_committed(0ULL);
  const EvaluationResult result = evaluate(envelope, request_for(1U, 40ULL * kWatt));
  RE_CHECK_EQ(result.outcome, Outcome::Granted);
  RE_CHECK_EQ(result.reason, StatusCode::Ok);
  RE_CHECK_EQ(result.stage, RefusalStage::None);
  RE_CHECK_EQ(result.dimensions.size(), std::size_t{1});
  const DimensionResult& power = result.dimensions.front();
  RE_CHECK_EQ(power.outcome, DimensionOutcome::Satisfied);
  RE_CHECK(result.secondary.empty());
  // A granted decision carries a grant digest, which is what a downstream consumer fences on.
  RE_CHECK(result.decision_digest.known());
  RE_REQUIRE(power.limit.has_value());
  RE_CHECK_EQ(*power.limit, 100ULL * kWatt);
  RE_CHECK_EQ(power.reserved, 10ULL * kWatt);
  RE_REQUIRE(power.residual.has_value());
  RE_CHECK_EQ(*power.residual, 90ULL * kWatt);
  RE_REQUIRE(power.headroom_after.has_value());
  RE_CHECK_EQ(*power.headroom_after, 50ULL * kWatt);
}

RE_TEST(missing_committed_usage_never_becomes_zero_headroom) {
  // The envelope declares no committed usage and the request supplies none, so the
  // residual is unknown. Treating that as zero would refuse a request the facility may
  // well be able to serve.
  const Envelope envelope = tenant_envelope();
  const EvaluationResult result = evaluate(envelope, request_for(1U, 1ULL * kWatt));
  RE_CHECK_EQ(result.outcome, Outcome::Indeterminate);
  RE_CHECK_EQ(result.reason, StatusCode::IndeterminateUnknownCommitted);
  RE_CHECK_EQ(result.stage, RefusalStage::DimensionConstraint);
  RE_REQUIRE(result.blocking_dimension.has_value());
  RE_CHECK_EQ(*result.blocking_dimension, DimensionKind::PowerDrawWatts);
  RE_CHECK(!result.dimensions.front().residual.has_value());
}

RE_TEST(declared_committed_usage_makes_the_residual_exact) {
  Envelope envelope = tenant_envelope();
  DimensionSpec* power = envelope.find(DimensionKind::PowerDrawWatts);
  RE_REQUIRE(power != nullptr);
  CommittedUsage committed;
  committed.status = MeasureStatus::Measured;
  committed.value = 30ULL * kWatt;
  committed.observed_at = 1767225600000000000LL;
  committed.source = "facility-capacity";
  power->committed = committed;
  const EvaluationResult result = evaluate(envelope, request_for(1U, 61ULL * kWatt));
  RE_CHECK_EQ(result.outcome, Outcome::Denied);
  RE_CHECK_EQ(result.reason, StatusCode::RejectedInsufficientResidual);
  const DimensionResult& item = result.dimensions.front();
  RE_REQUIRE(item.residual.has_value());
  RE_CHECK_EQ(*item.residual, 60ULL * kWatt);
  RE_REQUIRE(item.headroom_after.has_value());
  RE_CHECK_EQ(*item.headroom_after, 0ULL);
}

RE_TEST(observation_is_reported_but_never_used_as_consumption) {
  Envelope envelope = tenant_envelope();
  EvaluationRequest request = request_for(1U, 1ULL * kWatt);
  Evidence evidence;
  evidence.kind = DimensionKind::PowerDrawWatts;
  Measurement observation;
  observation.status = MeasureStatus::Measured;
  observation.value = 95ULL * kWatt;
  observation.observed_at = request.at;
  observation.source = "meter";
  evidence.observation = observation;
  request.evidence.items.push_back(evidence);
  const EvaluationResult result = evaluate(envelope, request);
  // The observation does not create a committed figure, so the determination stays
  // indeterminate even though a measurement exists.
  RE_CHECK_EQ(result.outcome, Outcome::Indeterminate);
  const DimensionResult& item = result.dimensions.front();
  RE_CHECK(item.observation_present);
  RE_REQUIRE(item.observed.has_value());
  RE_CHECK_EQ(*item.observed, 95ULL * kWatt);
  RE_CHECK(!item.committed.has_value());
}

RE_TEST(over_commitment_is_reported_rather_than_clamped) {
  Envelope envelope = tenant_envelope();
  DimensionSpec* power = envelope.find(DimensionKind::PowerDrawWatts);
  RE_REQUIRE(power != nullptr);
  CommittedUsage committed;
  committed.status = MeasureStatus::Measured;
  committed.value = 120ULL * kWatt;
  committed.observed_at = 1767225600000000000LL;
  committed.source = "facility-capacity";
  power->committed = committed;
  const EvaluationResult result = evaluate(envelope, request_for(1U, 1ULL * kWatt));
  RE_CHECK_EQ(result.outcome, Outcome::Denied);
  RE_CHECK_EQ(result.reason, StatusCode::RejectedHardLimit);
  const DimensionResult& item = result.dimensions.front();
  RE_CHECK(item.over_committed);
  RE_CHECK_EQ(item.over_committed_by, 30ULL * kWatt);
  RE_REQUIRE(item.residual.has_value());
  RE_CHECK_EQ(*item.residual, 0ULL);
}

RE_TEST(undeclared_dimension_is_refused_and_attributed) {
  const Envelope envelope = tenant_envelope();
  EvaluationRequest request = request_for(1U, 0ULL);
  request.dimensions.clear();
  DimensionRequest dimension;
  dimension.kind = DimensionKind::CoolingLoadWatts;
  dimension.quantity = 1ULL * kWatt;
  request.dimensions.push_back(dimension);
  const EvaluationResult result = evaluate(envelope, request);
  RE_CHECK_EQ(result.outcome, Outcome::Denied);
  RE_CHECK_EQ(result.reason, StatusCode::OpaqueLimit);
  RE_REQUIRE(result.blocking_dimension.has_value());
  RE_CHECK_EQ(*result.blocking_dimension, DimensionKind::CoolingLoadWatts);
}

RE_TEST(unknown_limit_is_indeterminate_not_zero) {
  Envelope envelope = tenant_envelope();
  DimensionSpec* space = envelope.find(DimensionKind::SpaceRackUnits);
  RE_REQUIRE(space != nullptr);
  space->hard_limit = std::nullopt;
  EvaluationRequest request = request_for(1U, 0ULL);
  request.dimensions.clear();
  DimensionRequest dimension;
  dimension.kind = DimensionKind::SpaceRackUnits;
  dimension.quantity = 5ULL * kNanounitsPerUnit;
  request.dimensions.push_back(dimension);
  const EvaluationResult result = evaluate(envelope, request);
  RE_CHECK_EQ(result.outcome, Outcome::Indeterminate);
  RE_CHECK_EQ(result.reason, StatusCode::IndeterminateUnknownLimit);
}

RE_TEST(quantization_refuses_misalignment_and_reports_the_aligned_quantity) {
  Envelope envelope = tenant_envelope_with_committed(0ULL);
  DimensionSpec* power = envelope.find(DimensionKind::PowerDrawWatts);
  RE_REQUIRE(power != nullptr);
  power->quantum = 5ULL * kWatt;
  CommittedUsage committed;
  committed.status = MeasureStatus::Measured;
  committed.value = 0ULL;
  committed.observed_at = 1767225600000000000LL;
  committed.source = "facility-capacity";
  power->committed = committed;
  const EvaluationResult misaligned = evaluate(envelope, request_for(1U, 7ULL * kWatt));
  RE_CHECK_EQ(misaligned.outcome, Outcome::Denied);
  RE_CHECK_EQ(misaligned.reason, StatusCode::RejectedQuantization);
  RE_REQUIRE(misaligned.dimensions.front().quantized_quantity.has_value());
  RE_CHECK_EQ(*misaligned.dimensions.front().quantized_quantity, 10ULL * kWatt);
  const EvaluationResult aligned = evaluate(envelope, request_for(1U, 5ULL * kWatt));
  RE_CHECK_EQ(aligned.outcome, Outcome::Granted);
  RE_REQUIRE(aligned.dimensions.front().headroom_after.has_value());
  RE_CHECK_EQ(*aligned.dimensions.front().headroom_after, 85ULL * kWatt);

}

RE_TEST(expired_envelope_is_never_authoritative) {
  Envelope envelope = tenant_envelope();
  envelope.window.effective_until = 1767225600000000000LL;
  const EvaluationResult result = evaluate(envelope, request_for(1U, 1ULL * kWatt));
  RE_CHECK_EQ(result.outcome, Outcome::Denied);
  RE_CHECK_EQ(result.reason, StatusCode::ExpiredAuthority);
  RE_CHECK_EQ(result.stage, RefusalStage::EffectiveWindow);
}

RE_TEST(window_boundaries_are_half_open) {
  Envelope envelope = tenant_envelope();
  envelope.window.effective_from = 1767225600000000000LL;
  envelope.window.effective_until = 1767225600000000000LL + 1000LL;
  EvaluationRequest request = request_for(1U, 1ULL * kWatt);
  request.at = 1767225600000000000LL - 1LL;
  RE_CHECK_EQ(evaluate(envelope, request).reason, StatusCode::EnvelopeNotCurrent);
  request.at = 1767225600000000000LL;
  RE_CHECK_EQ(evaluate(envelope, request).stage, RefusalStage::DimensionConstraint);
  request.at = 1767225600000000000LL + 999LL;
  RE_CHECK_EQ(evaluate(envelope, request).stage, RefusalStage::DimensionConstraint);
  request.at = 1767225600000000000LL + 1000LL;
  RE_CHECK_EQ(evaluate(envelope, request).reason, StatusCode::ExpiredAuthority);
}

RE_TEST(scope_mismatch_and_revision_fencing_are_named) {
  const Envelope envelope = tenant_envelope();
  EvaluationRequest wrong_scope = request_for(1U, 1ULL * kWatt);
  wrong_scope.scope.identity.generation = 8U;
  RE_CHECK_EQ(evaluate(envelope, wrong_scope).reason, StatusCode::ForeignScope);
  EvaluationRequest stale = request_for(1U, 1ULL * kWatt);
  stale.expected_envelope_revision = 4U;
  RE_CHECK_EQ(evaluate(envelope, stale).reason, StatusCode::StaleRevision);
  EvaluationRequest digest = request_for(1U, 1ULL * kWatt);
  Digest::Value zeros{};
  digest.expected_envelope_digest = Digest(zeros);
  RE_CHECK_EQ(evaluate(envelope, digest).reason, StatusCode::StaleRevision);
}

RE_TEST(redundancy_requires_an_operational_spare) {
  Envelope envelope = tenant_envelope_with_committed(0ULL);
  DimensionSpec redundancy;
  redundancy.kind = DimensionKind::RedundancyLevel;
  redundancy.hard_limit = 1ULL;
  envelope.dimensions.push_back(redundancy);
  EvaluationRequest request = request_for(1U, 0ULL);
  request.dimensions.clear();
  DimensionRequest dimension;
  dimension.kind = DimensionKind::RedundancyLevel;
  dimension.level = 1U;
  request.dimensions.push_back(dimension);
  // A declared level without an evidenced operational spare cannot satisfy the
  // requirement, because redundancy that is not operational is not redundancy.
  Evidence evidence;
  evidence.kind = DimensionKind::RedundancyLevel;
  CommittedUsage committed;
  committed.status = MeasureStatus::Measured;
  committed.value = 1ULL;
  committed.observed_at = request.at;
  committed.source = "facility-capacity";
  evidence.committed = committed;
  request.evidence.items.push_back(evidence);
  RE_CHECK_EQ(evaluate(envelope, request).outcome, Outcome::Indeterminate);
  request.dimensions.front().operational_spare = false;
  RE_CHECK_EQ(evaluate(envelope, request).outcome, Outcome::Denied);
  request.dimensions.front().operational_spare = true;
  RE_CHECK_EQ(evaluate(envelope, request).outcome, Outcome::Granted);
}

RE_TEST(exclusive_dimension_requires_an_evidenced_free_holder) {
  Envelope envelope = tenant_envelope_with_committed(0ULL);
  DimensionSpec exposure;
  exposure.kind = DimensionKind::RackExposureClass;
  exposure.hard_limit = 1ULL;
  exposure.compatibility_class = "class-a";
  envelope.dimensions.push_back(exposure);
  EvaluationRequest request = request_for(1U, 0ULL);
  request.dimensions.clear();
  DimensionRequest dimension;
  dimension.kind = DimensionKind::RackExposureClass;
  dimension.quantity = 1ULL * kNanounitsPerUnit;
  dimension.compatibility_class = "class-a";
  request.dimensions.push_back(dimension);
  // Without a holder declaration the runtime cannot tell whether the exclusive
  // dimension is free, so absence of evidence is not evidence that it is free.
  RE_CHECK_EQ(evaluate(envelope, request).outcome, Outcome::Indeterminate);
  Evidence evidence;
  evidence.kind = DimensionKind::RackExposureClass;
  ExclusiveDeclaration declaration;
  declaration.status = MeasureStatus::Measured;
  declaration.holders = 0U;
  declaration.observed_at = request.at;
  declaration.source = "facility-capacity";
  evidence.exclusive = declaration;
  request.evidence.items.push_back(evidence);
  RE_CHECK_EQ(evaluate(envelope, request).outcome, Outcome::Granted);
  request.evidence.items.front().exclusive->compatibility_class = "class-b";
  request.evidence.items.front().exclusive->holders = 1U;
  RE_CHECK_EQ(evaluate(envelope, request).reason, StatusCode::RejectedExclusiveConflict);
}

RE_TEST(request_validation_precedes_every_dimension_check) {
  const Envelope envelope = tenant_envelope();
  EvaluationRequest request = request_for(1U, 1ULL * kWatt);
  request.scope.identity.id = "Not-Canonical";
  const EvaluationResult result = evaluate(envelope, request);
  RE_CHECK_EQ(result.stage, RefusalStage::RequestValidation);
  RE_CHECK_EQ(result.reason, StatusCode::InvalidIdentifier);
  RE_CHECK(result.dimensions.empty());
}

RE_TEST(duplicate_dimensions_are_refused_before_arithmetic) {
  const Envelope envelope = tenant_envelope();
  EvaluationRequest request = request_for(1U, 1ULL * kWatt);
  request.dimensions.push_back(request.dimensions.front());
  const EvaluationResult result = evaluate(envelope, request);
  RE_CHECK_EQ(result.reason, StatusCode::DuplicateDimension);
  RE_CHECK(result.dimensions.empty());
}

RE_TEST(determinism_same_inputs_produce_the_same_decision_digest) {
  const Envelope envelope = tenant_envelope();
  const EvaluationRequest request = request_for(1U, 1ULL * kWatt);
  const EvaluationResult first = evaluate(envelope, request);
  const EvaluationResult second = evaluate(envelope, request);
  RE_CHECK(first.decision_digest == second.decision_digest);
  RE_CHECK(first.request_digest == second.request_digest);
  EvaluationRequest changed = request;
  changed.dimensions.front().quantity = 2ULL * kWatt;
  RE_CHECK(evaluate(envelope, changed).request_digest != first.request_digest);
}

RE_TEST(merge_refuses_ambiguous_overlap_and_permits_dominance) {
  Envelope base;
  base.id = "facility-envelope";
  base.revision = 1U;
  base.scope.kind = EnvelopeScopeKind::Facility;
  base.scope.identity.id = "site-one";
  DimensionSpec base_power;
  base_power.kind = DimensionKind::PowerDrawWatts;
  base_power.hard_limit = 200ULL * kWatt;
  base.dimensions.push_back(base_power);
  DimensionSpec base_space;
  base_space.kind = DimensionKind::SpaceRackUnits;
  base_space.hard_limit = 10ULL * kNanounitsPerUnit;
  base.dimensions.push_back(base_space);
  Envelope overlay = tenant_envelope_with_committed(0ULL);
  DimensionSpec tight;
  tight.kind = DimensionKind::PowerDrawWatts;
  tight.hard_limit = 20ULL * kWatt;
  overlay.dimensions.clear();
  overlay.dimensions.push_back(tight);
  const Result<MergeResult> strict = merge_envelopes(base, overlay, MergePolicy::Strict);
  RE_CHECK(!strict.ok());
  RE_CHECK_EQ(strict.status().code(), StatusCode::AmbiguousOverlap);
  const Result<MergeResult> dominated = merge_envelopes(base, overlay, MergePolicy::TightestWins);
  RE_REQUIRE(dominated.ok());
  RE_CHECK_EQ(dominated.value().dominance, MergeDominance::SecondDominates);
  RE_CHECK_EQ(dominated.value().envelope.kind, EnvelopeKind::Composite);
  const DimensionSpec* merged = dominated.value().envelope.find(DimensionKind::PowerDrawWatts);
  RE_REQUIRE(merged != nullptr);
  RE_REQUIRE(merged->hard_limit.has_value());
  RE_CHECK_EQ(*merged->hard_limit, 20ULL * kWatt);
  RE_CHECK_EQ(dominated.value().envelope.dimensions.size(), std::size_t{2});
  // The merge is order independent, so both orders produce the same envelope identity.
  const Result<MergeResult> reversed = merge_envelopes(overlay, base, MergePolicy::TightestWins);
  RE_REQUIRE(reversed.ok());
  RE_CHECK_EQ(reversed.value().envelope.id, dominated.value().envelope.id);
  // Composition is fully order independent: both spellings produce the same envelope, the
  // same canonical bytes, and therefore the same content and record digests. The result is
  // canonicalised rather than recomputed per spelling, so the two cannot drift apart.
  RE_CHECK_EQ(record_digest(reversed.value().envelope), record_digest(dominated.value().envelope));
  RE_CHECK_EQ(content_digest(reversed.value().envelope), content_digest(dominated.value().envelope));
  RE_CHECK_EQ(canonical_envelope(reversed.value().envelope), canonical_envelope(dominated.value().envelope));
}

RE_TEST(merge_refuses_mutually_non_dominant_overlaps) {
  Envelope base;
  base.id = "facility-envelope";
  base.revision = 1U;
  base.scope.kind = EnvelopeScopeKind::Facility;
  base.scope.identity.id = "site-one";
  DimensionSpec base_power;
  base_power.kind = DimensionKind::PowerDrawWatts;
  base_power.hard_limit = 200ULL * kWatt;
  base.dimensions.push_back(base_power);
  DimensionSpec base_space;
  base_space.kind = DimensionKind::SpaceRackUnits;
  base_space.hard_limit = 5ULL * kNanounitsPerUnit;
  base.dimensions.push_back(base_space);
  Envelope overlay = tenant_envelope();
  overlay.dimensions.clear();
  DimensionSpec tight_power;
  tight_power.kind = DimensionKind::PowerDrawWatts;
  tight_power.hard_limit = 20ULL * kWatt;
  overlay.dimensions.push_back(tight_power);
  DimensionSpec loose_space;
  loose_space.kind = DimensionKind::SpaceRackUnits;
  loose_space.hard_limit = 40ULL * kNanounitsPerUnit;
  overlay.dimensions.push_back(loose_space);
  const Result<MergeResult> merged = merge_envelopes(base, overlay, MergePolicy::TightestWins);
  RE_CHECK(!merged.ok());
  RE_CHECK_EQ(merged.status().code(), StatusCode::AmbiguousOverlap);
}

RE_TEST(canonical_envelope_round_trips_exactly) {
  const Envelope envelope = tenant_envelope();
  const Bytes encoded = canonical_envelope(envelope);
  const Result<Envelope> decoded = decode_envelope(encoded);
  RE_REQUIRE(decoded.ok());
  RE_CHECK_EQ(canonical_envelope(decoded.value()), encoded);
  RE_CHECK(record_digest(decoded.value()) == record_digest(envelope));
  // Trailing bytes are refused rather than ignored.
  Bytes extended = encoded;
  extended.push_back(0U);
  RE_CHECK(!decode_envelope(extended).ok());
  Bytes truncated = encoded;
  truncated.resize(encoded.size() - 1U);
  RE_CHECK(!decode_envelope(truncated).ok());
}

RE_TEST(content_digest_ignores_provenance_but_record_digest_does_not) {
  Envelope envelope = tenant_envelope();
  const Digest content_before = content_digest(envelope);
  const Digest record_before = record_digest(envelope);
  envelope.provenance.reason = "a corrected explanation";
  RE_CHECK(content_digest(envelope) == content_before);
  RE_CHECK(record_digest(envelope) != record_before);
  Envelope limits = tenant_envelope();
  DimensionSpec* power = limits.find(DimensionKind::PowerDrawWatts);
  RE_REQUIRE(power != nullptr);
  power->hard_limit = 101ULL * kWatt;
  RE_CHECK(content_digest(limits) != content_before);
  RE_CHECK(record_digest(limits) != record_before);
}

RE_TEST(declaration_order_does_not_change_a_digest) {
  Envelope first = tenant_envelope();
  Envelope second = tenant_envelope();
  std::reverse(second.dimensions.begin(), second.dimensions.end());
  RE_CHECK(content_digest(first) == content_digest(second));
  RE_CHECK(canonical_envelope(first) == canonical_envelope(second));
}

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
