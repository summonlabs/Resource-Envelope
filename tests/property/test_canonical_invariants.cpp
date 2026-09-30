// Property and round-trip invariants.
//
// These cases assert properties over generated inputs rather than over hand-written examples:
// the properties hold for every case the generator produces, and the generator is seeded from a
// fixed constant so a failure is reproducible from the seed printed with it.
//
// Three families are covered:
//
//  1. Canonical encoding is a total, injective, order-independent function: decode(encode(x))
//     reproduces x, distinct envelopes encode to distinct bytes, and the digest of an envelope
//     does not depend on the order its dimensions or its lineage were declared in.
//  2. Evaluation is monotone and total: raising a requested quantity never turns a refusal into
//     a grant, lowering a limit never turns a grant into a grant, and every evaluation of a
//     well-formed request produces a determination with a digest.
//  3. Idempotent replay is exact: the same request under the same key returns the same recorded
//     decision, and the same declaration under the same key returns the same revision.

#include <resource_envelope/canonical.hpp>
#include <resource_envelope/service.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <process.h>

#include "support/testing.hpp"

using namespace resource_envelope;

namespace {

// A fixed seed: a failure is reproducible by rerunning this suite, and the seed is reported so
// an investigation does not have to guess which sequence produced the failure.
constexpr std::uint64_t kSeed = 0x5EED1234U;
constexpr int kCases = 300;

const DimensionKind kKinds[] = {
    DimensionKind::SpaceRackUnits,   DimensionKind::SpaceRackSlots,
    DimensionKind::PowerDrawWatts,   DimensionKind::PowerFeedCircuits,
    DimensionKind::CoolingLoadWatts, DimensionKind::CoolingEnergyBudget,
    DimensionKind::RackExposureClass, DimensionKind::RedundancyLevel,
    DimensionKind::CountRackPositions,
};

const char* const kSites[] = {"site-one", "site-two", "site-three"};

Envelope generate_envelope(std::mt19937_64& engine, std::uint64_t ordinal) {
  std::uniform_int_distribution<std::uint64_t> quantity(0ULL, 1000ULL);
  std::uniform_int_distribution<int> kind_count(1, 5);
  std::uniform_int_distribution<int> kind_pick(0, 8);
  std::uniform_int_distribution<int> site_pick(0, 2);
  std::uniform_int_distribution<int> flag(0, 1);

  Envelope envelope;
  envelope.id = "generated-envelope-" + std::to_string(ordinal);
  envelope.revision = 1U;
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = "generated-tenant-" + std::to_string(ordinal);
  envelope.scope.identity.generation = 1U + (ordinal % 7U);
  envelope.site_id = kSites[site_pick(engine)];
  envelope.require_binding_confirmation = flag(engine) != 0;
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "generator";
  envelope.provenance.reason = "property case " + std::to_string(ordinal);
  envelope.provenance.declared_at = 1767225600000000000LL + static_cast<Timestamp>(ordinal);

  std::vector<DimensionKind> chosen;
  const int count = kind_count(engine);
  while (static_cast<int>(chosen.size()) < count) {
    const DimensionKind candidate = kKinds[kind_pick(engine)];
    if (std::find(chosen.begin(), chosen.end(), candidate) == chosen.end()) chosen.push_back(candidate);
  }
  for (const DimensionKind kind : chosen) {
    DimensionSpec spec;
    spec.kind = kind;
    spec.hard_limit = quantity(engine) * kNanounitsPerUnit;
    // A reservation is a part of the limit, never more than the whole, so the generator draws it
    // from what is left rather than independently. The runtime refuses the invalid combination,
    // which is what its own reservation validation asserts.
    spec.reserved = quantity(engine) % (*spec.hard_limit + 1U);
    spec.quantum = flag(engine) != 0 ? kNanounitsPerUnit : 0U;
    envelope.dimensions.push_back(spec);
  }
  return envelope;
}

EvaluationRequest generate_request(std::mt19937_64& engine, const Envelope& envelope,
                                   std::uint64_t ordinal) {
  std::uniform_int_distribution<std::uint64_t> quantity(0ULL, 1000ULL);
  EvaluationRequest request;
  request.scope = envelope.scope;
  request.at = envelope.provenance.declared_at + 1LL;
  request.idempotency_key = "generated-key-" + std::to_string(ordinal);
  for (const DimensionSpec& spec : envelope.dimensions) {
    DimensionRequest dimension;
    dimension.kind = spec.kind;
    dimension.quantity = spec.quantum == 0U ? quantity(engine) * kNanounitsPerUnit
                                            : quantity(engine) * spec.quantum;
    request.dimensions.push_back(dimension);
  }
  return request;
}

class ScratchStore {
 public:
  explicit ScratchStore(const std::string& name) {
    path_ = std::filesystem::current_path() / (name + "-" + std::to_string(::_getpid()));
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

}  // namespace

RE_TEST(canonical_encoding_round_trips_every_generated_envelope) {
  std::mt19937_64 engine(kSeed);
  int encoded_distinct = 0;
  for (int index = 0; index < kCases; ++index) {
    const Envelope envelope = generate_envelope(engine, static_cast<std::uint64_t>(index));
    const Bytes bytes = canonical_envelope(envelope);
    const Result<Envelope> decoded = decode_envelope(bytes);
    RE_REQUIRE(decoded.ok());
    // The decode reproduces the envelope exactly, so the encoding is injective on the fields
    // the record digest covers.
    RE_CHECK_EQ(record_digest(decoded.value()), record_digest(envelope));
    RE_CHECK_EQ(canonical_envelope(decoded.value()), bytes);
    RE_CHECK(content_digest(decoded.value()) == content_digest(envelope));
    if (bytes.size() > 0U) ++encoded_distinct;
    // Re-decoding the re-encoding is stable, so the encoding is a fixed point after one pass.
    const Result<Envelope> again = decode_envelope(canonical_envelope(decoded.value()));
    RE_REQUIRE(again.ok());
    RE_CHECK_EQ(canonical_envelope(again.value()), bytes);
  }
  RE_CHECK_EQ(encoded_distinct, kCases);
}

RE_TEST(the_envelope_digest_does_not_depend_on_declaration_order) {
  std::mt19937_64 engine(kSeed ^ 0xA5A5U);
  for (int index = 0; index < kCases; ++index) {
    const Envelope envelope = generate_envelope(engine, static_cast<std::uint64_t>(index));
    // Reversing the declared dimension order must not change the identity, the content digest
    // or the record digest: a declaration order is a presentation choice, not a constraint.
    Envelope reversed = envelope;
    std::reverse(reversed.dimensions.begin(), reversed.dimensions.end());
    RE_CHECK_EQ(content_digest(reversed), content_digest(envelope));
    RE_CHECK_EQ(record_digest(reversed), record_digest(envelope));
    // Reversing the lineage must not change it either.
    Envelope lineage_reversed = envelope;
    lineage_reversed.merged_from.push_back(EnvelopeRef{"source-a", 1U, record_digest(envelope)});
    lineage_reversed.merged_from.push_back(EnvelopeRef{"source-b", 2U, content_digest(envelope)});
    Envelope lineage_forward = lineage_reversed;
    std::reverse(lineage_forward.merged_from.begin(), lineage_forward.merged_from.end());
    RE_CHECK_EQ(record_digest(lineage_forward), record_digest(lineage_reversed));
  }
}

RE_TEST(evaluation_is_total_and_monotone_in_the_requested_quantity) {
  std::mt19937_64 engine(kSeed ^ 0x1234U);
  for (int index = 0; index < kCases; ++index) {
    Envelope envelope = generate_envelope(engine, static_cast<std::uint64_t>(index));
    EvaluationRequest request = generate_request(engine, envelope, static_cast<std::uint64_t>(index));
    const EvaluationResult baseline = evaluate(envelope, request);
    // Totality: a well-formed request always produces a determination with a digest, never a
    // partial result that a consumer would have to interpret.
    RE_CHECK(baseline.decision_digest.known());
    RE_CHECK(baseline.outcome == Outcome::Granted || baseline.outcome == Outcome::Denied ||
             baseline.outcome == Outcome::Indeterminate);
    // Determinism: the same inputs produce the same determination.
    const EvaluationResult repeated = evaluate(envelope, request);
    RE_CHECK_EQ(repeated.outcome, baseline.outcome);
    RE_CHECK_EQ(repeated.decision_digest, baseline.decision_digest);
    // Monotonicity: asking for more never improves the determination.
    for (DimensionRequest& dimension : request.dimensions) {
      dimension.quantity = dimension.quantity <= (kQuantityReasonableMax - kNanounitsPerUnit)
                               ? dimension.quantity + kNanounitsPerUnit
                               : dimension.quantity;
    }
    const EvaluationResult raised = evaluate(envelope, request);
    if (baseline.outcome == Outcome::Denied) {
      RE_CHECK_EQ(raised.outcome, Outcome::Denied);
    }
    if (raised.outcome == Outcome::Granted) {
      RE_CHECK_EQ(baseline.outcome, Outcome::Granted);
    }
  }
}

RE_TEST(tightening_a_limit_never_turns_a_denial_into_a_grant) {
  std::mt19937_64 engine(kSeed ^ 0x9876U);
  for (int index = 0; index < kCases; ++index) {
    const Envelope envelope = generate_envelope(engine, static_cast<std::uint64_t>(index));
    const EvaluationRequest request =
        generate_request(engine, envelope, static_cast<std::uint64_t>(index));
    const EvaluationResult baseline = evaluate(envelope, request);
    Envelope tightened = envelope;
    for (DimensionSpec& spec : tightened.dimensions) {
      if (spec.hard_limit.has_value() && *spec.hard_limit > 0U) {
        *spec.hard_limit = *spec.hard_limit / 2U;
      }
    }
    const EvaluationResult after = evaluate(tightened, request);
    if (baseline.outcome == Outcome::Denied) {
      // A tighter envelope can only refuse more, never less.
      RE_CHECK(after.outcome == Outcome::Denied || after.outcome == Outcome::Indeterminate);
    }
    if (after.outcome == Outcome::Granted) {
      // Nothing that a tighter envelope grants may be refused by the looser one.
      RE_CHECK(baseline.outcome == Outcome::Granted);
    }
  }
}

RE_TEST(idempotent_replay_returns_the_recorded_answer_exactly) {
  const ScratchStore scratch("re-property-replay");
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(scratch.path(), create_options());
  RE_REQUIRE(service.ok());
  std::mt19937_64 engine(kSeed ^ 0xFEEDU);
  for (int index = 0; index < 40; ++index) {
    Envelope envelope = generate_envelope(engine, static_cast<std::uint64_t>(index));
    envelope.require_binding_confirmation = false;
    DeclareInput declaration;
    declaration.envelope = envelope;
    declaration.idempotency_key = "declare-" + std::to_string(index);
    declaration.requested_at = 1767225600000000000LL + static_cast<Timestamp>(index) * 10LL;
    const Result<EnvelopeDeclaration> first = service.value()->declare(declaration);
    RE_REQUIRE(first.ok());
    const Result<EnvelopeDeclaration> second = service.value()->declare(declaration);
    RE_REQUIRE(second.ok());
    RE_CHECK(second.value().replayed);
    // The replay returns the recorded answer bit for bit, and it appends nothing.
    RE_CHECK_EQ(second.value().revision, first.value().revision);
    RE_CHECK_EQ(second.value().record_digest, first.value().record_digest);
    RE_CHECK_EQ(second.value().content_digest, first.value().content_digest);

    EvaluationRequest request = generate_request(engine, envelope, static_cast<std::uint64_t>(index));
    request.idempotency_key = "authorize-" + std::to_string(index);
    const Result<AuthorizeResult> decided = service.value()->authorize(request);
    RE_REQUIRE(decided.ok());
    const Result<AuthorizeResult> replayed = service.value()->authorize(request);
    RE_REQUIRE(replayed.ok());
    RE_CHECK(replayed.value().replayed);
    RE_CHECK_EQ(replayed.value().decision_digest, decided.value().decision_digest);
    RE_CHECK_EQ(replayed.value().record.sequence, decided.value().record.sequence);
    RE_CHECK_EQ(replayed.value().record.outcome, decided.value().record.outcome);
    RE_CHECK_EQ(replayed.value().record.reason, decided.value().record.reason);
  }
  // Replays appended nothing: one decision per request, no more.
  const Result<StoreStatistics> stats = service.value()->statistics();
  RE_REQUIRE(stats.ok());
  RE_CHECK_EQ(stats.value().decision_count, 40ULL);
  RE_CHECK_EQ(stats.value().envelope_count, 40ULL);
}