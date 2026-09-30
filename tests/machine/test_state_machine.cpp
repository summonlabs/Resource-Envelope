// Seeded randomized state-machine validation of the public mutation and evaluation lifecycle.
//
// The property suite validates the pure functions against generated inputs. This suite does the
// complementary thing: it drives the real service - declarations, revisions, retirements, usage,
// observations, authorisations, verifications, compactions and restarts against a live store -
// through a long randomized sequence, and checks the core invariants after *every* step against a
// reference model held by the test.
//
//  1. envelope lifecycle and revision monotonicity
//  2. idempotent replay before staleness
//  3. generation and control-epoch fencing
//  4. exact residual arithmetic
//  5. unknown never becomes zero or permitted
//  6. retire, revise and reopen semantics
//  7. durable restart equivalence
//  8. compaction followed by continued mutation
//  9. canonical digest stability and declaration-order independence
// 10. no authority duplication
//
// The run is a pure function of its seed: every random choice comes from one generator, so a
// failure is reproducible exactly by rerunning the suite with the seed and step count it prints:
//
//   re_test_machine_test_state_machine --seed <seed> --steps <steps>

#include "support/testing.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/digest.hpp"
#include "resource_envelope/service.hpp"

using namespace resource_envelope;

namespace {

constexpr std::uint64_t kPrimarySeed = 0x5EED5EED1234ABCDULL;
constexpr std::uint64_t kSecondarySeed = 0x0F1E2D3C4B5A6978ULL;
constexpr int kPrimarySteps = 200;
constexpr int kSecondarySteps = 120;
constexpr int kReproducibilitySteps = 80;

// 2026-01-01T00:00:00Z in nanoseconds. Every timestamp in this suite is derived from it, so
// nothing depends on the wall clock and the whole run is reproducible from the seed alone.
constexpr Timestamp kBase = 1767225600000000000LL;
constexpr Nanounits kWatt = kNanounitsPerUnit;
constexpr Nanounits kSpaceLimit = 40ULL * kNanounitsPerUnit;

const char* const kEnvelopePool[] = {"machine-envelope-a", "machine-envelope-b",
                                     "machine-envelope-c"};

Digest digest_of(std::string_view text) { return Digest(sha256_domain("machine-identity-record", text)); }

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

// A scratch store directory inside the current test working directory, removed on scope exit even
// when a check fails.
class ScratchStore {
 public:
  explicit ScratchStore(const std::string& name) {
    path_ = std::filesystem::current_path() / name;
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

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------
// One declared envelope as the test declared it. The declared envelope is kept so a retry can
// reproduce the identical claim, which is what makes a replay a replay.
struct EnvelopeModel {
  std::string id;
  EnvelopeScope scope;
  Envelope declared;
  Nanounits limit = 0;
  Nanounits reserved = 0;
  Nanounits quantum = 0;
  Digest identity_digest;
  bool require_binding = true;

  std::uint64_t revision = 0;  // current revision; zero once retired
  Digest record_digest;
  bool retired = false;
  std::uint64_t last_revision = 0;
  Digest last_record_digest;
  std::uint64_t declared_revision = 0;  // the revision the declaration created
  Digest declared_record_digest;
  std::uint64_t usage_revision = 0;  // revision the last usage claim was recorded against
  std::string last_decision_id;
  std::uint64_t last_decision_sequence = 0;
  Outcome last_decision_outcome = Outcome::Indeterminate;

  std::optional<DeclareInput> last_declare;
  std::optional<ReviseInput> last_revise;
  std::optional<UsageInput> last_usage;
  std::optional<ObservationInput> last_observation;
  std::optional<TombstoneInput> last_retirement;
  std::optional<EvaluationRequest> last_authorize;
};

struct DecisionModel {
  std::string id;
  std::uint64_t sequence = 0;
  std::uint64_t control_epoch = 0;
  Outcome outcome = Outcome::Indeterminate;
  StatusCode reason = StatusCode::Ok;
};

struct Model {
  std::map<std::string, EnvelopeModel> envelopes;
  std::map<CommittedKey, std::int64_t> committed;
  std::vector<DecisionModel> decisions;
  std::map<std::string, std::uint64_t> observed_revision;
  std::uint64_t observations = 0;
  std::uint64_t usage_entries = 0;
  std::uint64_t epoch = 0;
  std::uint64_t state_sequence = 0;
  std::uint64_t compactions = 0;
  Digest state_digest;
  bool duplicate_scope_declared = false;
};

struct Machine {
  std::filesystem::path path;
  std::unique_ptr<EnvelopeService> service;
  Model model;
  std::mt19937_64 engine;
  std::uint64_t seed = 0;
  int steps = 0;
  int step = 0;
  std::vector<std::string> log;

  void note(const std::string& text) {
    log.push_back("#" + std::to_string(step) + " " + text);
    if (log.size() > 80U) log.erase(log.begin(), log.begin() + 40);
  }
};

void dump_failure(const Machine& machine, const char* what) {
  std::fprintf(stdout, "MACHINE %s at step %d of %d (seed %llu)\n", what, machine.step, machine.steps,
               static_cast<unsigned long long>(machine.seed));
  std::fprintf(stdout, "REPRODUCE: re_test_machine_test_state_machine --seed %llu --steps %d\n",
               static_cast<unsigned long long>(machine.seed), machine.steps);
  for (const std::string& line : machine.log) {
    std::fprintf(stdout, "  %s\n", line.c_str());
  }
  std::fflush(stdout);
}

std::uint64_t pick(std::mt19937_64& engine, std::uint64_t bound) {
  return bound == 0U ? 0U : std::uniform_int_distribution<std::uint64_t>(0U, bound - 1U)(engine);
}

// The envelope as the model holds it now: the declared constraints with the binding the
// envelope currently carries, so a revision that does not touch the binding preserves it rather
// than reverting it to whatever the first declaration stated.
Envelope build_envelope(const EnvelopeModel& spec, Timestamp at) {
  Envelope envelope = spec.declared;
  envelope.provenance.declared_at = at;
  envelope.identity_digest = spec.identity_digest;
  return envelope;
}

EvaluationRequest build_request(const EnvelopeModel& spec, Nanounits quantity, DimensionKind kind,
                                std::uint32_t principals, const std::string& key, Timestamp at,
                                bool with_identity, bool matching_digest) {
  EvaluationRequest request;
  request.scope = spec.scope;
  request.idempotency_key = key;
  request.at = at;
  if (with_identity) {
    request.identity.identity = spec.scope.identity;
    request.identity.digest = matching_digest ? spec.identity_digest : digest_of("stale-identity");
  }
  DimensionRequest dimension;
  dimension.kind = kind;
  dimension.quantity = quantity;
  dimension.principals = principals;
  request.dimensions.push_back(dimension);
  return request;
}

// True when the request confirms the envelope binding. An envelope that declares no identity
// digest has nothing to confirm; one that requires confirmation must be given a matching
// snapshot; one that waives confirmation may be given nothing.
bool binding_confirmed(const EnvelopeModel& spec, bool with_identity, bool matching_digest) {
  if (!spec.identity_digest.known()) return true;
  if (with_identity && matching_digest) return true;
  if (!with_identity && !spec.require_binding) return true;
  return false;
}

bool has_committed_row(const Model& model, const std::string& envelope_id, DimensionKind kind) {
  for (const auto& pair : model.committed) {
    if (pair.first.envelope_id == envelope_id && pair.first.kind == kind) return true;
  }
  return false;
}

std::int64_t committed_total(const Model& model, const std::string& envelope_id, DimensionKind kind) {
  std::int64_t total = 0;
  for (const auto& pair : model.committed) {
    if (pair.first.envelope_id == envelope_id && pair.first.kind == kind) total += pair.second;
  }
  return total;
}

bool any_negative_row(const Model& model, const std::string& envelope_id, DimensionKind kind) {
  for (const auto& pair : model.committed) {
    if (pair.first.envelope_id == envelope_id && pair.first.kind == kind && pair.second < 0) return true;
  }
  return false;
}

std::vector<std::string> envelope_ids(const Model& model) {
  std::vector<std::string> ids;
  for (const auto& pair : model.envelopes) ids.push_back(pair.first);
  return ids;
}

std::vector<std::string> live_envelopes(const Model& model) {
  std::vector<std::string> ids;
  for (const auto& pair : model.envelopes) {
    if (!pair.second.retired) ids.push_back(pair.first);
  }
  return ids;
}

// The identifiers whose model entry satisfies a predicate. A replay step needs an envelope that
// already carries a claim of that kind, and it deliberately does not require the envelope to be
// current: a retry of a claim that was accepted before the envelope was revised or retired is
// exactly the case the invariant "replay resolution precedes staleness" is about.
template <typename Predicate>
std::vector<std::string> select_ids(const Model& model, Predicate predicate) {
  std::vector<std::string> ids;
  for (const auto& pair : model.envelopes) {
    if (predicate(pair.second)) ids.push_back(pair.first);
  }
  return ids;
}

std::size_t claimants_for_scope(const Model& model, const EnvelopeScope& scope) {
  std::size_t count = 0;
  for (const auto& pair : model.envelopes) {
    if (!pair.second.retired && pair.second.scope == scope) ++count;
  }
  return count;
}

bool scope_is_ambiguous(const Model& model, const EnvelopeScope& scope) {
  return claimants_for_scope(model, scope) > 1U;
}

EnvelopeModel make_spec(const std::string& id, std::uint64_t generation, Nanounits limit,
                        Nanounits reserved, Digest identity_digest, bool require_binding) {
  EnvelopeModel spec;
  spec.id = id;
  spec.scope.kind = EnvelopeScopeKind::Tenant;
  spec.scope.identity.id = id + "-tenant";
  spec.scope.identity.generation = generation;
  spec.limit = limit;
  spec.reserved = reserved;
  spec.quantum = 5ULL * kWatt;
  spec.identity_digest = identity_digest;
  spec.require_binding = require_binding;
  Envelope envelope;
  envelope.id = id;
  envelope.kind = EnvelopeKind::Declared;
  envelope.scope = spec.scope;
  envelope.site_id = "site-one";
  envelope.identity_digest = identity_digest;
  envelope.require_binding_confirmation = require_binding;
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "state-machine";
  envelope.provenance.reason = "randomized lifecycle case";
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = limit;
  power.reserved = reserved;
  power.quantum = spec.quantum;
  envelope.dimensions.push_back(power);
  DimensionSpec space;
  space.kind = DimensionKind::SpaceRackUnits;
  space.hard_limit = kSpaceLimit;
  envelope.dimensions.push_back(space);
  spec.declared = envelope;
  return spec;
}

// ---------------------------------------------------------------------------
// Invariants checked after every step
// ---------------------------------------------------------------------------
void check_counters(Machine& machine) {
  const Result<StoreStatistics> stats = machine.service->statistics();
  RE_REQUIRE(stats.ok());
  const StoreStatistics& report = stats.value();
  machine.model.state_digest = report.state_digest;
  RE_CHECK_EQ(report.envelope_count, machine.model.envelopes.size());
  RE_CHECK_EQ(report.decision_count, machine.model.decisions.size());
  RE_CHECK_EQ(report.committed_key_count, machine.model.committed.size());
  RE_CHECK_EQ(report.usage_entry_count, machine.model.usage_entries);
  RE_CHECK_EQ(report.observation_count, machine.model.observations);
  RE_CHECK_EQ(report.compaction_count, machine.model.compactions);
  // The control epoch advances by exactly one per publication and by nothing else, so the model's
  // count is an exact expectation rather than a bound. That is what fences stale authority: a
  // determination recorded under any other epoch is not a determination about the current one.
  RE_CHECK_EQ(report.epoch, machine.model.epoch);
  RE_CHECK_EQ(report.state_sequence, machine.model.state_sequence);
  RE_CHECK(report.state_digest.known());
}

void check_envelopes(Machine& machine) {
  for (const auto& pair : machine.model.envelopes) {
    const EnvelopeModel& spec = pair.second;
    if (spec.retired) {
      // A retired envelope has no current revision. The identifier is still known, which is the
      // difference the status reports: "no envelope with this identifier" and "this envelope has
      // no current revision" are distinct answers, and the second is the accurate one here.
      const Result<EnvelopeView> current = machine.service->get_envelope(spec.id);
      RE_CHECK(!current.ok());
      if (!current.ok()) RE_CHECK_EQ(current.status().code(), StatusCode::EnvelopeNotCurrent);
      const Result<std::vector<EnvelopeView>> scoped = machine.service->find_by_scope(spec.scope);
      RE_REQUIRE(scoped.ok());
      for (const EnvelopeView& view : scoped.value()) {
        RE_CHECK(!(view.envelope.id == spec.id));
      }
    } else {
      const Result<EnvelopeView> current = machine.service->get_envelope(spec.id);
      RE_REQUIRE(current.ok());
      // Revision monotonicity: a revision never moves backwards, and the digest an envelope
      // reports is exactly the digest of the revision the model recorded.
      const auto observed = machine.model.observed_revision.find(spec.id);
      if (observed != machine.model.observed_revision.end()) {
        RE_CHECK(current.value().envelope.revision >= observed->second);
      }
      machine.model.observed_revision[spec.id] = current.value().envelope.revision;
      RE_CHECK_EQ(current.value().envelope.revision, spec.revision);
      RE_CHECK_EQ(current.value().record_digest, spec.record_digest);
      RE_CHECK(current.value().current);
      RE_CHECK(!current.value().tombstoned);
      RE_CHECK_EQ(current.value().envelope.scope, spec.scope);
      RE_CHECK_EQ(current.value().envelope.identity_digest, spec.identity_digest);
      RE_CHECK_EQ(current.value().envelope.require_binding_confirmation, spec.require_binding);
      RE_CHECK_EQ(current.value().record_digest, spec.last_record_digest);
    }
    // The highest revision an envelope ever carried stays readable with the digest it carried.
    const Result<EnvelopeView> last =
        machine.service->get_envelope_revision(spec.id, spec.last_revision);
    RE_REQUIRE(last.ok());
    RE_CHECK_EQ(last.value().record_digest, spec.last_record_digest);
    RE_CHECK_EQ(last.value().envelope.revision, spec.last_revision);
    // Canonical digest stability and declaration-order independence: the digest of a revision does
    // not depend on the order its dimensions were declared in, and the encoding is a fixed point.
    Envelope reordered = last.value().envelope;
    std::reverse(reordered.dimensions.begin(), reordered.dimensions.end());
    RE_CHECK_EQ(content_digest(reordered), content_digest(last.value().envelope));
    RE_CHECK_EQ(record_digest(reordered), record_digest(last.value().envelope));
    RE_CHECK_EQ(canonical_envelope(reordered), canonical_envelope(last.value().envelope));
    const Result<Envelope> decoded = decode_envelope(canonical_envelope(reordered));
    RE_REQUIRE(decoded.ok());
    RE_CHECK_EQ(record_digest(decoded.value()), spec.last_record_digest);
  }
}

void check_committed(Machine& machine) {
  for (const auto& pair : machine.model.envelopes) {
    const Result<CommittedMap> live = machine.service->committed_usage(pair.first);
    RE_REQUIRE(live.ok());
    std::size_t expected = 0;
    for (const auto& row : machine.model.committed) {
      if (row.first.envelope_id != pair.first) continue;
      ++expected;
      const auto found = live.value().find(row.first);
      RE_REQUIRE(found != live.value().end());
      // Accounting closure: the durable fold is exactly the sum of the deltas that were accepted.
      RE_CHECK_EQ(found->second.value, row.second);
    }
    RE_CHECK_EQ(live.value().size(), expected);
  }
}

// No authority duplication: when two current envelopes claim one scope the runtime refuses to
// choose, reports the ambiguity, and never names an authority it could not verify.
void check_scope_uniqueness(Machine& machine) {
  for (const auto& pair : machine.model.envelopes) {
    const EnvelopeModel& spec = pair.second;
    if (spec.retired) continue;
    const Result<std::vector<EnvelopeView>> found = machine.service->find_by_scope(spec.scope);
    RE_REQUIRE(found.ok());
    if (scope_is_ambiguous(machine.model, spec.scope)) {
      RE_CHECK(found.value().size() >= 2U);
      const EvaluationRequest request =
          build_request(spec, kWatt, DimensionKind::PowerDrawWatts, 1U, "ambiguity-probe",
                        kBase + static_cast<Timestamp>(machine.step), false, true);
      const Result<EvaluationResult> evaluated = machine.service->evaluate(request);
      RE_REQUIRE(evaluated.ok());
      RE_CHECK_EQ(evaluated.value().reason, StatusCode::DuplicateIdentity);
      RE_CHECK_EQ(evaluated.value().outcome, Outcome::Denied);
      RE_CHECK(evaluated.value().envelope_id.empty());
      return;
    }
    RE_CHECK_EQ(found.value().size(), std::size_t{1});
  }
}

// The residual arithmetic is checked against the documented formula, computed from the committed
// figures the store itself reports, so the check is independent of how the evaluation is written.
void check_residual_arithmetic(Machine& machine) {
  for (const auto& pair : machine.model.envelopes) {
    const EnvelopeModel& spec = pair.second;
    if (spec.retired || scope_is_ambiguous(machine.model, spec.scope)) continue;
    const Nanounits quantity = spec.quantum == 0U ? kWatt : spec.quantum;
    const EvaluationRequest request =
        build_request(spec, quantity, DimensionKind::PowerDrawWatts, 1U, "residual-probe",
                      kBase + static_cast<Timestamp>(machine.step), spec.identity_digest.known(), true);
    const Result<EvaluationResult> evaluated = machine.service->evaluate(request);
    RE_REQUIRE(evaluated.ok());
    const EvaluationResult& result = evaluated.value();
    RE_CHECK_EQ(result.envelope_id, spec.id);
    RE_CHECK_EQ(result.envelope_revision, spec.revision);
    RE_CHECK_EQ(result.envelope_digest, spec.record_digest);
    RE_REQUIRE(result.dimensions.size() == std::size_t{1});
    const DimensionResult& item = result.dimensions.front();
    RE_CHECK_EQ(item.kind, DimensionKind::PowerDrawWatts);
    RE_REQUIRE(item.limit.has_value());
    RE_CHECK_EQ(*item.limit, spec.limit);
    RE_CHECK_EQ(item.reserved, spec.reserved);
    if (!has_committed_row(machine.model, spec.id, DimensionKind::PowerDrawWatts)) {
      // Missing is not zero: with no recorded commitment and no declared committed figure the
      // residual is unknown, the reported commitment is absent, and nothing is granted.
      RE_CHECK(!item.committed.has_value());
      RE_CHECK(!item.residual.has_value());
      RE_CHECK(!item.headroom_after.has_value());
      RE_CHECK(item.outcome != DimensionOutcome::Satisfied);
      RE_CHECK(result.outcome != Outcome::Granted);
      RE_CHECK_EQ(result.reason, StatusCode::IndeterminateUnknownCommitted);
      continue;
    }
    if (any_negative_row(machine.model, spec.id, DimensionKind::PowerDrawWatts)) {
      // A commitment released below zero is not capacity and cannot be interpreted.
      RE_CHECK(!item.committed.has_value());
      RE_CHECK(!item.residual.has_value());
      RE_CHECK(result.outcome != Outcome::Granted);
      continue;
    }
    const Nanounits total =
        static_cast<Nanounits>(committed_total(machine.model, spec.id, DimensionKind::PowerDrawWatts));
    RE_REQUIRE(item.committed.has_value());
    RE_CHECK_EQ(*item.committed, total);
    const Nanounits effective =
        spec.quantum == 0U ? total : round_up_to_multiple(total, spec.quantum).value;
    RE_REQUIRE(item.effective_committed.has_value());
    RE_CHECK_EQ(*item.effective_committed, effective);
    const Nanounits available = spec.limit - spec.reserved;
    const Nanounits expected_residual = effective >= available ? 0U : available - effective;
    RE_REQUIRE(item.residual.has_value());
    RE_CHECK_EQ(*item.residual, expected_residual);
    RE_CHECK_EQ(item.over_committed, effective > available);
    RE_CHECK_EQ(item.over_committed_by, effective > available ? effective - available : 0U);
    RE_REQUIRE(item.headroom_after.has_value());
    RE_CHECK_EQ(*item.headroom_after, quantity > expected_residual ? 0U : expected_residual - quantity);
    if (effective > available) {
      RE_CHECK_EQ(result.outcome, Outcome::Denied);
      RE_CHECK_EQ(result.reason, StatusCode::RejectedHardLimit);
    } else if (quantity > expected_residual) {
      RE_CHECK_EQ(result.outcome, Outcome::Denied);
      RE_CHECK_EQ(result.reason, StatusCode::RejectedInsufficientResidual);
    } else {
      RE_CHECK_EQ(result.outcome, Outcome::Granted);
      RE_CHECK_EQ(result.reason, StatusCode::Ok);
      RE_CHECK_EQ(result.stage, RefusalStage::None);
    }
  }
}

// A dimension nobody ever commits against stays unknown: an unevidenced dimension is never read as
// zero capacity and is never permitted.
void check_unknown_dimension_is_never_zero(Machine& machine) {
  for (const auto& pair : machine.model.envelopes) {
    const EnvelopeModel& spec = pair.second;
    if (spec.retired || scope_is_ambiguous(machine.model, spec.scope)) continue;
    const EvaluationRequest request =
        build_request(spec, kNanounitsPerUnit, DimensionKind::SpaceRackUnits, 1U, "space-probe",
                      kBase + static_cast<Timestamp>(machine.step), spec.identity_digest.known(), true);
    const Result<EvaluationResult> evaluated = machine.service->evaluate(request);
    RE_REQUIRE(evaluated.ok());
    RE_REQUIRE(evaluated.value().dimensions.size() == std::size_t{1});
    const DimensionResult& item = evaluated.value().dimensions.front();
    RE_CHECK(!item.committed.has_value());
    RE_CHECK(!item.residual.has_value());
    RE_CHECK(!item.headroom_after.has_value());
    RE_CHECK(item.outcome == DimensionOutcome::Indeterminate ||
             item.outcome == DimensionOutcome::NotDeclared);
    RE_CHECK(evaluated.value().outcome != Outcome::Granted);
  }
}

// Control-epoch fencing: a determination recorded under an epoch that has since been retired is
// never reported valid, and a decision that granted nothing is never reported valid either.
void check_fencing(Machine& machine) {
  if (machine.model.decisions.empty()) return;
  const Result<StoreStatistics> stats = machine.service->statistics();
  RE_REQUIRE(stats.ok());
  std::vector<std::size_t> probes;
  probes.push_back(machine.model.decisions.size() - 1U);
  if (machine.model.decisions.size() > 1U) probes.push_back(machine.model.decisions.size() - 2U);
  probes.push_back(static_cast<std::size_t>(pick(machine.engine, machine.model.decisions.size())));
  for (const std::size_t index : probes) {
    const DecisionModel& decision = machine.model.decisions[index];
    VerifyInput verify;
    verify.decision_id = decision.id;
    const Result<VerifyResult> verdict = machine.service->verify_authority(verify);
    RE_REQUIRE(verdict.ok());
    RE_CHECK_EQ(verdict.value().record.decision_id, decision.id);
    RE_CHECK_EQ(verdict.value().record.outcome, decision.outcome);
    RE_CHECK_EQ(verdict.value().verdict.granted_control_epoch, decision.control_epoch);
    RE_CHECK_EQ(verdict.value().verdict.current_control_epoch, stats.value().epoch);
    if (decision.outcome != Outcome::Granted) {
      RE_CHECK_EQ(verdict.value().verdict.state, AuthorityState::NotAGrant);
      continue;
    }
    if (decision.control_epoch != stats.value().epoch) {
      RE_CHECK(!verdict.value().verdict.valid());
    }
  }
}

void check_invariants(Machine& machine) {
  check_counters(machine);
  check_envelopes(machine);
  check_committed(machine);
  check_scope_uniqueness(machine);
  check_residual_arithmetic(machine);
  check_unknown_dimension_is_never_zero(machine);
  check_fencing(machine);
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------
void step_declare_new(Machine& machine, const EnvelopeModel& spec) {
  machine.note("declare " + spec.id + " limit=" + std::to_string(spec.limit));
  DeclareInput input;
  input.envelope = spec.declared;
  input.idempotency_key = "declare-" + std::to_string(machine.step);
  input.requested_at = kBase + static_cast<Timestamp>(machine.step);
  const Result<EnvelopeDeclaration> declared = machine.service->declare(input);
  RE_REQUIRE(declared.ok());
  RE_CHECK(!declared.value().replayed);
  RE_CHECK_EQ(declared.value().revision, 1ULL);
  RE_CHECK(declared.value().record_digest.known());
  EnvelopeModel stored = spec;
  stored.revision = 1U;
  stored.last_revision = 1U;
  stored.record_digest = declared.value().record_digest;
  stored.last_record_digest = declared.value().record_digest;
  stored.declared_revision = 1U;
  stored.declared_record_digest = declared.value().record_digest;
  stored.last_declare = input;
  machine.model.envelopes[stored.id] = stored;
  machine.model.epoch += 1U;
  machine.model.state_sequence += 1U;
}

void step_declare_replay(Machine& machine, const std::string& id) {
  const EnvelopeModel& spec = machine.model.envelopes.at(id);
  RE_REQUIRE(spec.last_declare.has_value());
  machine.note("declare replay " + id);
  const Result<EnvelopeDeclaration> declared = machine.service->declare(*spec.last_declare);
  RE_REQUIRE(declared.ok());
  // Idempotent replay before staleness: the recorded answer comes back unchanged - the revision
  // the declaration created, which need not still be the current revision - and nothing is
  // published, so the control epoch does not move.
  RE_CHECK(declared.value().replayed);
  RE_CHECK_EQ(declared.value().revision, spec.declared_revision);
  RE_CHECK_EQ(declared.value().record_digest, spec.declared_record_digest);
}

void step_declare_conflict(Machine& machine, const std::string& id) {
  const EnvelopeModel& spec = machine.model.envelopes.at(id);
  machine.note("declare conflict " + id);
  DeclareInput input;
  input.envelope = build_envelope(spec, kBase + static_cast<Timestamp>(machine.step));
  input.envelope.find(DimensionKind::PowerDrawWatts)->hard_limit = spec.limit + kWatt;
  input.idempotency_key = "declare-conflict-" + std::to_string(machine.step);
  input.requested_at = kBase + static_cast<Timestamp>(machine.step);
  const Result<EnvelopeDeclaration> declared = machine.service->declare(input);
  RE_CHECK(!declared.ok());
  if (!declared.ok()) RE_CHECK_EQ(declared.status().code(), StatusCode::StoreExists);
}

void step_revise(Machine& machine, const std::string& id) {
  EnvelopeModel& spec = machine.model.envelopes.at(id);
  const Nanounits limit = 60ULL * kWatt + pick(machine.engine, 40ULL) * kWatt;
  const Nanounits reserved = pick(machine.engine, 5ULL) * kWatt;
  const bool advance_identity = spec.identity_digest.known() && pick(machine.engine, 2U) == 0U;
  machine.note("revise " + id + " limit=" + std::to_string(limit));
  ReviseInput input;
  input.envelope = build_envelope(spec, kBase + static_cast<Timestamp>(machine.step));
  input.envelope.find(DimensionKind::PowerDrawWatts)->hard_limit = limit;
  input.envelope.find(DimensionKind::PowerDrawWatts)->reserved = reserved;
  if (advance_identity) input.envelope.identity_digest = digest_of("identity-record-advanced");
  input.expected_current_revision = spec.revision;
  input.idempotency_key = "revise-" + std::to_string(machine.step);
  input.requested_at = kBase + static_cast<Timestamp>(machine.step);
  const Result<EnvelopeRevision> revised = machine.service->revise(input);
  RE_REQUIRE(revised.ok());
  RE_CHECK(!revised.value().replayed);
  RE_CHECK_EQ(revised.value().previous_revision, spec.revision);
  RE_CHECK_EQ(revised.value().revision, spec.revision + 1U);
  spec.limit = limit;
  spec.reserved = reserved;
  if (advance_identity) spec.identity_digest = input.envelope.identity_digest;
  spec.revision = revised.value().revision;
  spec.last_revision = revised.value().revision;
  spec.record_digest = revised.value().record_digest;
  spec.last_record_digest = revised.value().record_digest;
  spec.last_revise = input;
  machine.model.epoch += 1U;
  machine.model.state_sequence += 1U;
}

void step_revise_replay(Machine& machine, const std::string& id) {
  const EnvelopeModel& spec = machine.model.envelopes.at(id);
  RE_REQUIRE(spec.last_revise.has_value());
  machine.note("revise replay " + id);
  const Result<EnvelopeRevision> revised = machine.service->revise(*spec.last_revise);
  RE_REQUIRE(revised.ok());
  RE_CHECK(revised.value().replayed);
  RE_CHECK_EQ(revised.value().revision, spec.last_revision);
  RE_CHECK_EQ(revised.value().record_digest, spec.last_record_digest);
}

void step_revise_stale(Machine& machine, const std::string& id) {
  const EnvelopeModel& spec = machine.model.envelopes.at(id);
  RE_REQUIRE(spec.revision >= 2U);
  machine.note("revise stale " + id);
  ReviseInput input;
  input.envelope = build_envelope(spec, kBase + static_cast<Timestamp>(machine.step));
  input.expected_current_revision = spec.revision - 1U;
  input.idempotency_key = "revise-stale-" + std::to_string(machine.step);
  input.requested_at = kBase + static_cast<Timestamp>(machine.step);
  const Result<EnvelopeRevision> revised = machine.service->revise(input);
  RE_CHECK(!revised.ok());
  if (!revised.ok()) RE_CHECK_EQ(revised.status().code(), StatusCode::StaleRevision);
}

void step_record_usage(Machine& machine, const std::string& id, bool replay) {
  EnvelopeModel& spec = machine.model.envelopes.at(id);
  UsageInput input;
  std::string principal = "principal-a";
  std::int64_t delta = 0;
  if (replay) {
    RE_REQUIRE(spec.last_usage.has_value());
    input = *spec.last_usage;
    principal = input.deltas.front().principal;
    machine.note("usage replay " + id);
  } else {
    principal = pick(machine.engine, 2U) == 0U ? "principal-a" : "principal-b";
    const std::int64_t magnitude =
        static_cast<std::int64_t>((1U + pick(machine.engine, 4U)) * 5ULL * kWatt);
    const bool release = pick(machine.engine, 3U) == 0U;
    delta = release ? -magnitude : magnitude;
    input.envelope_id = id;
    input.idempotency_key = "usage-" + std::to_string(machine.step);
    input.requested_at = kBase + static_cast<Timestamp>(machine.step);
    UsageDelta entry;
    entry.envelope_id = id;
    entry.kind = DimensionKind::PowerDrawWatts;
    entry.principal = principal;
    entry.delta = delta;
    entry.source = "facility-capacity";
    input.deltas.push_back(entry);
    machine.note("usage " + id + " " + std::to_string(delta));
  }
  const Result<UsageResult> recorded = machine.service->record_usage(input);
  RE_REQUIRE(recorded.ok());
  if (replay) {
    // A retry is a replay: nothing is appended, nothing is counted twice, and the answer names the
    // revision the claim was recorded against.
    RE_CHECK_EQ(recorded.value().entries_appended, 0ULL);
    RE_CHECK_EQ(recorded.value().entries_replayed, 1ULL);
    RE_CHECK_EQ(recorded.value().envelope_revision, spec.usage_revision);
    return;
  }
  RE_CHECK_EQ(recorded.value().entries_appended, 1ULL);
  RE_CHECK_EQ(recorded.value().entries_replayed, 0ULL);
  RE_CHECK_EQ(recorded.value().envelope_revision, spec.revision);
  const CommittedKey key{id, DimensionKind::PowerDrawWatts, principal, std::string()};
  const auto current = machine.model.committed.find(key);
  const std::int64_t base = current == machine.model.committed.end() ? 0 : current->second;
  machine.model.committed[key] = base + delta;
  machine.model.usage_entries += 1U;
  machine.model.epoch += 1U;
  machine.model.state_sequence += 1U;
  spec.usage_revision = spec.revision;
  spec.last_usage = input;
}

void step_record_observation(Machine& machine, const std::string& id, bool replay) {
  EnvelopeModel& spec = machine.model.envelopes.at(id);
  ObservationInput input;
  if (replay) {
    RE_REQUIRE(spec.last_observation.has_value());
    input = *spec.last_observation;
    machine.note("observation replay " + id);
  } else {
    input.envelope_id = id;
    input.idempotency_key = "observation-" + std::to_string(machine.step);
    input.requested_at = kBase + static_cast<Timestamp>(machine.step);
    ObservationEntry entry;
    entry.envelope_id = id;
    entry.kind = DimensionKind::PowerDrawWatts;
    entry.status = MeasureStatus::Measured;
    entry.value = (1U + pick(machine.engine, 90U)) * kWatt;
    entry.principal = "principal-a";
    entry.observed_at = input.requested_at;
    entry.source = "meter";
    input.entries.push_back(entry);
    machine.note("observation " + id + " value=" + std::to_string(entry.value));
  }
  const Result<ObservationResult> recorded = machine.service->record_observation(input);
  RE_REQUIRE(recorded.ok());
  if (replay) {
    RE_CHECK_EQ(recorded.value().stored, 0ULL);
    RE_CHECK_EQ(recorded.value().replayed, 1ULL);
    return;
  }
  RE_CHECK_EQ(recorded.value().stored, 1ULL);
  RE_CHECK_EQ(recorded.value().replayed, 0ULL);
  machine.model.observations += 1U;
  machine.model.epoch += 1U;
  machine.model.state_sequence += 1U;
  spec.last_observation = input;
}

enum class BindingMode { Matching, Missing, StaleDigest, StaleGeneration };

void record_decision(Machine& machine, const DecisionRecord& record) {
  DecisionModel decision;
  decision.id = record.decision_id;
  decision.sequence = record.sequence;
  decision.control_epoch = record.control_epoch;
  decision.outcome = record.outcome;
  decision.reason = record.reason;
  machine.model.decisions.push_back(decision);
  machine.model.epoch += 1U;
  machine.model.state_sequence += 1U;
}

void step_authorize(Machine& machine, const std::string& id, BindingMode mode) {
  EnvelopeModel& spec = machine.model.envelopes.at(id);
  const Nanounits quantity = (1U + pick(machine.engine, 12U)) * 5ULL * kWatt;
  const bool with_identity = mode == BindingMode::Matching || mode == BindingMode::StaleDigest;
  const bool matching = mode == BindingMode::Matching;
  EvaluationRequest request =
      build_request(spec, quantity, DimensionKind::PowerDrawWatts, 1U,
                    "authorize-" + std::to_string(machine.step),
                    kBase + static_cast<Timestamp>(machine.step), with_identity, matching);
  if (mode == BindingMode::StaleGeneration) request.scope.identity.generation += 1U;
  machine.note("authorize " + id + " quantity=" + std::to_string(quantity));
  const Result<AuthorizeResult> authorized = machine.service->authorize(request);
  RE_REQUIRE(authorized.ok());

  if (mode == BindingMode::StaleGeneration) {
    // A generation the envelope is not bound to does not resolve to it at all: the request is
    // refused without naming an authority, and no decision record is written, because a record
    // that named no authority is a record nothing could later be fenced against.
    RE_CHECK(!authorized.value().replayed);
    RE_CHECK_EQ(authorized.value().evaluation.outcome, Outcome::Denied);
    RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::EnvelopeNotFound);
    RE_CHECK(authorized.value().evaluation.envelope_id.empty());
    return;
  }

  if (!binding_confirmed(spec, with_identity, matching)) {
    // Generation and digest binding are checked before the arithmetic, so a request that does not
    // confirm the binding never reaches a dimension, and the refusal is recorded as evidence.
    RE_CHECK(!authorized.value().replayed);
    RE_CHECK_EQ(authorized.value().evaluation.outcome, Outcome::Denied);
    RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::IdentityBindingMismatch);
    RE_CHECK_EQ(authorized.value().evaluation.stage, RefusalStage::GenerationBinding);
    RE_CHECK(authorized.value().evaluation.dimensions.empty());
    RE_CHECK_EQ(authorized.value().record.scope, spec.scope);
    RE_CHECK_EQ(authorized.value().record.identity, spec.scope.identity);
    RE_CHECK_EQ(authorized.value().record.identity_digest, spec.identity_digest);
    RE_CHECK_EQ(authorized.value().record.outcome, Outcome::Denied);
    RE_CHECK_EQ(authorized.value().record.reason, StatusCode::IdentityBindingMismatch);
    record_decision(machine, authorized.value().record);
    spec.last_authorize = request;
    spec.last_decision_id = authorized.value().record.decision_id;
    spec.last_decision_sequence = authorized.value().record.sequence;
    spec.last_decision_outcome = authorized.value().record.outcome;
    return;
  }

  // A confirmed binding reaches the arithmetic, and the recorded decision names exactly the
  // authority, the generation and the digest that answered.
  RE_CHECK(!authorized.value().replayed);
  RE_CHECK_EQ(authorized.value().record.scope, spec.scope);
  RE_CHECK_EQ(authorized.value().record.identity, spec.scope.identity);
  RE_CHECK_EQ(authorized.value().record.identity_digest, spec.identity_digest);
  RE_CHECK_EQ(authorized.value().record.identity_checked, !request.identity.identity.id.empty());
  RE_CHECK_EQ(authorized.value().record.envelope_revision, spec.revision);
  RE_CHECK_EQ(authorized.value().record.envelope_digest, spec.record_digest);
  RE_CHECK_EQ(authorized.value().record.control_epoch, machine.model.epoch + 1U);
  RE_CHECK_EQ(authorized.value().record.request_digest, authorized.value().evaluation.request_digest);
  const bool known = has_committed_row(machine.model, id, DimensionKind::PowerDrawWatts) &&
                     !any_negative_row(machine.model, id, DimensionKind::PowerDrawWatts);
  if (!known) {
    RE_CHECK_EQ(authorized.value().evaluation.outcome, Outcome::Indeterminate);
    RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::IndeterminateUnknownCommitted);
  } else {
    const Nanounits total = static_cast<Nanounits>(
        committed_total(machine.model, id, DimensionKind::PowerDrawWatts));
    const Nanounits effective =
        spec.quantum == 0U ? total : round_up_to_multiple(total, spec.quantum).value;
    const Nanounits available = spec.limit - spec.reserved;
    const Nanounits residual = effective >= available ? 0U : available - effective;
    if (effective > available) {
      RE_CHECK_EQ(authorized.value().evaluation.outcome, Outcome::Denied);
      RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::RejectedHardLimit);
    } else if (quantity > residual) {
      RE_CHECK_EQ(authorized.value().evaluation.outcome, Outcome::Denied);
      RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::RejectedInsufficientResidual);
    } else {
      RE_CHECK_EQ(authorized.value().evaluation.outcome, Outcome::Granted);
      RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::Ok);
      RE_CHECK(authorized.value().record.grant_digest.known());
    }
  }
  record_decision(machine, authorized.value().record);
  spec.last_authorize = request;
  spec.last_decision_id = authorized.value().record.decision_id;
  spec.last_decision_sequence = authorized.value().record.sequence;
  spec.last_decision_outcome = authorized.value().record.outcome;

  // The recorded decision is durable and readable exactly as recorded, and a key retry returns it
  // unchanged rather than re-deciding it against the authority as it stands now.
  const Result<DecisionRecord> stored = machine.service->get_decision(spec.last_decision_id);
  RE_REQUIRE(stored.ok());
  RE_CHECK_EQ(stored.value().decision_digest, authorized.value().decision_digest);
  RE_CHECK_EQ(stored.value().idempotency_key, request.idempotency_key);
  const Result<AuthorizeResult> retry = machine.service->authorize(request);
  RE_REQUIRE(retry.ok());
  RE_CHECK(retry.value().replayed);
  RE_CHECK_EQ(retry.value().record.sequence, spec.last_decision_sequence);
  RE_CHECK_EQ(retry.value().record.outcome, spec.last_decision_outcome);
}

void step_authorize_replay(Machine& machine, const std::string& id) {
  const EnvelopeModel& spec = machine.model.envelopes.at(id);
  RE_REQUIRE(spec.last_authorize.has_value());
  const std::size_t before = machine.model.decisions.size();
  machine.note("authorize replay " + id);
  const Result<AuthorizeResult> authorized = machine.service->authorize(*spec.last_authorize);
  RE_REQUIRE(authorized.ok());
  RE_CHECK(authorized.value().replayed);
  RE_CHECK_EQ(machine.model.decisions.size(), before);
  RE_CHECK_EQ(authorized.value().record.sequence, spec.last_decision_sequence);
  RE_CHECK_EQ(authorized.value().record.outcome, spec.last_decision_outcome);
  RE_CHECK_EQ(authorized.value().decision_digest, authorized.value().record.decision_digest);
}

void step_retire(Machine& machine, const std::string& id) {
  EnvelopeModel& spec = machine.model.envelopes.at(id);
  machine.note("retire " + id);
  TombstoneInput input;
  input.envelope_id = id;
  input.expected_current_revision = spec.revision;
  input.reason = "decommissioned";
  input.request_id = static_cast<std::uint64_t>(machine.step) + 1U;
  input.requested_at = kBase + static_cast<Timestamp>(machine.step);
  const Result<TombstoneResult> retired = machine.service->tombstone(input);
  RE_REQUIRE(retired.ok());
  RE_CHECK(!retired.value().replayed);
  RE_CHECK_EQ(retired.value().superseded_revision, spec.revision);
  spec.retired = true;
  spec.revision = 0U;
  spec.last_retirement = input;
  machine.model.epoch += 1U;
  machine.model.state_sequence += 1U;
}

void step_retire_replay(Machine& machine, const std::string& id) {
  const EnvelopeModel& spec = machine.model.envelopes.at(id);
  RE_REQUIRE(spec.last_retirement.has_value());
  machine.note("retire replay " + id);
  const Result<TombstoneResult> retired = machine.service->tombstone(*spec.last_retirement);
  RE_REQUIRE(retired.ok());
  RE_CHECK(retired.value().replayed);
}

void step_compact(Machine& machine) {
  machine.note("compact");
  const Result<StoreStatistics> before = machine.service->statistics();
  RE_REQUIRE(before.ok());
  const Status compacted = machine.service->compact();
  RE_REQUIRE(compacted.ok());
  machine.model.compactions += 1U;
  machine.model.epoch += 1U;
  // A compaction is a rewrite: it does not change what the store says, it does not advance the
  // journal sequence, and the store it produced is usable and can be mutated again.
  const Result<StoreStatistics> stats = machine.service->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().state_digest, before.value().state_digest);
  RE_CHECK_EQ(stats.value().state_sequence, before.value().state_sequence);
}

void step_reopen(Machine& machine) {
  machine.note("reopen");
  // Everything an observer can see is captured before the handle is closed and compared after it
  // is reopened: a restart must reproduce the authority exactly, not approximately.
  const Result<StoreStatistics> before = machine.service->statistics();
  RE_REQUIRE(before.ok());
  std::map<std::string, Digest> digests;
  std::vector<std::string> decision_ids;
  for (const auto& pair : machine.model.envelopes) {
    const Result<EnvelopeView> view =
        machine.service->get_envelope_revision(pair.first, pair.second.last_revision);
    RE_REQUIRE(view.ok());
    digests[pair.first] = view.value().record_digest;
  }
  for (const DecisionModel& decision : machine.model.decisions) decision_ids.push_back(decision.id);

  machine.service.reset();
  Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(machine.path, existing_options());
  RE_REQUIRE(reopened.ok());
  machine.service = std::move(reopened.value());

  const Result<StoreStatistics> after = machine.service->statistics();
  RE_REQUIRE(after.ok());
  RE_CHECK_EQ(after.value().state_digest, before.value().state_digest);
  RE_CHECK_EQ(after.value().epoch, before.value().epoch);
  RE_CHECK_EQ(after.value().state_sequence, before.value().state_sequence);
  RE_CHECK_EQ(after.value().decision_count, before.value().decision_count);
  RE_CHECK_EQ(after.value().envelope_count, before.value().envelope_count);
  RE_CHECK_EQ(after.value().committed_key_count, before.value().committed_key_count);
  RE_CHECK_EQ(after.value().usage_entry_count, before.value().usage_entry_count);
  for (const auto& pair : machine.model.envelopes) {
    const Result<EnvelopeView> view =
        machine.service->get_envelope_revision(pair.first, pair.second.last_revision);
    RE_REQUIRE(view.ok());
    RE_CHECK_EQ(view.value().record_digest, digests.at(pair.first));
  }
  for (const auto& pair : machine.model.committed) {
    const Result<CommittedMap> live = machine.service->committed_usage(pair.first.envelope_id);
    RE_REQUIRE(live.ok());
    const auto found = live.value().find(pair.first);
    RE_REQUIRE(found != live.value().end());
    RE_CHECK_EQ(found->second.value, pair.second);
  }
  for (const std::string& id : decision_ids) {
    RE_CHECK(machine.service->get_decision(id).ok());
  }
}

void step_verify(Machine& machine) {
  if (machine.model.decisions.empty()) return;
  const DecisionModel& decision =
      machine.model.decisions[pick(machine.engine, machine.model.decisions.size())];
  machine.note("verify " + decision.id);
  VerifyInput input;
  input.decision_id = decision.id;
  const Result<VerifyResult> verdict = machine.service->verify_authority(input);
  RE_REQUIRE(verdict.ok());
  RE_CHECK_EQ(verdict.value().record.decision_id, decision.id);
  RE_CHECK_EQ(verdict.value().record.outcome, decision.outcome);
  RE_CHECK_EQ(verdict.value().record.reason, decision.reason);
  RE_CHECK(verdict.value().record.decision_digest.known());
  if (decision.outcome != Outcome::Granted) {
    RE_CHECK_EQ(verdict.value().verdict.state, AuthorityState::NotAGrant);
  }
}

// No authority duplication is created silently: two envelopes may be declared for one scope, and
// from then on every determination for that scope is refused as ambiguous rather than resolved by
// an arbitrary choice.
void step_duplicate_scope(Machine& machine) {
  if (machine.model.duplicate_scope_declared) return;
  machine.note("declare two envelopes for one scope");
  const Digest identity = digest_of("duplicate-scope-record");
  EnvelopeModel first = make_spec("machine-duplicate-a", 1U, 100ULL * kWatt, 0U, identity, false);
  first.scope.identity.id = "machine-tenant-duplicate";
  first.declared.scope = first.scope;
  EnvelopeModel second = make_spec("machine-duplicate-b", 1U, 90ULL * kWatt, 0U, identity, false);
  second.scope.identity.id = "machine-tenant-duplicate";
  second.declared.scope = second.scope;
  EnvelopeModel* const targets[] = {&first, &second};
  for (EnvelopeModel* spec : targets) {
    DeclareInput input;
    input.envelope = spec->declared;
    input.idempotency_key = "declare-" + spec->id;
    input.requested_at = kBase + static_cast<Timestamp>(machine.step);
    const Result<EnvelopeDeclaration> declared = machine.service->declare(input);
    RE_REQUIRE(declared.ok());
    spec->revision = 1U;
    spec->last_revision = 1U;
    spec->record_digest = declared.value().record_digest;
    spec->last_record_digest = declared.value().record_digest;
    spec->declared_revision = 1U;
    spec->declared_record_digest = declared.value().record_digest;
    spec->last_declare = input;
    machine.model.envelopes[spec->id] = *spec;
    machine.model.epoch += 1U;
    machine.model.state_sequence += 1U;
  }
  machine.model.duplicate_scope_declared = true;
  const EvaluationRequest request = build_request(first, kWatt, DimensionKind::PowerDrawWatts, 1U,
                                                  "ambiguity-probe",
                                                  kBase + static_cast<Timestamp>(machine.step),
                                                  false, true);
  const std::size_t before = machine.model.decisions.size();
  const Result<EvaluationResult> evaluated = machine.service->evaluate(request);
  RE_REQUIRE(evaluated.ok());
  RE_CHECK_EQ(evaluated.value().reason, StatusCode::DuplicateIdentity);
  const Result<AuthorizeResult> authorized = machine.service->authorize(request);
  RE_REQUIRE(authorized.ok());
  RE_CHECK_EQ(authorized.value().evaluation.reason, StatusCode::DuplicateIdentity);
  RE_CHECK(authorized.value().evaluation.envelope_id.empty());
  // The ambiguity is reported rather than resolved, so nothing was recorded for it.
  RE_CHECK_EQ(machine.model.decisions.size(), before);
}

void step_read_paths(Machine& machine) {
  machine.note("read paths");
  const Result<std::vector<EnvelopeView>> listed = machine.service->list_envelopes();
  RE_REQUIRE(listed.ok());
  std::size_t expected_current = 0;
  for (const auto& pair : machine.model.envelopes) {
    if (!pair.second.retired) ++expected_current;
  }
  RE_CHECK_EQ(listed.value().size(), expected_current);
  for (const auto& pair : machine.model.envelopes) {
    const Result<std::vector<HistoryEntry>> history = machine.service->history(pair.first);
    RE_REQUIRE(history.ok());
    std::uint64_t previous_sequence = 0U;
    for (const HistoryEntry& entry : history.value()) {
      RE_CHECK_EQ(entry.envelope_id, pair.first);
      RE_CHECK(entry.sequence > previous_sequence);
      previous_sequence = entry.sequence;
    }
    const Result<std::vector<ObservationEntry>> observations =
        machine.service->observations(pair.first);
    RE_REQUIRE(observations.ok());
    for (const ObservationEntry& entry : observations.value()) {
      RE_CHECK_EQ(entry.envelope_id, pair.first);
    }
  }
  const Result<std::vector<DecisionRecord>> recent = machine.service->list_decisions(std::string(), 5U);
  RE_REQUIRE(recent.ok());
  RE_CHECK(recent.value().size() <= 5U);
  RE_CHECK(recent.value().size() <= machine.model.decisions.size());
}

// ---------------------------------------------------------------------------
// The machine itself
// ---------------------------------------------------------------------------
bool run_machine(const std::filesystem::path& path, std::uint64_t seed, int steps) {
  Machine machine;
  machine.path = path;
  machine.seed = seed;
  machine.steps = steps;
  machine.engine = std::mt19937_64(seed);
  Result<std::unique_ptr<EnvelopeService>> opened = EnvelopeService::open(path, create_options());
  if (!opened.ok()) {
    std::fprintf(stdout, "MACHINE could not open the store: %s\n", opened.status().describe().c_str());
    return false;
  }
  machine.service = std::move(opened.value());

  for (int index = 0; index < steps; ++index) {
    machine.step = index;
    const int before = testing::failure_count();
    const std::vector<std::string> declared = envelope_ids(machine.model);
    std::vector<std::string> missing;
    for (const char* const id : kEnvelopePool) {
      if (machine.model.envelopes.find(id) == machine.model.envelopes.end()) missing.push_back(id);
    }
    const std::vector<std::string> live = live_envelopes(machine.model);
    const int action = static_cast<int>(pick(machine.engine, 20U));
    switch (action) {
      case 0:
      case 1:
      case 2: {
        if (missing.empty()) break;
        const std::string id = missing[pick(machine.engine, missing.size())];
        const std::uint64_t generation = 3U + pick(machine.engine, 2U);
        const Nanounits limit = 60ULL * kWatt + pick(machine.engine, 40ULL) * kWatt;
        const Nanounits reserved = pick(machine.engine, 5ULL) * kWatt;
        Digest identity;
        if (id != "machine-envelope-c") identity = digest_of(id + "-identity");
        step_declare_new(machine, make_spec(id, generation, limit, reserved, identity, true));
        break;
      }
      case 3: {
        if (declared.empty()) break;
        step_declare_replay(machine, declared[pick(machine.engine, declared.size())]);
        break;
      }
      case 4: {
        if (live.empty()) break;
        step_declare_conflict(machine, live[pick(machine.engine, live.size())]);
        break;
      }
      case 5:
      case 6: {
        if (live.empty()) break;
        step_revise(machine, live[pick(machine.engine, live.size())]);
        break;
      }
      case 7: {
        const std::vector<std::string> revisable =
            select_ids(machine.model, [](const EnvelopeModel& spec) { return spec.last_revise.has_value(); });
        if (revisable.empty()) break;
        step_revise_replay(machine, revisable[pick(machine.engine, revisable.size())]);
        break;
      }
      case 8: {
        bool done = false;
        for (std::size_t attempt = 0; attempt < live.size() && !done; ++attempt) {
          const std::string id = live[pick(machine.engine, live.size())];
          if (machine.model.envelopes.at(id).revision < 2U) continue;
          step_revise_stale(machine, id);
          done = true;
        }
        break;
      }
      case 9:
      case 10:
      case 11: {
        if (live.empty()) break;
        step_record_usage(machine, live[pick(machine.engine, live.size())], false);
        break;
      }
      case 12: {
        const std::vector<std::string> claimed =
            select_ids(machine.model, [](const EnvelopeModel& spec) { return spec.last_usage.has_value(); });
        if (claimed.empty()) break;
        step_record_usage(machine, claimed[pick(machine.engine, claimed.size())], true);
        break;
      }
      case 13: {
        if (live.empty()) break;
        step_record_observation(machine, live[pick(machine.engine, live.size())], false);
        break;
      }
      case 14: {
        const std::vector<std::string> claimed = select_ids(
            machine.model, [](const EnvelopeModel& spec) { return spec.last_observation.has_value(); });
        if (claimed.empty()) break;
        step_record_observation(machine, claimed[pick(machine.engine, claimed.size())], true);
        break;
      }
      case 15: {
        // A new determination needs an envelope that is the only current claimant of its scope: an
        // ambiguous scope is refused without a record, which the ambiguity step and the invariant
        // check cover. A *replay* is not restricted that way, because a recorded determination
        // stays answerable even when the authority it named has since become ambiguous.
        const std::vector<std::string> authorizable =
            select_ids(machine.model, [&machine](const EnvelopeModel& spec) {
              return !spec.retired && !scope_is_ambiguous(machine.model, spec.scope);
            });
        if (authorizable.empty()) break;
        static const BindingMode kModes[] = {BindingMode::Matching, BindingMode::Missing,
                                             BindingMode::StaleDigest, BindingMode::StaleGeneration};
        step_authorize(machine, authorizable[pick(machine.engine, authorizable.size())],
                       kModes[pick(machine.engine, 4U)]);
        break;
      }
      case 16: {
        const std::vector<std::string> decided = select_ids(
            machine.model, [](const EnvelopeModel& spec) { return spec.last_authorize.has_value(); });
        if (decided.empty()) break;
        step_authorize_replay(machine, decided[pick(machine.engine, decided.size())]);
        break;
      }
      case 17: {
        const std::vector<std::string> retired =
            select_ids(machine.model, [](const EnvelopeModel& spec) { return spec.last_retirement.has_value(); });
        if (!retired.empty() && pick(machine.engine, 2U) == 0U) {
          step_retire_replay(machine, retired[pick(machine.engine, retired.size())]);
          break;
        }
        if (live.empty()) break;
        step_retire(machine, live[pick(machine.engine, live.size())]);
        break;
      }
      case 18:
        step_compact(machine);
        break;
      case 19: {
        const std::uint64_t choice = pick(machine.engine, 4U);
        if (choice == 0U) {
          step_reopen(machine);
        } else if (choice == 1U) {
          step_duplicate_scope(machine);
        } else if (choice == 2U) {
          step_verify(machine);
        } else {
          step_read_paths(machine);
        }
        break;
      }
      default:
        break;
    }
    check_invariants(machine);
    if (testing::failure_count() != before) {
      dump_failure(machine, "FAILED");
      return false;
    }
  }
  std::fprintf(stdout, "MACHINE completed %d steps with seed %llu; state digest %s\n", steps,
               static_cast<unsigned long long>(seed), machine.model.state_digest.to_string().c_str());
  std::fflush(stdout);
  return true;
}

}  // namespace

RE_TEST(randomized_lifecycle_holds_every_core_invariant) {
  const ScratchStore scratch("re-machine-primary");
  std::fprintf(stdout, "seed %llu, steps %d\n", static_cast<unsigned long long>(kPrimarySeed),
               kPrimarySteps);
  RE_CHECK(run_machine(scratch.path(), kPrimarySeed, kPrimarySteps));
}

RE_TEST(a_second_seed_explores_a_different_history) {
  const ScratchStore scratch("re-machine-secondary");
  std::fprintf(stdout, "seed %llu, steps %d\n", static_cast<unsigned long long>(kSecondarySeed),
               kSecondarySteps);
  RE_CHECK(run_machine(scratch.path(), kSecondarySeed, kSecondarySteps));
}

RE_TEST(the_same_seed_reproduces_the_same_state_in_another_store) {
  // The run is a pure function of its seed: the same seed over a fresh store produces the same
  // state commitment, which is what makes a reported failure reproducible rather than a report
  // about one particular interleaving of an unseeded run.
  const ScratchStore first("re-machine-reproduce-a");
  const ScratchStore second("re-machine-reproduce-b");
  RE_REQUIRE(run_machine(first.path(), kSecondarySeed, kReproducibilitySteps));
  Digest left;
  {
    const Result<std::unique_ptr<EnvelopeService>> reopened =
        EnvelopeService::open(first.path(), existing_options());
    RE_REQUIRE(reopened.ok());
    const Result<StoreStatistics> stats = reopened.value()->statistics();
    RE_REQUIRE(stats.ok());
    left = stats.value().state_digest;
  }
  RE_REQUIRE(run_machine(second.path(), kSecondarySeed, kReproducibilitySteps));
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(second.path(), existing_options());
  RE_REQUIRE(reopened.ok());
  const Result<StoreStatistics> stats = reopened.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(left, stats.value().state_digest);
}

int main(int argc, char** argv) {
  std::uint64_t seed = 0U;
  int steps = 0;
  for (int index = 1; index + 1 < argc; index += 2) {
    const std::string flag = argv[index];
    const std::string value = argv[index + 1];
    if (flag == "--seed") {
      seed = std::strtoull(value.c_str(), nullptr, 10);
    } else if (flag == "--steps") {
      steps = std::atoi(value.c_str());
    }
  }
  if (seed != 0U && steps > 0) {
    const std::filesystem::path path =
        std::filesystem::current_path() / ("re-machine-replay-" + std::to_string(seed));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    const int failures = testing::failure_count();
    const bool completed = run_machine(path, seed, steps);
    std::filesystem::remove_all(path, error);
    return completed && testing::failure_count() == failures ? 0 : 1;
  }
  return testing::run_all();
}
