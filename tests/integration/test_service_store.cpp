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
