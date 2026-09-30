// Direct coverage of the identity-binding path.
//
// An envelope binds to a tenant, service or facility identity owned by another authority: an
// identifier, a generation and a digest of the identity record that authority publishes. This
// suite proves that the binding is checked exactly as designed, at evaluation time, at
// determination time and at verification time, and that neither a restart nor an idempotent
// replay can bypass it.
//
//  1. a matching generation and digest succeeds, for tenant, service and facility scopes alike
//  2. a stale generation, a stale digest, a foreign identity and an absent confirmation each fail
//     with GenerationBinding / IdentityBindingMismatch, before any arithmetic runs
//  3. an identity digest supplied without an identity identifier never reads as agreement
//  4. a waived binding is an explicit waiver, and it does not license a wrong digest
//  5. service-class and policy digests are checked the same way
//  6. a recorded decision carries the binding that answered, and verification reports a changed
//     generation or a changed digest as IdentityBindingChanged
//  7. a restart and an idempotent replay both re-check the binding rather than bypassing it, and a
//     recorded determination is returned unchanged for the claim that produced it

#include "support/testing.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/service.hpp"

using namespace resource_envelope;

namespace {

constexpr Timestamp kAt = 1767225600000000000LL;
constexpr Nanounits kWatt = kNanounitsPerUnit;

Digest digest_of(std::string_view text) { return Digest(sha256_domain("binding-identity-record", text)); }

const Digest kIdentityDigest = digest_of("identity-generation-3");
const Digest kAdvancedIdentityDigest = digest_of("identity-generation-4");
const Digest kServiceClassDigest = digest_of("service-class-revision-7");
const Digest kPolicyDigest = digest_of("policy-revision-11");

StoreOptions create_options() {
  StoreOptions options;
  options.mode = OpenMode::OpenOrCreate;
  return options;
}

StoreOptions existing_options() {
  StoreOptions options;
  options.mode = OpenMode::OpenExisting;
  return options;
}

class ScratchStore {
 public:
  explicit ScratchStore(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::current_path() / (name + "-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ~ScratchStore() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ScratchStore(const ScratchStore&) = delete;
  ScratchStore& operator=(const ScratchStore&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// An envelope whose arithmetic is fully determined: a 100 W bound with 10 W reserved and 30 W of
// declared committed usage leaves a residual of exactly 60 W, so a 50 W request is granted when
// and only when every earlier refusal stage passes.
Envelope bound_envelope(EnvelopeScopeKind kind, const std::string& identity, std::uint64_t generation,
                        const Digest& identity_digest, bool require_binding) {
  Envelope envelope;
  envelope.id = identity + "-envelope";
  envelope.revision = 1U;
  envelope.kind = EnvelopeKind::Declared;
  envelope.scope.kind = kind;
  envelope.scope.identity.id = identity;
  envelope.scope.identity.generation = generation;
  envelope.site_id = "site-one";
  envelope.identity_digest = identity_digest;
  envelope.require_binding_confirmation = require_binding;
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "operator";
  envelope.provenance.declared_at = kAt;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = 100ULL * kWatt;
  power.reserved = 10ULL * kWatt;
  CommittedUsage committed;
  committed.status = MeasureStatus::Measured;
  committed.value = 30ULL * kWatt;
  committed.observed_at = kAt;
  committed.source = "facility-capacity";
  power.committed = committed;
  envelope.dimensions.push_back(power);
  return envelope;
}

EvaluationRequest request_for(const Envelope& envelope, const std::string& key) {
  EvaluationRequest request;
  request.scope = envelope.scope;
  request.at = kAt;
  request.idempotency_key = key;
  DimensionRequest dimension;
  dimension.kind = DimensionKind::PowerDrawWatts;
  dimension.quantity = 50ULL * kWatt;
  request.dimensions.push_back(dimension);
  return request;
}

void expect_generation_refusal(const EvaluationResult& result, StatusCode code) {
  RE_CHECK_EQ(result.outcome, Outcome::Denied);
  RE_CHECK_EQ(result.reason, code);
  RE_CHECK_EQ(result.stage, RefusalStage::GenerationBinding);
  // The binding is checked before the arithmetic, so a request that does not confirm it never
  // reaches a dimension and no residual is ever computed for it.
  RE_CHECK(result.dimensions.empty());
}

struct ScopeCase {
  EnvelopeScopeKind kind;
  const char* identity;
};

const ScopeCase kScopes[] = {{EnvelopeScopeKind::Tenant, "tenant-a"},
                             {EnvelopeScopeKind::Service, "service-billing"},
                             {EnvelopeScopeKind::Facility, "site-one"}};

}  // namespace

// ---------------------------------------------------------------------------
// Evaluation-time binding
// ---------------------------------------------------------------------------
RE_TEST(a_matching_generation_and_digest_is_confirmed_for_every_scope_kind) {
  for (const ScopeCase& scope : kScopes) {
    const Envelope envelope =
        bound_envelope(scope.kind, scope.identity, 3U, kIdentityDigest, true);
    EvaluationRequest request = request_for(envelope, "matching");
    request.identity.identity = envelope.scope.identity;
    request.identity.digest = kIdentityDigest;
    const EvaluationResult result = evaluate(envelope, request);
    // The confirmed binding passes its stage and the request is decided on its merits, so the
    // residual arithmetic is reached: 100 - 10 - 30 = 60, and 50 W fits.
    RE_CHECK_EQ(result.outcome, Outcome::Granted);
    RE_CHECK_EQ(result.reason, StatusCode::Ok);
    RE_CHECK_EQ(result.stage, RefusalStage::None);
    RE_REQUIRE(result.dimensions.size() == std::size_t{1});
    RE_REQUIRE(result.dimensions.front().residual.has_value());
    RE_CHECK_EQ(*result.dimensions.front().residual, 60ULL * kWatt);
  }
}

RE_TEST(a_stale_generation_or_digest_is_refused_at_the_generation_stage) {
  for (const ScopeCase& scope : kScopes) {
    const Envelope envelope = bound_envelope(scope.kind, scope.identity, 3U, kIdentityDigest, true);

    // The owning authority advanced the identity: the generation no longer matches.
    EvaluationRequest advanced = request_for(envelope, "stale-generation");
    advanced.identity.identity = envelope.scope.identity;
    advanced.identity.identity.generation = 4U;
    advanced.identity.digest = kIdentityDigest;
    expect_generation_refusal(evaluate(envelope, advanced), StatusCode::IdentityBindingMismatch);

    // The identity record changed but the generation did not: the digest no longer matches.
    EvaluationRequest changed_record = request_for(envelope, "stale-digest");
    changed_record.identity.identity = envelope.scope.identity;
    changed_record.identity.digest = kAdvancedIdentityDigest;
    expect_generation_refusal(evaluate(envelope, changed_record), StatusCode::IdentityBindingMismatch);

    // A different identity at the same generation is foreign authority, not this envelope's.
    EvaluationRequest foreign = request_for(envelope, "foreign");
    foreign.identity.identity.id = "someone-else";
    foreign.identity.identity.generation = 3U;
    foreign.identity.digest = kIdentityDigest;
    expect_generation_refusal(evaluate(envelope, foreign), StatusCode::IdentityBindingMismatch);

    // An absent confirmation is never read as agreement...
    const EvaluationRequest absent = request_for(envelope, "absent");
    expect_generation_refusal(evaluate(envelope, absent), StatusCode::IdentityBindingMismatch);

    // ...and an identity digest supplied without an identity identifier is not agreement either:
    // the generation stage refuses first and the provider availability stage records that the
    // authority was never consulted.
    EvaluationRequest digest_only = request_for(envelope, "digest-only");
    digest_only.identity.digest = kIdentityDigest;
    const EvaluationResult result = evaluate(envelope, digest_only);
    expect_generation_refusal(result, StatusCode::IdentityBindingMismatch);
    RE_CHECK_EQ(result.secondary.size(), std::size_t{1});
    if (!result.secondary.empty()) {
      RE_CHECK_EQ(result.secondary.front(), StatusCode::RequiredBindingUnavailable);
    }
  }
}

RE_TEST(a_waived_binding_waives_confirmation_and_nothing_else) {
  const Envelope waived = bound_envelope(EnvelopeScopeKind::Tenant, "tenant-a", 3U, kIdentityDigest, false);
  // An operator that has not wired an identity authority declares the waiver explicitly, and the
  // request then proceeds without a snapshot.
  const EvaluationResult without_snapshot = evaluate(waived, request_for(waived, "waived"));
  RE_CHECK_EQ(without_snapshot.outcome, Outcome::Granted);
  RE_CHECK_EQ(without_snapshot.reason, StatusCode::Ok);
  // The waiver is not a licence to supply a wrong digest: a declared binding is compared whenever
  // the caller states one.
  EvaluationRequest wrong = request_for(waived, "waived-wrong");
  wrong.identity.identity = waived.scope.identity;
  wrong.identity.digest = kAdvancedIdentityDigest;
  expect_generation_refusal(evaluate(waived, wrong), StatusCode::IdentityBindingMismatch);
  // A digest with no identifier is an unconsulted authority even under the waiver, and an
  // unavailable authority is indeterminate rather than granted.
  EvaluationRequest digest_only = request_for(waived, "waived-digest-only");
  digest_only.identity.digest = kIdentityDigest;
  const EvaluationResult result = evaluate(waived, digest_only);
  RE_CHECK_EQ(result.outcome, Outcome::Indeterminate);
  RE_CHECK_EQ(result.reason, StatusCode::RequiredBindingUnavailable);
  RE_CHECK_EQ(result.stage, RefusalStage::ProviderAvailability);
}

RE_TEST(an_envelope_with_no_declared_binding_requires_nothing) {
  // An unknown digest is explicitly not a binding: there is nothing to confirm, so the request is
  // decided on its merits whether or not the caller states an identity.
  const Envelope envelope = bound_envelope(EnvelopeScopeKind::Tenant, "tenant-a", 3U, Digest::unknown(), true);
  RE_CHECK_EQ(evaluate(envelope, request_for(envelope, "unbound")).outcome, Outcome::Granted);
  EvaluationRequest stated = request_for(envelope, "unbound-stated");
  stated.identity.identity = envelope.scope.identity;
  stated.identity.digest = kIdentityDigest;
  RE_CHECK_EQ(evaluate(envelope, stated).outcome, Outcome::Granted);
}

RE_TEST(service_class_and_policy_digests_are_checked_the_same_way) {
  Envelope envelope = bound_envelope(EnvelopeScopeKind::Tenant, "tenant-a", 3U, kIdentityDigest, true);
  envelope.service_class_digest = kServiceClassDigest;
  envelope.policy_digest = kPolicyDigest;

  EvaluationRequest confirming = request_for(envelope, "context");
  confirming.identity.identity = envelope.scope.identity;
  confirming.identity.digest = kIdentityDigest;
  confirming.service_class.state = ResolutionState::Provided;
  confirming.service_class.digest = kServiceClassDigest;
  confirming.policy.state = ResolutionState::Provided;
  confirming.policy.digest = kPolicyDigest;
  RE_CHECK_EQ(evaluate(envelope, confirming).outcome, Outcome::Granted);

  // An unstated referenced context is a refusal, and every later failure is preserved as evidence:
  // the missing policy and the fact that the service class authority was never consulted.
  EvaluationRequest unstated = request_for(envelope, "context-unstated");
  unstated.identity.identity = envelope.scope.identity;
  unstated.identity.digest = kIdentityDigest;
  const EvaluationResult missing_context = evaluate(envelope, unstated);
  RE_CHECK_EQ(missing_context.outcome, Outcome::Denied);
  RE_CHECK_EQ(missing_context.reason, StatusCode::ServiceClassBindingMismatch);
  RE_CHECK_EQ(missing_context.stage, RefusalStage::ContextBinding);
  RE_CHECK(std::find(missing_context.secondary.begin(), missing_context.secondary.end(),
                     StatusCode::PolicyBindingMismatch) != missing_context.secondary.end());
  RE_CHECK(std::find(missing_context.secondary.begin(), missing_context.secondary.end(),
                     StatusCode::RequiredBindingUnavailable) != missing_context.secondary.end());

  // A different revision of the service class is refused exactly as a different identity is.
  EvaluationRequest stale_class = confirming;
  stale_class.service_class.digest = digest_of("service-class-revision-8");
  RE_CHECK_EQ(evaluate(envelope, stale_class).reason, StatusCode::ServiceClassBindingMismatch);

  // An authority that could not be consulted is never agreement. It is reported at the binding
  // stage, because a binding the caller did not supply cannot be compared, and the fact that the
  // authority was unavailable is preserved as secondary evidence rather than lost.
  EvaluationRequest unavailable = confirming;
  unavailable.service_class.state = ResolutionState::Unavailable;
  const EvaluationResult unconsulted = evaluate(envelope, unavailable);
  RE_CHECK_EQ(unconsulted.outcome, Outcome::Denied);
  RE_CHECK_EQ(unconsulted.reason, StatusCode::ServiceClassBindingMismatch);
  RE_CHECK_EQ(unconsulted.stage, RefusalStage::ContextBinding);
  RE_CHECK(std::find(unconsulted.secondary.begin(), unconsulted.secondary.end(),
                     StatusCode::RequiredBindingUnavailable) != unconsulted.secondary.end());

  // "Not current" is a definite statement by the owning authority, not an absence of one.
  EvaluationRequest not_current = confirming;
  not_current.policy.state = ResolutionState::NotCurrent;
  RE_CHECK_EQ(evaluate(envelope, not_current).reason, StatusCode::PolicyBindingMismatch);
}

// ---------------------------------------------------------------------------
// Determination-time binding, against a live store
// ---------------------------------------------------------------------------
namespace {

Envelope store_envelope(const std::string& id, const std::string& identity, std::uint64_t generation,
                        const Digest& identity_digest, bool require_binding, Nanounits limit) {
  Envelope envelope;
  envelope.id = id;
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = identity;
  envelope.scope.identity.generation = generation;
  envelope.site_id = "site-one";
  envelope.identity_digest = identity_digest;
  envelope.require_binding_confirmation = require_binding;
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "operator";
  envelope.provenance.declared_at = kAt;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = limit;
  envelope.dimensions.push_back(power);
  return envelope;
}

DeclareInput declaration_for(const Envelope& envelope, const std::string& key, Timestamp at) {
  DeclareInput input;
  input.envelope = envelope;
  input.idempotency_key = key;
  input.requested_at = at;
  return input;
}

void record_usage(EnvelopeService& service, const std::string& envelope_id, Nanounits watts,
                  const std::string& key, Timestamp at) {
  UsageInput usage;
  usage.envelope_id = envelope_id;
  usage.idempotency_key = key;
  usage.requested_at = at;
  UsageDelta delta;
  delta.envelope_id = envelope_id;
  delta.kind = DimensionKind::PowerDrawWatts;
  delta.delta = static_cast<std::int64_t>(watts);
  delta.source = "facility-capacity";
  usage.deltas.push_back(delta);
  const Result<UsageResult> recorded = service.record_usage(usage);
  RE_REQUIRE(recorded.ok());
}

}  // namespace

RE_TEST(the_service_records_the_binding_that_answered_and_refuses_one_that_did_not) {
  const ScratchStore scratch("re-binding-service");
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  const Envelope envelope =
      store_envelope("binding-envelope", "tenant-a", 3U, kIdentityDigest, true, 100ULL * kWatt);
  RE_REQUIRE(service.value()->declare(declaration_for(envelope, "declare-1", kAt)).ok());
  record_usage(*service.value(), "binding-envelope", 30ULL * kWatt, "usage-1", kAt + 1LL);

  EvaluationRequest request = request_for(envelope, "authorize-1");
  request.identity.identity = envelope.scope.identity;
  request.identity.digest = kIdentityDigest;
  const Result<AuthorizeResult> granted = service.value()->authorize(request);
  RE_REQUIRE(granted.ok());
  RE_CHECK_EQ(granted.value().evaluation.outcome, Outcome::Granted);
  RE_CHECK_EQ(granted.value().evaluation.reason, StatusCode::Ok);
  // The record names the identity, its generation and the digest it was bound to, and states that
  // the caller confirmed it. A decision that relied on a generation is only explainable if it
  // records which generation that was.
  RE_CHECK_EQ(granted.value().record.identity, envelope.scope.identity);
  RE_CHECK_EQ(granted.value().record.identity.generation, 3ULL);
  RE_CHECK_EQ(granted.value().record.identity_digest, kIdentityDigest);
  RE_CHECK(granted.value().record.identity_checked);
  RE_CHECK(granted.value().record.grant_digest.known());
  const std::string granted_decision = granted.value().record.decision_id;

  // A stale digest is refused and the refusal is itself recorded, with the same binding named.
  EvaluationRequest stale = request;
  stale.idempotency_key = "authorize-stale";
  stale.identity.digest = kAdvancedIdentityDigest;
  const Result<AuthorizeResult> refused = service.value()->authorize(stale);
  RE_REQUIRE(refused.ok());
  RE_CHECK_EQ(refused.value().evaluation.outcome, Outcome::Denied);
  RE_CHECK_EQ(refused.value().evaluation.reason, StatusCode::IdentityBindingMismatch);
  RE_CHECK_EQ(refused.value().evaluation.stage, RefusalStage::GenerationBinding);
  RE_CHECK_EQ(refused.value().record.identity_digest, kIdentityDigest);
  RE_CHECK(refused.value().record.identity_checked);
  RE_CHECK(!refused.value().record.grant_digest.known());

  // The recorded decisions are durable, and the binding each one relied on survives a restart.
  service.value().reset();
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options());
  RE_REQUIRE(reopened.ok());
  const Result<DecisionRecord> reread = reopened.value()->get_decision(granted_decision);
  RE_REQUIRE(reread.ok());
  RE_CHECK_EQ(reread.value().identity, envelope.scope.identity);
  RE_CHECK_EQ(reread.value().identity_digest, kIdentityDigest);
  RE_CHECK(reread.value().identity_checked);
  RE_CHECK_EQ(reread.value().outcome, Outcome::Granted);
}

RE_TEST(verification_reports_a_changed_generation_or_digest) {
  const ScratchStore scratch("re-binding-verify");
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  const Envelope envelope =
      store_envelope("binding-envelope", "tenant-a", 3U, kIdentityDigest, true, 100ULL * kWatt);
  RE_REQUIRE(service.value()->declare(declaration_for(envelope, "declare-1", kAt)).ok());
  record_usage(*service.value(), "binding-envelope", 30ULL * kWatt, "usage-1", kAt + 1LL);
  EvaluationRequest request = request_for(envelope, "authorize-1");
  request.identity.identity = envelope.scope.identity;
  request.identity.digest = kIdentityDigest;
  const Result<AuthorizeResult> granted = service.value()->authorize(request);
  RE_REQUIRE(granted.ok());
  RE_REQUIRE(granted.value().evaluation.outcome == Outcome::Granted);

  VerifyInput verify;
  verify.decision_id = granted.value().record.decision_id;
  const Result<VerifyResult> valid = service.value()->verify_authority(verify);
  RE_REQUIRE(valid.ok());
  RE_CHECK(valid.value().verdict.valid());
  RE_CHECK_EQ(valid.value().verdict.state, AuthorityState::Valid);

  // The caller reports the identity it holds. A generation the envelope is not bound to invalidates
  // the determination, ...
  VerifyInput advanced = verify;
  advanced.identity = IdentitySnapshot{IdentityRef{"tenant-a", 4U}, kIdentityDigest};
  const Result<VerifyResult> advanced_verdict = service.value()->verify_authority(advanced);
  RE_REQUIRE(advanced_verdict.ok());
  RE_CHECK_EQ(advanced_verdict.value().verdict.state, AuthorityState::IdentityBindingChanged);
  RE_CHECK(!advanced_verdict.value().verdict.valid());

  // ... a digest that no longer matches the binding invalidates it exactly as a generation does, ...
  VerifyInput changed = verify;
  changed.identity = IdentitySnapshot{IdentityRef{"tenant-a", 3U}, kAdvancedIdentityDigest};
  const Result<VerifyResult> changed_verdict = service.value()->verify_authority(changed);
  RE_REQUIRE(changed_verdict.ok());
  RE_CHECK_EQ(changed_verdict.value().verdict.state, AuthorityState::IdentityBindingChanged);
  RE_CHECK(!changed_verdict.value().verdict.valid());

  // ... and an identity reference supplied without the digest it was bound to is not agreement
  // either: the caller has not stated the digest the envelope is bound to.
  VerifyInput partial = verify;
  partial.identity = IdentitySnapshot{IdentityRef{"tenant-a", 3U}, Digest::unknown()};
  const Result<VerifyResult> partial_verdict = service.value()->verify_authority(partial);
  RE_REQUIRE(partial_verdict.ok());
  RE_CHECK_EQ(partial_verdict.value().verdict.state, AuthorityState::IdentityBindingChanged);

  // The matching snapshot is the only input that leaves the determination valid, and the same
  // snapshot continues to verify after a restart.
  service.value().reset();
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options());
  RE_REQUIRE(reopened.ok());
  const Result<VerifyResult> after_restart = reopened.value()->verify_authority(advanced);
  RE_REQUIRE(after_restart.ok());
  RE_CHECK_EQ(after_restart.value().verdict.state, AuthorityState::IdentityBindingChanged);
  const Result<VerifyResult> still_valid = reopened.value()->verify_authority(verify);
  RE_REQUIRE(still_valid.ok());
  RE_CHECK(still_valid.value().verdict.valid());
}

RE_TEST(restart_and_replay_cannot_bypass_the_binding) {
  const ScratchStore scratch("re-binding-replay");
  Digest decision_digest;
  std::uint64_t decision_sequence = 0;
  std::uint64_t epoch_after_grant = 0;
  {
    const Result<std::unique_ptr<EnvelopeService>> service =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(service.ok());
    const Envelope envelope =
        store_envelope("binding-envelope", "tenant-a", 3U, kIdentityDigest, true, 100ULL * kWatt);
    RE_REQUIRE(service.value()->declare(declaration_for(envelope, "declare-1", kAt)).ok());
    record_usage(*service.value(), "binding-envelope", 30ULL * kWatt, "usage-1", kAt + 1LL);
    EvaluationRequest request = request_for(envelope, "authorize-1");
    request.identity.identity = envelope.scope.identity;
    request.identity.digest = kIdentityDigest;
    const Result<AuthorizeResult> granted = service.value()->authorize(request);
    RE_REQUIRE(granted.ok());
    RE_REQUIRE(granted.value().evaluation.outcome == Outcome::Granted);
    decision_digest = granted.value().decision_digest;
    decision_sequence = granted.value().record.sequence;
    const Result<StoreStatistics> stats = service.value()->statistics();
    RE_REQUIRE(stats.ok());
    epoch_after_grant = stats.value().epoch;
  }

  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), existing_options());
  RE_REQUIRE(service.ok());
  const Envelope envelope =
      store_envelope("binding-envelope", "tenant-a", 3U, kIdentityDigest, true, 100ULL * kWatt);
  EvaluationRequest request = request_for(envelope, "authorize-1");
  request.identity.identity = envelope.scope.identity;
  request.identity.digest = kIdentityDigest;

  // A retry after a restart returns the recorded determination unchanged: it is not re-decided
  // against the authority as it stands now, and it appends nothing.
  const std::uint64_t decisions_before = service.value()->statistics().value().decision_count;
  const Result<AuthorizeResult> replayed = service.value()->authorize(request);
  RE_REQUIRE(replayed.ok());
  RE_CHECK(replayed.value().replayed);
  RE_CHECK_EQ(replayed.value().decision_digest, decision_digest);
  RE_CHECK_EQ(replayed.value().record.sequence, decision_sequence);
  RE_CHECK_EQ(replayed.value().record.identity_digest, kIdentityDigest);
  RE_CHECK_EQ(service.value()->statistics().value().decision_count, decisions_before);
  RE_CHECK_EQ(service.value()->statistics().value().epoch, epoch_after_grant);

  // The same key with a different binding is a different request, so it cannot be used to make the
  // recorded claim cover a binding the envelope never confirmed.
  EvaluationRequest tampered = request;
  tampered.identity.digest = kAdvancedIdentityDigest;
  const Result<AuthorizeResult> conflict = service.value()->authorize(tampered);
  RE_CHECK(!conflict.ok());
  if (!conflict.ok()) RE_CHECK_EQ(conflict.status().code(), StatusCode::IdempotencyConflict);

  // A fresh key with a stale binding is refused against the current revision after the restart.
  EvaluationRequest fresh = tampered;
  fresh.idempotency_key = "authorize-after-restart";
  const Result<AuthorizeResult> refused = service.value()->authorize(fresh);
  RE_REQUIRE(refused.ok());
  RE_CHECK(!refused.value().replayed);
  RE_CHECK_EQ(refused.value().evaluation.reason, StatusCode::IdentityBindingMismatch);
  RE_CHECK_EQ(refused.value().evaluation.stage, RefusalStage::GenerationBinding);

  // An identity authority that advances the identity record changes the binding: the revision
  // carries the new digest, the old digest is refused against it, and the new one is confirmed.
  ReviseInput revise;
  revise.envelope = envelope;
  revise.envelope.identity_digest = kAdvancedIdentityDigest;
  revise.idempotency_key = "revise-1";
  revise.requested_at = kAt + 100LL;
  revise.expected_current_revision = 1U;
  const Result<EnvelopeRevision> revised = service.value()->revise(revise);
  RE_REQUIRE(revised.ok());
  EvaluationRequest old_digest = request;
  old_digest.idempotency_key = "authorize-old-digest";
  const Result<AuthorizeResult> refused_old = service.value()->authorize(old_digest);
  RE_REQUIRE(refused_old.ok());
  RE_CHECK_EQ(refused_old.value().evaluation.reason, StatusCode::IdentityBindingMismatch);
  RE_CHECK_EQ(refused_old.value().evaluation.stage, RefusalStage::GenerationBinding);
  EvaluationRequest new_digest = old_digest;
  new_digest.idempotency_key = "authorize-new-digest";
  new_digest.identity.digest = kAdvancedIdentityDigest;
  const Result<AuthorizeResult> reconfirmed = service.value()->authorize(new_digest);
  RE_REQUIRE(reconfirmed.ok());
  RE_CHECK_EQ(reconfirmed.value().evaluation.outcome, Outcome::Granted);
  RE_CHECK_EQ(reconfirmed.value().record.identity_digest, kAdvancedIdentityDigest);

  // Retiring the envelope does not lose the answer to a claim that was already decided: replay
  // resolution precedes staleness, and the recorded determination is returned as recorded.
  TombstoneInput retire;
  retire.envelope_id = "binding-envelope";
  retire.reason = "decommissioned";
  retire.request_id = 9U;
  retire.requested_at = kAt + 200LL;
  RE_REQUIRE(service.value()->tombstone(retire).ok());
  const Result<AuthorizeResult> after_retirement = service.value()->authorize(request);
  RE_REQUIRE(after_retirement.ok());
  RE_CHECK(after_retirement.value().replayed);
  RE_CHECK_EQ(after_retirement.value().decision_digest, decision_digest);
  RE_CHECK_EQ(after_retirement.value().record.outcome, Outcome::Granted);
}

RE_TEST(identity_generations_are_separate_authority_domains) {
  // A generation is part of the scope, so advancing it does not silently re-point an existing
  // envelope at a new identity: the generation the request claims is the generation that answers,
  // and the generation before it remains bound to the envelope that declared it.
  const ScratchStore scratch("re-binding-generations");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  const Envelope third =
      store_envelope("binding-envelope-3", "tenant-a", 3U, kIdentityDigest, true, 100ULL * kWatt);
  const Envelope fourth = store_envelope("binding-envelope-4", "tenant-a", 4U,
                                         kAdvancedIdentityDigest, true, 200ULL * kWatt);
  RE_REQUIRE(service.value()->declare(declaration_for(third, "declare-3", kAt)).ok());
  RE_REQUIRE(service.value()->declare(declaration_for(fourth, "declare-4", kAt + 1LL)).ok());
  record_usage(*service.value(), "binding-envelope-3", 30ULL * kWatt, "usage-3", kAt + 2LL);
  record_usage(*service.value(), "binding-envelope-4", 120ULL * kWatt, "usage-4", kAt + 3LL);

  EvaluationRequest request = request_for(third, "authorize-3");
  request.identity.identity = third.scope.identity;
  request.identity.digest = kIdentityDigest;
  const Result<AuthorizeResult> at_third = service.value()->authorize(request);
  RE_REQUIRE(at_third.ok());
  RE_CHECK_EQ(at_third.value().record.envelope_id, std::string("binding-envelope-3"));
  RE_CHECK_EQ(at_third.value().record.identity.generation, 3ULL);
  RE_CHECK_EQ(at_third.value().record.identity_digest, kIdentityDigest);
  RE_REQUIRE(at_third.value().evaluation.dimensions.size() == std::size_t{1});
  RE_REQUIRE(at_third.value().evaluation.dimensions.front().residual.has_value());
  RE_CHECK_EQ(*at_third.value().evaluation.dimensions.front().residual, 70ULL * kWatt);

  EvaluationRequest advanced = request_for(fourth, "authorize-4");
  advanced.identity.identity = fourth.scope.identity;
  advanced.identity.digest = kAdvancedIdentityDigest;
  const Result<AuthorizeResult> at_fourth = service.value()->authorize(advanced);
  RE_REQUIRE(at_fourth.ok());
  RE_CHECK_EQ(at_fourth.value().record.envelope_id, std::string("binding-envelope-4"));
  RE_CHECK_EQ(at_fourth.value().record.identity.generation, 4ULL);
  RE_CHECK_EQ(at_fourth.value().record.identity_digest, kAdvancedIdentityDigest);
  RE_REQUIRE(at_fourth.value().evaluation.dimensions.size() == std::size_t{1});
  RE_REQUIRE(at_fourth.value().evaluation.dimensions.front().residual.has_value());
  RE_CHECK_EQ(*at_fourth.value().evaluation.dimensions.front().residual, 80ULL * kWatt);

  // The digest of one generation is not accepted for the other, even though the identity
  // identifier is the same: a generation is not a presentation detail.
  EvaluationRequest crossed = advanced;
  crossed.idempotency_key = "authorize-crossed";
  crossed.identity.digest = kIdentityDigest;
  const Result<AuthorizeResult> refused = service.value()->authorize(crossed);
  RE_REQUIRE(refused.ok());
  RE_CHECK_EQ(refused.value().evaluation.reason, StatusCode::IdentityBindingMismatch);
  RE_CHECK_EQ(refused.value().evaluation.stage, RefusalStage::GenerationBinding);
}
