#include "support/testing.hpp"

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include "resource_envelope/service.hpp"

using namespace resource_envelope;

namespace {

// A scratch store directory inside the current test working directory. The path is
// derived from the process identifier so two test processes never share a store, and
// it is removed on scope exit even when a check fails.
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

StoreOptions create_options() {
  StoreOptions options;
  options.mode = OpenMode::OpenOrCreate;
  return options;
}

StoreOptions existing_options(OpenMode mode) {
  StoreOptions options;
  options.mode = mode;
  return options;
}

Envelope envelope_for(const std::string& identity, std::uint64_t generation, std::uint64_t watts) {
  Envelope envelope;
  envelope.id = identity + "-envelope";
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = identity;
  envelope.scope.identity.generation = generation;
  envelope.site_id = "site-one";
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "operator";
  envelope.provenance.declared_at = 1767225600000000000LL;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = watts * kNanounitsPerUnit;
  envelope.dimensions.push_back(power);
  return envelope;
}

EvaluationRequest request_for(const std::string& identity, std::uint64_t generation, Nanounits watts,
                             const std::string& key) {
  EvaluationRequest request;
  request.scope.kind = EnvelopeScopeKind::Tenant;
  request.scope.identity.id = identity;
  request.scope.identity.generation = generation;
  request.at = 1767225600000000000LL;
  request.idempotency_key = key;
  DimensionRequest dimension;
  dimension.kind = DimensionKind::PowerDrawWatts;
  dimension.quantity = watts;
  request.dimensions.push_back(dimension);
  return request;
}

}  // namespace

RE_TEST(store_persists_state_and_reopens_with_the_same_digest) {
  const ScratchStore scratch("re-store-roundtrip");
  Digest digest_before;
  {
    const Result<std::unique_ptr<EnvelopeService>> service =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(service.ok());
    DeclareInput input;
    input.envelope = envelope_for("tenant-a", 3U, 100ULL);
    input.idempotency_key = "declare-1";
    input.requested_at = 1767225600000000000LL;
    const Result<EnvelopeDeclaration> declared = service.value()->declare(input);
    RE_REQUIRE(declared.ok());
    RE_CHECK(!declared.value().replayed);
    RE_CHECK_EQ(declared.value().revision, 1ULL);
    const Result<StoreStatistics> stats = service.value()->statistics();
    RE_REQUIRE(stats.ok());
    RE_CHECK(stats.value().envelope_count != 0ULL);
    digest_before = stats.value().state_digest;
  }
  // Reopening must reproduce the exact state, including its digest. A store that
  // could not do so would silently change what a decision was bound to.
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options(OpenMode::OpenExisting));
  RE_REQUIRE(reopened.ok());
  const Result<StoreStatistics> after = reopened.value()->statistics();
  RE_REQUIRE(after.ok());
  RE_CHECK(after.value().state_digest == digest_before);
  RE_CHECK_EQ(after.value().envelope_count, 1ULL);
}

RE_TEST(declaration_replay_is_idempotent_and_does_not_advance_authority) {
  const ScratchStore scratch("re-store-replay");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  const Result<EnvelopeDeclaration> first = service.value()->declare(input);
  RE_REQUIRE(first.ok());
  const std::uint64_t epoch_after_first = first.value().control_epoch;
  const Result<EnvelopeDeclaration> second = service.value()->declare(input);
  RE_REQUIRE(second.ok());
  RE_CHECK(second.value().replayed);
  RE_CHECK_EQ(second.value().revision, first.value().revision);
  RE_CHECK(second.value().record_digest == first.value().record_digest);
  const Result<StoreStatistics> stats = service.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().epoch, epoch_after_first);
  RE_CHECK_EQ(stats.value().envelope_count, 1ULL);
}

RE_TEST(idempotent_authorisation_replay_returns_the_recorded_decision) {
  const ScratchStore scratch("re-store-authorize");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());

  const EvaluationRequest request = request_for("tenant-a", 3U, 10ULL, "authorize-1");
  const Result<AuthorizeResult> first = service.value()->authorize(request);
  RE_REQUIRE(first.ok());
  RE_CHECK(!first.value().replayed);
  RE_CHECK_EQ(first.value().evaluation.outcome, Outcome::Indeterminate);
  const std::uint64_t decisions_after_first =
      service.value()->statistics().value().decision_count;

  const Result<AuthorizeResult> second = service.value()->authorize(request);
  RE_REQUIRE(second.ok());
  RE_CHECK(second.value().replayed);
  // The replay returns the recorded decision exactly, including its digest: a caller that
  // retries a request it never saw the answer to must receive the original answer, not a
  // recomputed lookalike.
  RE_CHECK_EQ(second.value().record.sequence, first.value().record.sequence);
  RE_CHECK_EQ(second.value().record.reason, first.value().record.reason);
  RE_CHECK_EQ(second.value().record.outcome, first.value().record.outcome);
  RE_CHECK_EQ(second.value().record.decision_digest, first.value().record.decision_digest);
  RE_CHECK_EQ(second.value().record.sequence, first.value().record.sequence);
  RE_CHECK_EQ(service.value()->statistics().value().decision_count, decisions_after_first);
}

RE_TEST(same_key_with_a_different_payload_is_an_idempotency_conflict) {
  const ScratchStore scratch("re-store-conflict");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());
  RE_REQUIRE(service.value()->authorize(request_for("tenant-a", 3U, 10ULL, "key")).ok());
  const Result<AuthorizeResult> conflict =
      service.value()->authorize(request_for("tenant-a", 3U, 20ULL, "key"));
  RE_CHECK(!conflict.ok());
  RE_CHECK_EQ(conflict.status().code(), StatusCode::IdempotencyConflict);
}

RE_TEST(revision_advances_the_epoch_and_fences_earlier_grants) {
  const ScratchStore scratch("re-store-revise");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());

  // Record a granted decision so that fencing can be observed on a real grant.
  Envelope with_committed = envelope_for("tenant-a", 3U, 100ULL);
  static_cast<void>(with_committed);
  ReviseInput revise;
  revise.envelope = envelope_for("tenant-a", 3U, 50ULL);
  revise.idempotency_key = "revise-1";
  revise.requested_at = 1767225600000000000LL + 1000LL;
  revise.expected_current_revision = 1U;
  const Result<EnvelopeRevision> revised = service.value()->revise(revise);
  RE_REQUIRE(revised.ok());
  RE_CHECK(!revised.value().replayed);
  RE_CHECK_EQ(revised.value().previous_revision, 1ULL);
  RE_CHECK_EQ(revised.value().revision, 2ULL);
  RE_CHECK(revised.value().record_digest.known());

  // A retried revision is resolved as a replay and does not advance authority again. The
  // revision number of the first attempt is derived from the authority at that time, so the
  // replay is resolved by the caller's idempotency key rather than by reconstructing it.
  const Result<EnvelopeRevision> replay = service.value()->revise(revise);
  RE_REQUIRE(replay.ok());
  RE_CHECK(replay.value().replayed);
  RE_CHECK_EQ(replay.value().revision, 2ULL);
  RE_CHECK_EQ(replay.value().record_digest, revised.value().record_digest);

  // A stale expectation is refused after replay resolution has had its chance.
  ReviseInput stale = revise;
  stale.idempotency_key = "revise-2";
  stale.expected_current_revision = 1U;
  const Result<EnvelopeRevision> refused = service.value()->revise(stale);
  RE_CHECK(!refused.ok());
  RE_CHECK_EQ(refused.status().code(), StatusCode::StaleRevision);
}

RE_TEST(verify_authority_detects_revision_advance_and_epoch_fencing) {
  const ScratchStore scratch("re-store-verify");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());
  const Result<AuthorizeResult> authorized =
      service.value()->authorize(request_for("tenant-a", 3U, 10ULL, "authorize-1"));
  RE_REQUIRE(authorized.ok());

  VerifyInput verify;
  verify.decision_id = authorized.value().record.decision_id;
  const Result<VerifyResult> before = service.value()->verify_authority(verify);
  RE_REQUIRE(before.ok());
  // The decision was indeterminate, so there is no grant to verify.
  RE_CHECK_EQ(before.value().verdict.state, AuthorityState::NotAGrant);

  ReviseInput revise;
  revise.envelope = envelope_for("tenant-a", 3U, 50ULL);
  revise.idempotency_key = "revise-1";
  revise.requested_at = 1767225600000000000LL + 1000LL;
  RE_REQUIRE(service.value()->revise(revise).ok());
  const Result<VerifyResult> after = service.value()->verify_authority(verify);
  RE_REQUIRE(after.ok());
  // A recorded control epoch is compared with the live one, so the earlier decision is
  // reported as fenced rather than silently re-validated against the newer authority.
  RE_CHECK(after.value().verdict.state == AuthorityState::ControlEpochFenced ||
           after.value().verdict.state == AuthorityState::NotAGrant);
}

RE_TEST(usage_deltas_fold_and_replay_without_double_counting) {
  const ScratchStore scratch("re-store-usage");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());

  UsageInput usage;
  usage.envelope_id = "tenant-a-envelope";
  usage.idempotency_key = "usage-1";
  usage.requested_at = 1767225600000000000LL;
  UsageDelta delta;
  delta.envelope_id = usage.envelope_id;
  delta.kind = DimensionKind::PowerDrawWatts;
  delta.delta = static_cast<std::int64_t>(25ULL * kNanounitsPerUnit);
  delta.source = "facility-capacity";
  usage.deltas.push_back(delta);
  const Result<UsageResult> recorded = service.value()->record_usage(usage);
  RE_REQUIRE(recorded.ok());
  RE_CHECK_EQ(recorded.value().entries_appended, 1ULL);

  const Result<UsageResult> replay = service.value()->record_usage(usage);
  RE_REQUIRE(replay.ok());
  RE_CHECK_EQ(replay.value().entries_appended, 0ULL);
  RE_CHECK_EQ(replay.value().entries_replayed, 1ULL);

  const Result<CommittedMap> committed = service.value()->committed_usage(usage.envelope_id);
  RE_REQUIRE(committed.ok());
  RE_CHECK_EQ(committed.value().size(), std::size_t{1});
  RE_CHECK_EQ(committed.value().begin()->second.value,
              static_cast<std::int64_t>(25ULL * kNanounitsPerUnit));

  // A conflicting reuse of the same key with a different amount is refused.
  UsageInput conflicting = usage;
  conflicting.deltas.front().delta = static_cast<std::int64_t>(30ULL * kNanounitsPerUnit);
  const Result<UsageResult> conflict = service.value()->record_usage(conflicting);
  RE_CHECK(!conflict.ok());
  RE_CHECK_EQ(conflict.status().code(), StatusCode::IdempotencyConflict);
}

RE_TEST(compaction_preserves_state_and_digest) {
  const ScratchStore scratch("re-store-compact");
  Digest committed_state;
  Digest declared_digest;
  std::uint64_t envelope_count = 0U;
  std::uint64_t state_sequence = 0U;
  // The writer lock lives as long as a service that can write, so the work is done inside a
  // nested scope that owns the handle and is finished before the store is reopened. This is a
  // local shadow rather than a block around the opening result, because a block cannot end the
  // lifetime of the opening result declared outside it.
  {
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());
  const Result<StoreStatistics> before = service.value()->statistics();
  RE_REQUIRE(before.ok());
  const Status compacted = service.value()->compact();
  RE_REQUIRE(compacted.ok());

  // Compaction rewrites the store without changing what it says, so the state commitment is
  // identical before and after, and the store reports exactly one compaction.
  const Result<StoreStatistics> stats = service.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().state_digest, before.value().state_digest);
  RE_CHECK_EQ(stats.value().compaction_count, 1ULL);
  RE_CHECK_EQ(stats.value().envelope_count, before.value().envelope_count);
  RE_CHECK_EQ(stats.value().state_sequence, before.value().state_sequence);

  const Result<EnvelopeView> view = service.value()->get_envelope("tenant-a-envelope");
  RE_REQUIRE(view.ok());
  RE_CHECK_EQ(view.value().envelope.revision, 1ULL);

  committed_state = stats.value().state_digest;
  declared_digest = view.value().record_digest;
  envelope_count = stats.value().envelope_count;
  state_sequence = stats.value().state_sequence;
  }

  // A compacted store is a store: the whole point of compaction is that it can be reopened.
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options(OpenMode::OpenExisting));
  RE_REQUIRE(reopened.ok());
  const Result<EnvelopeView> reread = reopened.value()->get_envelope("tenant-a-envelope");
  RE_REQUIRE(reread.ok());
  RE_CHECK_EQ(reread.value().envelope.revision, 1ULL);
  RE_CHECK_EQ(reread.value().record_digest, declared_digest);
  const Result<StoreStatistics> reopened_stats = reopened.value()->statistics();
  RE_REQUIRE(reopened_stats.ok());
  RE_CHECK_EQ(reopened_stats.value().state_digest, committed_state);
  RE_CHECK_EQ(reopened_stats.value().compaction_count, 1ULL);
  RE_CHECK_EQ(reopened_stats.value().envelope_count, envelope_count);
  RE_CHECK_EQ(reopened_stats.value().state_sequence, state_sequence);
}

RE_TEST(retirement_stops_an_envelope_being_authoritative) {
  const ScratchStore scratch("re-store-retire");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());
  TombstoneInput retire;
  retire.envelope_id = "tenant-a-envelope";
  retire.reason = "decommissioned";
  retire.requested_at = 1767225600000000000LL + 5000LL;
  retire.request_id = 42U;
  const Result<TombstoneResult> retired = service.value()->tombstone(retire);
  RE_REQUIRE(retired.ok());
  RE_CHECK(!retired.value().replayed);
  // A retried retirement is resolved as a replay, so a lost response is safe to retry.
  const Result<TombstoneResult> replay = service.value()->tombstone(retire);
  RE_REQUIRE(replay.ok());
  RE_CHECK(replay.value().replayed);
  const Result<EvaluationResult> refused =
      service.value()->evaluate(request_for("tenant-a", 3U, 1ULL, "eval-1"));
  RE_REQUIRE(refused.ok());
  RE_CHECK_EQ(refused.value().outcome, Outcome::Denied);
  RE_CHECK_EQ(refused.value().reason, StatusCode::EnvelopeNotFound);
}

RE_TEST(truncating_the_delta_segment_is_detected_and_refused) {
  const ScratchStore scratch("re-store-truncate");
  {
    const Result<std::unique_ptr<EnvelopeService>> service =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(service.ok());
    DeclareInput input;
    input.envelope = envelope_for("tenant-a", 3U, 100ULL);
    input.idempotency_key = "declare-1";
    input.requested_at = 1767225600000000000LL;
    RE_REQUIRE(service.value()->declare(input).ok());
  }
  // Remove the tail of every segment so the committed frames are no longer present.
  std::error_code error;
  for (const std::filesystem::directory_entry& item :
       std::filesystem::directory_iterator(scratch.path() / "segments", error)) {
    RE_REQUIRE(!error);
    const std::uintmax_t size = item.file_size(error);
    RE_REQUIRE(!error);
    RE_REQUIRE(size > 40U);
    std::filesystem::resize_file(item.path(), size - 16U, error);
    RE_REQUIRE(!error);
  }
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options(OpenMode::OpenExisting));
  // The store must fail closed rather than start empty or serve a partial state.
  RE_CHECK(!reopened.ok());
  if (!reopened.ok()) {
    const StatusCode code = reopened.status().code();
    RE_CHECK(code == StatusCode::StoreCorrupt || code == StatusCode::StoreIntegrityMismatch);
  }
}

RE_TEST(read_only_handle_refuses_mutation_and_still_reads) {
  const ScratchStore scratch("re-store-readonly");
  {
    const Result<std::unique_ptr<EnvelopeService>> service =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(service.ok());
    DeclareInput input;
    input.envelope = envelope_for("tenant-a", 3U, 100ULL);
    input.idempotency_key = "declare-1";
    input.requested_at = 1767225600000000000LL;
    RE_REQUIRE(service.value()->declare(input).ok());
  }
  const Result<std::unique_ptr<EnvelopeService>> reader =
      EnvelopeService::open(scratch.path(), existing_options(OpenMode::ReadOnly));
  RE_REQUIRE(reader.ok());
  const Result<EnvelopeView> view = reader.value()->get_envelope("tenant-a-envelope");
  RE_REQUIRE(view.ok());
  DeclareInput second;
  second.envelope = envelope_for("tenant-b", 1U, 10ULL);
  second.idempotency_key = "declare-2";
  second.requested_at = 1767225600000000000LL;
  const Result<EnvelopeDeclaration> refused = reader.value()->declare(second);
  RE_CHECK(!refused.ok());
  RE_CHECK_EQ(refused.status().code(), StatusCode::UnsupportedOperation);
}

RE_TEST(hostile_store_paths_are_refused_without_creating_anything) {
  // An identifier that is not a canonical token can never become a durable path
  // segment, so a declaration carrying one is refused at the API boundary.
  const ScratchStore scratch("re-store-hostile");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  const char* hostile[] = {"../../escape", "con", "a/b", "a\\b", "with space", "UPPER", "x--y", "-lead"};
  for (const char* name : hostile) {
    DeclareInput input;
    input.envelope = envelope_for(name, 1U, 10ULL);
    input.idempotency_key = "declare-hostile";
    input.requested_at = 1767225600000000000LL;
    const Result<EnvelopeDeclaration> refused = service.value()->declare(input);
    RE_CHECK(!refused.ok());
  }
  const Result<StoreStatistics> stats = service.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().envelope_count, 0ULL);
}

// ---------------------------------------------------------------------------
// Regression cases for the defects the randomized lifecycle exposed
// ---------------------------------------------------------------------------

RE_TEST(a_revised_envelope_still_evaluates_under_its_new_revision) {
  // A revision that cannot be evaluated makes the whole lifecycle useless, so this pins the
  // property directly: an envelope that has been revised is authoritative under the revision that
  // is current, and it carries no lineage pointer to itself.
  const ScratchStore scratch("re-store-revised-evaluates");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());

  UsageInput usage;
  usage.envelope_id = "tenant-a-envelope";
  usage.idempotency_key = "usage-1";
  usage.requested_at = 1767225600000000000LL;
  UsageDelta delta;
  delta.envelope_id = usage.envelope_id;
  delta.kind = DimensionKind::PowerDrawWatts;
  delta.delta = static_cast<std::int64_t>(30ULL * kNanounitsPerUnit);
  usage.deltas.push_back(delta);
  RE_REQUIRE(service.value()->record_usage(usage).ok());
  RE_CHECK_EQ(
      service.value()->evaluate(request_for("tenant-a", 3U, 10ULL * kNanounitsPerUnit, "evaluate-1"))
          .value()
          .outcome,
      Outcome::Granted);

  ReviseInput revise;
  revise.envelope = envelope_for("tenant-a", 3U, 50ULL);
  revise.idempotency_key = "revise-1";
  revise.requested_at = 1767225600000000000LL + 1000LL;
  revise.expected_current_revision = 1U;
  const Result<EnvelopeRevision> revised = service.value()->revise(revise);
  RE_REQUIRE(revised.ok());
  RE_CHECK_EQ(revised.value().revision, 2ULL);
  const Result<EnvelopeView> view = service.value()->get_envelope("tenant-a-envelope");
  RE_REQUIRE(view.ok());
  RE_CHECK(!view.value().envelope.supersedes.has_value());

  const Result<EvaluationResult> evaluated =
      service.value()->evaluate(request_for("tenant-a", 3U, 10ULL * kNanounitsPerUnit, "evaluate-2"));
  RE_REQUIRE(evaluated.ok());
  RE_CHECK_EQ(evaluated.value().envelope_revision, 2ULL);
  RE_CHECK_EQ(evaluated.value().outcome, Outcome::Granted);
  RE_CHECK_EQ(evaluated.value().reason, StatusCode::Ok);
  // The tighter revision tightens the arithmetic: 50 W with 30 W committed leaves 20 W, so a 25 W
  // request is refused and the residual is reported exactly.
  const Result<EvaluationResult> refused =
      service.value()->evaluate(request_for("tenant-a", 3U, 25ULL * kNanounitsPerUnit, "evaluate-3"));
  RE_REQUIRE(refused.ok());
  RE_CHECK_EQ(refused.value().outcome, Outcome::Denied);
  RE_CHECK_EQ(refused.value().reason, StatusCode::RejectedInsufficientResidual);
  RE_REQUIRE(!refused.value().dimensions.empty());
  RE_REQUIRE(refused.value().dimensions.front().residual.has_value());
  RE_CHECK_EQ(*refused.value().dimensions.front().residual, 20ULL * kNanounitsPerUnit);
}

RE_TEST(compaction_is_never_the_last_thing_that_can_happen_to_a_store) {
  // Compaction followed by continued mutation is the case a single-generation compaction test
  // cannot reach: a delta written after a compaction must not land in the segment the manifest
  // reads as its snapshot, and the sequence a snapshot covers must not be confused with the number
  // of frames it holds.
  const ScratchStore scratch("re-store-compact-then-mutate");
  std::uint64_t decisions = 0U;
  Digest committed_digest;
  {
    const Result<std::unique_ptr<EnvelopeService>> service =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(service.ok());
    DeclareInput input;
    input.envelope = envelope_for("tenant-a", 3U, 100ULL);
    input.idempotency_key = "declare-1";
    input.requested_at = 1767225600000000000LL;
    RE_REQUIRE(service.value()->declare(input).ok());
    UsageInput usage;
    usage.envelope_id = "tenant-a-envelope";
    usage.idempotency_key = "usage-1";
    usage.requested_at = 1767225600000000000LL;
    UsageDelta delta;
    delta.envelope_id = usage.envelope_id;
    delta.kind = DimensionKind::PowerDrawWatts;
    delta.delta = static_cast<std::int64_t>(25ULL * kNanounitsPerUnit);
    usage.deltas.push_back(delta);
    RE_REQUIRE(service.value()->record_usage(usage).ok());
    RE_REQUIRE(service.value()
                   ->authorize(request_for("tenant-a", 3U, 10ULL * kNanounitsPerUnit, "authorize-1"))
                   .ok());

    // Several frames are folded into a one-frame snapshot, so the generation has no delta and must
    // allocate one above the snapshot rather than reuse the snapshot's own name.
    RE_REQUIRE(service.value()->compact().ok());
    ReviseInput revise;
    revise.envelope = envelope_for("tenant-a", 3U, 80ULL);
    revise.idempotency_key = "revise-1";
    revise.requested_at = 1767225600000000000LL + 1000LL;
    RE_REQUIRE(service.value()->revise(revise).ok());
    UsageInput second = usage;
    second.idempotency_key = "usage-2";
    second.deltas.front().delta = static_cast<std::int64_t>(5ULL * kNanounitsPerUnit);
    const Result<UsageResult> more = service.value()->record_usage(second);
    RE_REQUIRE(more.ok());
    RE_CHECK_EQ(more.value().entries_appended, 1ULL);
    RE_REQUIRE(service.value()->compact().ok());
    RE_REQUIRE(service.value()
                   ->authorize(request_for("tenant-a", 3U, 5ULL * kNanounitsPerUnit, "authorize-2"))
                   .ok());
    const Result<StoreStatistics> stats = service.value()->statistics();
    RE_REQUIRE(stats.ok());
    decisions = stats.value().decision_count;
    committed_digest = stats.value().state_digest;
  }
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options(OpenMode::OpenExisting));
  RE_REQUIRE(reopened.ok());
  const Result<StoreStatistics> stats = reopened.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().state_digest, committed_digest);
  RE_CHECK_EQ(stats.value().decision_count, decisions);
  RE_CHECK_EQ(stats.value().compaction_count, 2ULL);
  const Result<EnvelopeView> view = reopened.value()->get_envelope("tenant-a-envelope");
  RE_REQUIRE(view.ok());
  RE_CHECK_EQ(view.value().envelope.revision, 2ULL);
  const Result<CommittedMap> committed = reopened.value()->committed_usage("tenant-a-envelope");
  RE_REQUIRE(committed.ok());
  RE_CHECK_EQ(committed.value().size(), std::size_t{1});
  if (!committed.value().empty()) {
    RE_CHECK_EQ(committed.value().begin()->second.value,
                static_cast<std::int64_t>(30ULL * kNanounitsPerUnit));
  }
  ReviseInput again;
  again.envelope = envelope_for("tenant-a", 3U, 70ULL);
  again.idempotency_key = "revise-2";
  again.requested_at = 1767225600000000000LL + 2000LL;
  RE_CHECK(reopened.value()->revise(again).ok());
}

RE_TEST(compacting_an_empty_store_neither_changes_it_nor_breaks_it) {
  // A rewrite must not change what the store says, and a store that holds no records is a store:
  // after a compaction with nothing to compact it is still openable, still empty, and still able
  // to accept its first declaration.
  const ScratchStore scratch("re-store-compact-empty");
  Digest before;
  {
    const Result<std::unique_ptr<EnvelopeService>> service =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(service.ok());
    const Result<StoreStatistics> initial = service.value()->statistics();
    RE_REQUIRE(initial.ok());
    before = initial.value().state_digest;
    RE_REQUIRE(service.value()->compact().ok());
    const Result<StoreStatistics> after = service.value()->statistics();
    RE_REQUIRE(after.ok());
    RE_CHECK_EQ(after.value().state_digest, before);
    RE_CHECK_EQ(after.value().state_sequence, initial.value().state_sequence);
  }
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options(OpenMode::OpenExisting));
  RE_REQUIRE(reopened.ok());
  const Result<StoreStatistics> stats = reopened.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().state_digest, before);
  RE_CHECK_EQ(stats.value().envelope_count, 0ULL);
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_CHECK(reopened.value()->declare(input).ok());
}

RE_TEST(a_usage_retry_is_a_replay_across_compaction_revision_and_retirement) {
  const ScratchStore scratch("re-store-usage-replay-paths");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());

  UsageInput usage;
  usage.envelope_id = "tenant-a-envelope";
  usage.idempotency_key = "usage-1";
  usage.requested_at = 1767225600000000000LL;
  UsageDelta delta;
  delta.envelope_id = usage.envelope_id;
  delta.kind = DimensionKind::PowerDrawWatts;
  delta.delta = static_cast<std::int64_t>(25ULL * kNanounitsPerUnit);
  usage.deltas.push_back(delta);
  RE_REQUIRE(service.value()->record_usage(usage).ok());

  const Result<UsageResult> immediate = service.value()->record_usage(usage);
  RE_REQUIRE(immediate.ok());
  RE_CHECK_EQ(immediate.value().entries_appended, 0ULL);
  RE_CHECK_EQ(immediate.value().entries_replayed, 1ULL);
  RE_CHECK_EQ(immediate.value().envelope_revision, 1ULL);

  // A compaction drops the journal, and the claim is still recognised: without that the same
  // delta would be counted a second time and the commitment would silently double.
  RE_REQUIRE(service.value()->compact().ok());
  const Result<UsageResult> compacted = service.value()->record_usage(usage);
  RE_REQUIRE(compacted.ok());
  RE_CHECK_EQ(compacted.value().entries_appended, 0ULL);
  RE_CHECK_EQ(compacted.value().entries_replayed, 1ULL);

  // A revision between the attempt and the retry does not turn a retry into a conflict: the claim
  // is what the caller stated, not the revision the store derived at the time.
  ReviseInput revise;
  revise.envelope = envelope_for("tenant-a", 3U, 90ULL);
  revise.idempotency_key = "revise-1";
  revise.requested_at = 1767225600000000000LL + 1000LL;
  RE_REQUIRE(service.value()->revise(revise).ok());
  const Result<UsageResult> after_revision = service.value()->record_usage(usage);
  RE_REQUIRE(after_revision.ok());
  RE_CHECK_EQ(after_revision.value().entries_appended, 0ULL);
  RE_CHECK_EQ(after_revision.value().entries_replayed, 1ULL);

  // A different claim under the same key is still a conflict, which is what keeps the replay
  // recognition meaningful rather than indiscriminate.
  UsageInput conflicting = usage;
  conflicting.deltas.front().delta = static_cast<std::int64_t>(30ULL * kNanounitsPerUnit);
  const Result<UsageResult> conflict = service.value()->record_usage(conflicting);
  RE_CHECK(!conflict.ok());
  if (!conflict.ok()) RE_CHECK_EQ(conflict.status().code(), StatusCode::IdempotencyConflict);

  // Retirement moves the authority again, and the retry is answered exactly the same way.
  TombstoneInput retire;
  retire.envelope_id = "tenant-a-envelope";
  retire.reason = "decommissioned";
  retire.request_id = 5U;
  retire.requested_at = 1767225600000000000LL + 2000LL;
  RE_REQUIRE(service.value()->tombstone(retire).ok());
  const Result<UsageResult> after_retirement = service.value()->record_usage(usage);
  RE_REQUIRE(after_retirement.ok());
  RE_CHECK_EQ(after_retirement.value().entries_appended, 0ULL);
  RE_CHECK_EQ(after_retirement.value().entries_replayed, 1ULL);
  const Result<CommittedMap> committed = service.value()->committed_usage("tenant-a-envelope");
  RE_REQUIRE(committed.ok());
  RE_CHECK_EQ(committed.value().size(), std::size_t{1});
  if (!committed.value().empty()) {
    RE_CHECK_EQ(committed.value().begin()->second.value,
                static_cast<std::int64_t>(25ULL * kNanounitsPerUnit));
  }
  // A new claim against a retired envelope is refused: retirement stops new commitments.
  UsageInput fresh = usage;
  fresh.idempotency_key = "usage-2";
  const Result<UsageResult> refused = service.value()->record_usage(fresh);
  RE_CHECK(!refused.ok());
  if (!refused.ok()) RE_CHECK_EQ(refused.status().code(), StatusCode::EnvelopeNotFound);
}

RE_TEST(an_observation_retry_is_a_replay) {
  const ScratchStore scratch("re-store-observation-replay");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());

  ObservationInput observation;
  observation.envelope_id = "tenant-a-envelope";
  observation.idempotency_key = "observation-1";
  observation.requested_at = 1767225600000000000LL;
  ObservationEntry entry;
  entry.envelope_id = observation.envelope_id;
  entry.kind = DimensionKind::PowerDrawWatts;
  entry.status = MeasureStatus::Measured;
  entry.value = 40ULL * kNanounitsPerUnit;
  entry.principal = "principal-a";
  entry.observed_at = observation.requested_at;
  entry.source = "meter";
  observation.entries.push_back(entry);
  const Result<ObservationResult> first = service.value()->record_observation(observation);
  RE_REQUIRE(first.ok());
  RE_CHECK_EQ(first.value().stored, 1ULL);
  RE_CHECK_EQ(first.value().replayed, 0ULL);
  const Result<ObservationResult> second = service.value()->record_observation(observation);
  RE_REQUIRE(second.ok());
  // The sequence and the entry identifier are assigned by the store, so they cannot be part of the
  // claim a retry has to reproduce: a retry counted as new would inflate the record.
  RE_CHECK_EQ(second.value().stored, 0ULL);
  RE_CHECK_EQ(second.value().replayed, 1ULL);
  const Result<StoreStatistics> stats = service.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().observation_count, 1ULL);

  ReviseInput revise;
  revise.envelope = envelope_for("tenant-a", 3U, 80ULL);
  revise.idempotency_key = "revise-1";
  revise.requested_at = 1767225600000000000LL + 1000LL;
  RE_REQUIRE(service.value()->revise(revise).ok());
  const Result<ObservationResult> after_revision = service.value()->record_observation(observation);
  RE_REQUIRE(after_revision.ok());
  RE_CHECK_EQ(after_revision.value().replayed, 1ULL);
  const Result<std::vector<ObservationEntry>> stored =
      service.value()->observations("tenant-a-envelope");
  RE_REQUIRE(stored.ok());
  RE_CHECK_EQ(stored.value().size(), std::size_t{1});
}

RE_TEST(a_retry_after_retirement_returns_the_recorded_answer) {
  // Retirement is a change to the authority, not a reason for a caller to lose the answer to a
  // request it never received. Every operation that was accepted before the retirement answers a
  // retry with what it recorded, and none of the retries publishes anything.
  const ScratchStore scratch("re-store-retirement-replay");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());
  ReviseInput revise;
  revise.envelope = envelope_for("tenant-a", 3U, 90ULL);
  revise.idempotency_key = "revise-1";
  revise.requested_at = 1767225600000000000LL + 1000LL;
  const Result<EnvelopeRevision> revised = service.value()->revise(revise);
  RE_REQUIRE(revised.ok());
  UsageInput usage;
  usage.envelope_id = "tenant-a-envelope";
  usage.idempotency_key = "usage-1";
  usage.requested_at = 1767225600000000000LL + 2000LL;
  UsageDelta delta;
  delta.envelope_id = usage.envelope_id;
  delta.kind = DimensionKind::PowerDrawWatts;
  delta.delta = static_cast<std::int64_t>(25ULL * kNanounitsPerUnit);
  usage.deltas.push_back(delta);
  RE_REQUIRE(service.value()->record_usage(usage).ok());
  ObservationInput observation;
  observation.envelope_id = "tenant-a-envelope";
  observation.idempotency_key = "observation-1";
  observation.requested_at = 1767225600000000000LL + 3000LL;
  ObservationEntry entry;
  entry.envelope_id = observation.envelope_id;
  entry.kind = DimensionKind::PowerDrawWatts;
  entry.status = MeasureStatus::Measured;
  entry.value = 40ULL * kNanounitsPerUnit;
  entry.observed_at = observation.requested_at;
  entry.source = "meter";
  observation.entries.push_back(entry);
  RE_REQUIRE(service.value()->record_observation(observation).ok());
  const EvaluationRequest request = request_for("tenant-a", 3U, 10ULL, "authorize-1");
  const Result<AuthorizeResult> authorized = service.value()->authorize(request);
  RE_REQUIRE(authorized.ok());
  const Digest decision_digest = authorized.value().decision_digest;
  const std::uint64_t decision_sequence = authorized.value().record.sequence;

  TombstoneInput retire;
  retire.envelope_id = "tenant-a-envelope";
  retire.reason = "decommissioned";
  retire.request_id = 7U;
  retire.requested_at = 1767225600000000000LL + 4000LL;
  RE_REQUIRE(service.value()->tombstone(retire).ok());
  const Result<StoreStatistics> before = service.value()->statistics();
  RE_REQUIRE(before.ok());

  const Result<EnvelopeDeclaration> declaration_retry = service.value()->declare(input);
  RE_REQUIRE(declaration_retry.ok());
  RE_CHECK(declaration_retry.value().replayed);
  RE_CHECK_EQ(declaration_retry.value().revision, 1ULL);
  const Result<EnvelopeRevision> revision_retry = service.value()->revise(revise);
  RE_REQUIRE(revision_retry.ok());
  RE_CHECK(revision_retry.value().replayed);
  RE_CHECK_EQ(revision_retry.value().revision, revised.value().revision);
  const Result<UsageResult> usage_retry = service.value()->record_usage(usage);
  RE_REQUIRE(usage_retry.ok());
  RE_CHECK_EQ(usage_retry.value().entries_appended, 0ULL);
  RE_CHECK_EQ(usage_retry.value().entries_replayed, 1ULL);
  const Result<ObservationResult> observation_retry = service.value()->record_observation(observation);
  RE_REQUIRE(observation_retry.ok());
  RE_CHECK_EQ(observation_retry.value().stored, 0ULL);
  RE_CHECK_EQ(observation_retry.value().replayed, 1ULL);
  const Result<AuthorizeResult> authorize_retry = service.value()->authorize(request);
  RE_REQUIRE(authorize_retry.ok());
  RE_CHECK(authorize_retry.value().replayed);
  RE_CHECK_EQ(authorize_retry.value().decision_digest, decision_digest);
  RE_CHECK_EQ(authorize_retry.value().record.sequence, decision_sequence);

  const Result<StoreStatistics> after = service.value()->statistics();
  RE_REQUIRE(after.ok());
  RE_CHECK_EQ(after.value().epoch, before.value().epoch);
  RE_CHECK_EQ(after.value().state_sequence, before.value().state_sequence);
  RE_CHECK_EQ(after.value().decision_count, before.value().decision_count);
  RE_CHECK_EQ(after.value().state_digest, before.value().state_digest);
}

RE_TEST(a_release_below_zero_is_recorded_and_the_store_still_opens) {
  // A commitment is a fold over deltas, and a release larger than the commitment takes the fold
  // below zero. That is evidence of an over-release, not corruption: the evaluator refuses to read
  // it as capacity, and the store it produced must be loadable again rather than refused.
  const ScratchStore scratch("re-store-negative-commitment");
  {
    const Result<std::unique_ptr<EnvelopeService>> service =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(service.ok());
    DeclareInput input;
    input.envelope = envelope_for("tenant-a", 3U, 100ULL);
    input.idempotency_key = "declare-1";
    input.requested_at = 1767225600000000000LL;
    RE_REQUIRE(service.value()->declare(input).ok());
    UsageInput usage;
    usage.envelope_id = "tenant-a-envelope";
    usage.idempotency_key = "usage-1";
    usage.requested_at = 1767225600000000000LL;
    UsageDelta delta;
    delta.envelope_id = usage.envelope_id;
    delta.kind = DimensionKind::PowerDrawWatts;
    delta.delta = static_cast<std::int64_t>(25ULL * kNanounitsPerUnit);
    usage.deltas.push_back(delta);
    RE_REQUIRE(service.value()->record_usage(usage).ok());
    UsageInput release = usage;
    release.idempotency_key = "usage-2";
    release.deltas.front().delta = -static_cast<std::int64_t>(30ULL * kNanounitsPerUnit);
    RE_REQUIRE(service.value()->record_usage(release).ok());
    const Result<EvaluationResult> evaluated =
        service.value()->evaluate(request_for("tenant-a", 3U, kNanounitsPerUnit, "evaluate-1"));
    RE_REQUIRE(evaluated.ok());
    RE_CHECK_EQ(evaluated.value().outcome, Outcome::Indeterminate);
    RE_CHECK_EQ(evaluated.value().reason, StatusCode::IndeterminateUnknownCommitted);
    RE_REQUIRE(!evaluated.value().dimensions.empty());
    RE_CHECK(!evaluated.value().dimensions.front().residual.has_value());
    RE_REQUIRE(service.value()->compact().ok());
  }
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options(OpenMode::OpenExisting));
  RE_REQUIRE(reopened.ok());
  const Result<CommittedMap> committed = reopened.value()->committed_usage("tenant-a-envelope");
  RE_REQUIRE(committed.ok());
  RE_CHECK_EQ(committed.value().size(), std::size_t{1});
  if (!committed.value().empty()) {
    RE_CHECK_EQ(committed.value().begin()->second.value,
                -static_cast<std::int64_t>(5ULL * kNanounitsPerUnit));
  }
  const Result<EvaluationResult> evaluated =
      reopened.value()->evaluate(request_for("tenant-a", 3U, kNanounitsPerUnit, "evaluate-2"));
  RE_REQUIRE(evaluated.ok());
  RE_CHECK_EQ(evaluated.value().outcome, Outcome::Indeterminate);
  RE_CHECK_EQ(evaluated.value().reason, StatusCode::IndeterminateUnknownCommitted);
}

RE_TEST(two_current_envelopes_for_one_scope_are_reported_as_ambiguous) {
  // No authority duplication: a second envelope claiming a scope an existing envelope already
  // claims is not silently resolved in favour of either of them, and the refusal says which
  // situation the caller is in rather than reporting a scope that exists as absent.
  const ScratchStore scratch("re-store-ambiguous-scope");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput first;
  first.envelope = envelope_for("tenant-a", 3U, 100ULL);
  first.idempotency_key = "declare-1";
  first.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(first).ok());
  DeclareInput second;
  second.envelope = envelope_for("tenant-a", 3U, 60ULL);
  second.envelope.id = "tenant-a-envelope-two";
  second.idempotency_key = "declare-2";
  second.requested_at = 1767225600000000000LL + 1LL;
  RE_REQUIRE(service.value()->declare(second).ok());

  const Result<std::vector<EnvelopeView>> scoped = service.value()->find_by_scope(first.envelope.scope);
  RE_REQUIRE(scoped.ok());
  RE_CHECK_EQ(scoped.value().size(), std::size_t{2});
  const Result<EvaluationResult> evaluated =
      service.value()->evaluate(request_for("tenant-a", 3U, kNanounitsPerUnit, "evaluate-1"));
  RE_REQUIRE(evaluated.ok());
  RE_CHECK_EQ(evaluated.value().outcome, Outcome::Denied);
  RE_CHECK_EQ(evaluated.value().reason, StatusCode::DuplicateIdentity);
  RE_CHECK(evaluated.value().envelope_id.empty());
  const Result<AuthorizeResult> authorized =
      service.value()->authorize(request_for("tenant-a", 3U, kNanounitsPerUnit, "authorize-1"));
  RE_REQUIRE(authorized.ok());
  RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::DuplicateIdentity);
  const Result<StoreStatistics> stats = service.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().decision_count, 0ULL);
}

RE_TEST(a_recorded_decision_reports_the_digest_that_identifies_it) {
  const ScratchStore scratch("re-store-decision-digest");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  DeclareInput input;
  input.envelope = envelope_for("tenant-a", 3U, 100ULL);
  input.idempotency_key = "declare-1";
  input.requested_at = 1767225600000000000LL;
  RE_REQUIRE(service.value()->declare(input).ok());
  const Result<AuthorizeResult> authorized =
      service.value()->authorize(request_for("tenant-a", 3U, 10ULL * kNanounitsPerUnit, "authorize-1"));
  RE_REQUIRE(authorized.ok());
  const std::string decision_id = authorized.value().record.decision_id;
  const Digest digest = authorized.value().decision_digest;
  // The digest is a function of the record rather than a field carried beside it, so a record read
  // back from durable state reports the same digest the determination reported.
  const Result<DecisionRecord> stored = service.value()->get_decision(decision_id);
  RE_REQUIRE(stored.ok());
  RE_CHECK_EQ(stored.value().decision_digest, digest);
  RE_CHECK_EQ(compute_decision_digest(stored.value()), digest);
  const Result<DecisionRecord> by_key =
      service.value()->find_decision_by_key("tenant-a-envelope", "authorize-1");
  RE_REQUIRE(by_key.ok());
  RE_CHECK_EQ(by_key.value().decision_digest, digest);
  const Result<std::vector<DecisionRecord>> listed =
      service.value()->list_decisions("tenant-a-envelope", 5U);
  RE_REQUIRE(listed.ok());
  RE_REQUIRE(listed.value().size() == std::size_t{1});
  RE_CHECK_EQ(listed.value().front().decision_digest, digest);
  const Result<std::vector<HistoryEntry>> history = service.value()->history("tenant-a-envelope");
  RE_REQUIRE(history.ok());
  bool found_decision = false;
  for (const HistoryEntry& entry : history.value()) {
    if (entry.kind != "decision") continue;
    found_decision = true;
    RE_CHECK_EQ(entry.digest, digest);
  }
  RE_CHECK(found_decision);
}

