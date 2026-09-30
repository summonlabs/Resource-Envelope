#include "resource_envelope/service.hpp"

#include <algorithm>
#include <set>

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/text.hpp"

namespace resource_envelope {

namespace {

const EnvelopeRevisionRecord* current_revision_record(const EnvelopeEntry& entry) {
  for (const EnvelopeRevisionRecord& record : entry.revisions) {
    if (record.current) return &record;
  }
  return nullptr;
}

const EnvelopeRevisionRecord* find_revision_record(const EnvelopeEntry& entry, std::uint64_t revision) {
  for (const EnvelopeRevisionRecord& record : entry.revisions) {
    if (record.envelope.revision == revision) return &record;
  }
  return nullptr;
}

EnvelopeView make_view(const EnvelopeEntry& entry, const EnvelopeRevisionRecord& record) {
  EnvelopeView view;
  view.envelope = record.envelope;
  view.record_digest = record.record_digest;
  view.content_digest = record.content_digest;
  view.stored_sequence = record.stored_sequence;
  view.current = record.current;
  view.tombstoned = entry.tombstoned;
  return view;
}

// The exact claim a declaration or a revision makes. The provenance timestamp and the
// supersedes pointer are excluded, so the same declaration replayed later is the same
// claim, while any change to a constraint produces a different digest.
Digest declaration_payload_digest(const Envelope& envelope) {
  Envelope normalized = envelope;
  normalized.provenance.declared_at = 0;
  normalized.supersedes.reset();
  const Bytes body = canonical_envelope(normalized);
  return Digest(sha256_domain(kDomainIdempotency, std::span<const std::uint8_t>(body.data(), body.size())));
}

Digest usage_entries_digest(const std::vector<UsageDelta>& deltas) {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(deltas.size()));
  for (const UsageDelta& delta : deltas) writer.blob(canonical_usage_delta(delta));
  const Bytes body = writer.take();
  return Digest(sha256_domain(kDomainIdempotency, std::span<const std::uint8_t>(body.data(), body.size())));
}

Digest observation_entries_digest(const std::vector<ObservationEntry>& entries) {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(entries.size()));
  for (const ObservationEntry& entry : entries) writer.blob(canonical_observation_entry(entry));
  const Bytes body = writer.take();
  return Digest(sha256_domain(kDomainIdempotency, std::span<const std::uint8_t>(body.data(), body.size())));
}

bool is_idempotency_key(const std::string& key) {
  return !key.empty() && key.size() <= kIdempotencyKeyMaxLength;
}

std::uint64_t fnv1a64_id(const DecisionRecord& record) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  hash = fnv1a64_extend(hash, record.envelope_id);
  hash = fnv1a64_extend(hash, record.idempotency_key);
  hash = fnv1a64_extend(hash, record.request_digest.to_string());
  return hash;
}

Status validate_principal_token(const std::string& token, const char* what) {
  if (token.empty()) return Status{};
  if (validate_identifier(token, IdentifierUse::Canonical) != TextDefect::None) {
    return Status(StatusCode::InvalidIdentifier,
                  std::string("the ") + what + " is not a canonical identifier");
  }
  return Status{};
}

Status validate_envelope_shape(const Envelope& envelope) {
  if (validate_identifier(envelope.id, IdentifierUse::Canonical) != TextDefect::None) {
    return Status(StatusCode::InvalidIdentifier, "the envelope identifier is not a canonical identifier");
  }
  if (envelope.revision == 0U) {
    return Status(StatusCode::OutOfRange, "an envelope revision starts at one");
  }
  if (validate_identifier(envelope.scope.identity.id, IdentifierUse::Canonical) != TextDefect::None) {
    return Status(StatusCode::InvalidIdentifier, "the scope identity is not a canonical identifier");
  }
  if (!envelope.site_id.empty() &&
      validate_identifier(envelope.site_id, IdentifierUse::Canonical) != TextDefect::None) {
    return Status(StatusCode::InvalidIdentifier, "the site identifier is not a canonical identifier");
  }
  if (envelope.dimensions.empty()) {
    return Status(StatusCode::EmptyEnvelope, "an envelope must declare at least one dimension");
  }
  if (envelope.dimensions.size() > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "an envelope declares more dimensions than permitted");
  }
  if (envelope.precedence.size() > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "the precedence list exceeds the permitted length");
  }
  if (!is_valid_timestamp(envelope.provenance.declared_at)) {
    return Status(StatusCode::OutOfRange, "the declaration timestamp is outside the representable range");
  }
  Status status = validate_principal_token(envelope.provenance.authority, "declaring authority");
  if (!status.ok()) return status;
  status = validate_principal_token(envelope.provenance.actor, "declaring actor");
  if (!status.ok()) return status;
  if (envelope.provenance.reason.size() > kProvenanceMaxLength) {
    return Status(StatusCode::OutOfRange, "the declaration reason exceeds the permitted length");
  }

  if (envelope.window.effective_from.has_value() && !is_valid_timestamp(*envelope.window.effective_from)) {
    return Status(StatusCode::OutOfRange, "the effective window start is outside the representable range");
  }
  if (envelope.window.effective_until.has_value() && !is_valid_timestamp(*envelope.window.effective_until)) {
    return Status(StatusCode::OutOfRange, "the effective window end is outside the representable range");
  }
  if (envelope.window.effective_from.has_value() && envelope.window.effective_until.has_value() &&
      *envelope.window.effective_from >= *envelope.window.effective_until) {
    return Status(StatusCode::InvalidArgument,
                  "an effective window whose end is not strictly after its start never becomes authoritative");
  }

  std::set<DimensionKind> seen;
  for (const DimensionSpec& spec : envelope.dimensions) {
    if (!is_valid(spec.kind)) {
      return Status(StatusCode::InvalidEnumValue, "a declared dimension kind is not a defined value");
    }
    if (!seen.insert(spec.kind).second) {
      return Status(StatusCode::DuplicateDimension, "the envelope declares the same dimension twice");
    }
    if (spec.hard_limit.has_value() && *spec.hard_limit > kQuantityReasonableMax) {
      return Status(StatusCode::OutOfRange, "a declared limit exceeds the representable envelope magnitude");
    }
    if (spec.reserved > kQuantityReasonableMax) {
      return Status(StatusCode::OutOfRange, "a declared reservation exceeds the representable magnitude");
    }
    if (spec.quantum > kQuantityReasonableMax) {
      return Status(StatusCode::OutOfRange, "a declared alignment exceeds the representable magnitude");
    }
    if (spec.hard_limit.has_value() && spec.reserved > *spec.hard_limit) {
      return Status(StatusCode::InsufficientReserved,
                    "a declared reservation exceeds the limit it is reserved from");
    }
    if (!spec.compatibility_class.empty() &&
        validate_identifier(spec.compatibility_class, IdentifierUse::Canonical) != TextDefect::None) {
      return Status(StatusCode::InvalidIdentifier, "a declared compatibility class is not canonical");
    }
    if (requires_compatibility_class(spec.kind) && !spec.hard_limit.has_value()) {
      return Status(StatusCode::OpaqueLimit, "an exclusive dimension requires a declared capacity");
    }
    const DimensionUnit unit = unit_of(spec.kind);
    if (is_quantized_unit(unit)) {
      if (spec.hard_limit.has_value() && !is_multiple_of(*spec.hard_limit, base_increment(unit))) {
        return Status(StatusCode::OutOfRange, "a declared limit is not a whole multiple of its unit");
      }
      if (spec.quantum != 0U && !is_multiple_of(spec.quantum, base_increment(unit))) {
        return Status(StatusCode::OutOfRange, "a declared alignment is not a whole multiple of its unit");
      }
    }
  }
  for (const DimensionKind kind : envelope.precedence) {
    if (!is_valid(kind)) {
      return Status(StatusCode::InvalidEnumValue, "a precedence entry is not a defined dimension kind");
    }
  }
  for (const EnvelopeRef& reference : envelope.merged_from) {
    if (validate_identifier(reference.envelope_id, IdentifierUse::Canonical) != TextDefect::None) {
      return Status(StatusCode::InvalidIdentifier, "a lineage reference is not a canonical identifier");
    }
    if (reference.revision == 0U) {
      return Status(StatusCode::OutOfRange, "a lineage reference names revision zero");
    }
  }
  return Status{};
}

}  // namespace

// The service implementation holds no authority of its own: it loads state, decides,
// and publishes inside one call, so nothing survives a restart and stale authority
// has nowhere to live between calls.
class ServiceImpl {
 public:
  ServiceImpl(std::unique_ptr<Store> store, std::filesystem::path root)
      : store_(std::move(store)), root_(std::move(root)) {}

  ServiceImpl(const ServiceImpl&) = delete;
  ServiceImpl& operator=(const ServiceImpl&) = delete;

  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return store_->recovery(); }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  [[nodiscard]] Result<EnvelopeDeclaration> declare(const DeclareInput& input);
  [[nodiscard]] Result<EnvelopeRevision> revise(const ReviseInput& input);
  [[nodiscard]] Result<TombstoneResult> tombstone(const TombstoneInput& input);
  [[nodiscard]] Result<AuthorizeResult> authorize(const EvaluationRequest& request);
  [[nodiscard]] Result<UsageResult> record_usage(const UsageInput& input);
  [[nodiscard]] Result<ObservationResult> record_observation(const ObservationInput& input);

  [[nodiscard]] Result<EnvelopeView> get_envelope(const std::string& envelope_id) const;
  [[nodiscard]] Result<EnvelopeView> get_envelope_revision(const std::string& envelope_id,
                                                          std::uint64_t revision) const;
  [[nodiscard]] Result<std::vector<EnvelopeView>> list_envelopes() const;
  [[nodiscard]] Result<EvaluationResult> evaluate(const EvaluationRequest& request) const;
  [[nodiscard]] Result<std::vector<EnvelopeView>> find_by_scope(const EnvelopeScope& scope) const;
  [[nodiscard]] Result<std::vector<HistoryEntry>> history(const std::string& envelope_id) const;
  [[nodiscard]] Result<DecisionRecord> get_decision(const std::string& decision_id) const;
  [[nodiscard]] Result<std::vector<DecisionRecord>> list_decisions(const std::string& envelope_id,
                                                                  std::uint64_t limit) const;
  [[nodiscard]] Result<VerifyResult> verify_authority(const VerifyInput& input) const;
  [[nodiscard]] Result<StoreStatistics> statistics() const;
  [[nodiscard]] Result<CommittedMap> committed_usage(const std::string& envelope_id) const;
  [[nodiscard]] Result<std::vector<ObservationEntry>> observations(const std::string& envelope_id) const;
  [[nodiscard]] Result<DecisionRecord> find_decision_by_key(const std::string& envelope_id,
                                                           const std::string& idempotency_key) const;
  [[nodiscard]] Status compact();

  // Resolves the envelope and, when the request composes conjunctively, the facility
  // envelope. Both pointers reference the caller's live state and are valid only for
  // the duration of the call.
  [[nodiscard]] Status resolve_targets(const StoreState& state, const EvaluationRequest& request,
                                      const Envelope*& envelope, bool& is_current,
                                      const Envelope*& facility, std::uint64_t& stored_sequence) const;

  [[nodiscard]] EvaluationResult run_evaluation(const StoreState& state, const Envelope& envelope,
                                              bool is_current, std::uint64_t stored_sequence,
                                              const Envelope* facility, std::uint64_t control_epoch,
                                              const Digest& authority_digest, std::uint64_t decision_sequence,
                                              const EvaluationRequest& request) const;

  [[nodiscard]] DecisionRecord build_record(const EvaluationResult& evaluation,
                                           const EvaluationRequest& request, const Envelope& envelope,
                                           std::uint64_t control_epoch, const Digest& authority_digest,
                                           std::uint64_t decision_sequence) const;

 private:
  std::unique_ptr<Store> store_;
  std::filesystem::path root_;
};

namespace {

// Resolves the committed usage map only when the envelope actually has recorded
// commitments, so an envelope with none is never given an empty map that could be
// mistaken for "nothing is committed".
const CommittedMap* committed_for(const StoreState& state, const std::string& envelope_id,
                                  CommittedMap& storage) {
  const auto lower = state.committed.lower_bound(
      CommittedKey{envelope_id, DimensionKind::SpaceRackUnits, std::string(), std::string()});
  if (lower == state.committed.end() || lower->first.envelope_id != envelope_id) return nullptr;
  storage.clear();
  for (auto iterator = lower; iterator != state.committed.end(); ++iterator) {
    if (iterator->first.envelope_id != envelope_id) break;
    storage.insert(*iterator);
  }
  return &storage;
}

// Locates a usage claim already recorded under an idempotency key. Both the durable
// journal and, after a compaction, the committed aggregates are consulted so a
// replay is recognised even when the original record predates the last snapshot.
const UsageDelta* find_recorded_delta(const StoreState& state, const std::string& envelope_id,
                                      const std::string& idempotency_key, const Digest& payload) {
  for (const JournalEntry& entry : state.journal) {
    if (entry.kind != JournalEntryKind::UsageCommit || !entry.usage.has_value()) continue;
    if (entry.usage->idempotency_key != idempotency_key) continue;
    if (entry.usage->envelope_id != envelope_id) continue;
    if (entry.usage->payload_digest != payload) return nullptr;
    return &*entry.usage;
  }
  return nullptr;
}

bool usage_key_recorded(const StoreState& state, const std::string& idempotency_key) {
  for (const JournalEntry& entry : state.journal) {
    if (entry.kind == JournalEntryKind::UsageCommit && entry.usage.has_value() &&
        entry.usage->idempotency_key == idempotency_key) {
      return true;
    }
  }
  return false;
}

const DecisionRecord* latest_decision_for(const StoreState& state, const std::string& envelope_id,
                                         const std::string& idempotency_key, const Digest& request_digest,
                                         StatusCode& conflict) {
  for (auto iterator = state.decisions.rbegin(); iterator != state.decisions.rend(); ++iterator) {
    if (iterator->envelope_id != envelope_id) continue;
    if (iterator->idempotency_key != idempotency_key) continue;
    if (iterator->request_digest != request_digest) {
      conflict = StatusCode::IdempotencyConflict;
      return nullptr;
    }
    return &*iterator;
  }
  return nullptr;
}

}  // namespace

// A declaration that replays an existing claim is resolved before any freshness check,
// which is what makes a lost response safe to retry. A declaration that conflicts with
// an existing envelope of the same identifier is refused rather than merged.
Result<EnvelopeDeclaration> ServiceImpl::declare(const DeclareInput& input) {
  Envelope envelope = input.envelope;
  envelope.revision = 1U;
  if (input.idempotency_key.empty() || input.idempotency_key.size() > kIdempotencyKeyMaxLength) {
    return Status(StatusCode::InvalidArgument,
                  "a declaration requires an idempotency key of at most the permitted length");
  }
  const Status shape = validate_envelope_shape(envelope);
  if (!shape.ok()) return shape;
  if (!is_valid_timestamp(input.requested_at)) {
    return Status(StatusCode::OutOfRange, "the request timestamp is outside the representable range");
  }
  const Digest payload = declaration_payload_digest(envelope);
  std::optional<EnvelopeDeclaration> result;
  Status failure;
  const Status status = store_->with_writer([&](JournalSession& session) {
    const StoreState& state = session.state();
    const auto found = state.envelopes.find(envelope.id);
    if (found != state.envelopes.end() && !found->second.revisions.empty()) {
      const EnvelopeRevisionRecord* current = current_revision_record(found->second);
      if (current != nullptr && declaration_payload_digest(current->envelope) == payload) {
        EnvelopeDeclaration replayed;
        replayed.envelope_id = current->envelope.id;
        replayed.revision = current->envelope.revision;
        replayed.record_digest = current->record_digest;
        replayed.content_digest = current->content_digest;
        replayed.control_epoch = store_->control_epoch();
        replayed.replayed = true;
        result = replayed;
        return Status{};
      }
      failure = Status(StatusCode::StoreExists,
                       "an envelope with this identifier already exists and differs from this declaration");
      return failure;
    }
    EnvelopeDeclarationEntry entry;
    entry.envelope = envelope;
    entry.envelope.provenance.declared_at = input.requested_at;
    entry.idempotency_key = input.idempotency_key;
    entry.payload_digest = payload;
    entry.declared_at = input.requested_at;
    session.stage_envelope_declaration(std::move(entry));
    if (!session.failure().ok()) {
      failure = session.failure();
      return failure;
    }
    const auto staged = session.state().envelopes.find(envelope.id);
    if (staged == session.state().envelopes.end()) {
      failure = Status(StatusCode::StoreCorrupt, "the staged declaration was not applied to the state");
      return failure;
    }
    const EnvelopeRevisionRecord* record = current_revision_record(staged->second);
    if (record == nullptr) {
      failure = Status(StatusCode::StoreCorrupt, "the staged declaration produced no current revision");
      return failure;
    }
    EnvelopeDeclaration declared;
    declared.envelope_id = record->envelope.id;
    declared.revision = record->envelope.revision;
    declared.record_digest = record->record_digest;
    declared.content_digest = record->content_digest;
    declared.control_epoch = store_->control_epoch() + 1U;
    declared.replayed = false;
    result = declared;
    return Status{};
  });
  if (!status.ok()) return status;
  if (!result.has_value()) {
    return failure.ok() ? Status(StatusCode::StoreCorrupt, "the declaration produced no result") : failure;
  }
  return *result;
}

Result<EnvelopeRevision> ServiceImpl::revise(const ReviseInput& input) {
  Envelope envelope = input.envelope;
  if (input.idempotency_key.empty() || input.idempotency_key.size() > kIdempotencyKeyMaxLength) {
    return Status(StatusCode::InvalidArgument,
                  "a revision requires an idempotency key of at most the permitted length");
  }
  if (!is_valid_timestamp(input.requested_at)) {
    return Status(StatusCode::OutOfRange, "the request timestamp is outside the representable range");
  }
  if (input.expected_current_revision.has_value() && *input.expected_current_revision == 0U) {
    return Status(StatusCode::OutOfRange, "expected current revision zero is not a valid revision");
  }
  std::optional<EnvelopeRevision> result;
  Status failure;
  const Status status = store_->with_writer([&](JournalSession& session) {
    const StoreState& state = session.state();
    const auto found = state.envelopes.find(envelope.id);
    if (found == state.envelopes.end()) {
      failure = Status(StatusCode::EnvelopeNotFound, "no envelope with this identifier exists");
      return failure;
    }
    if (found->second.tombstoned) {
      failure = Status(StatusCode::EnvelopeNotFound, "the envelope has been retired and cannot be revised");
      return failure;
    }
    const EnvelopeRevisionRecord* current = current_revision_record(found->second);
    if (current == nullptr) {
      failure = Status(StatusCode::EnvelopeNotCurrent, "the envelope has no current revision");
      return failure;
    }
    // The previous revision is copied before anything is staged. Staging appends to the
    // revision vector, which can reallocate it and invalidate a pointer into it.
    const std::uint64_t previous_revision = current->envelope.revision;
    const Digest previous_digest = current->record_digest;
    Envelope candidate = envelope;
    candidate.revision = previous_revision + 1U;
    const Digest requested_payload = declaration_payload_digest(candidate);
    if (find_revision_record(found->second, candidate.revision) != nullptr) {
      const EnvelopeRevisionRecord* existing = find_revision_record(found->second, candidate.revision);
      if (declaration_payload_digest(existing->envelope) == requested_payload) {
        EnvelopeRevision replayed;
        replayed.envelope_id = envelope.id;
        replayed.previous_revision = previous_revision;
        replayed.revision = existing->envelope.revision;
        replayed.record_digest = existing->record_digest;
        replayed.content_digest = existing->content_digest;
        replayed.control_epoch = store_->control_epoch();
        replayed.replayed = true;
        result = replayed;
        return Status{};
      }
      failure = Status(StatusCode::IdempotencyConflict,
                        "a revision with the requested number already exists and differs from this request");
      return failure;
    }
    if (input.expected_current_revision.has_value() && *input.expected_current_revision != previous_revision) {
      failure = Status(StatusCode::StaleRevision,
                        "the expected current revision is not the current revision");
      return failure;
    }
    if (input.expected_current_digest.has_value() && *input.expected_current_digest != previous_digest) {
      failure = Status(StatusCode::StaleRevision, "the expected current digest is not the current digest");
      return failure;
    }
    candidate.supersedes = envelope.id;
    candidate.provenance.declared_at = input.requested_at;
    const Status shape = validate_envelope_shape(candidate);
    if (!shape.ok()) {
      failure = shape;
      return failure;
    }
    EnvelopeRevisionEntry entry;
    entry.envelope = candidate;
    entry.idempotency_key = input.idempotency_key;
    entry.payload_digest = requested_payload;
    entry.revised_at = input.requested_at;
    session.stage_envelope_revision(std::move(entry));
    if (!session.failure().ok()) {
      failure = session.failure();
      return failure;
    }
    const EnvelopeRevisionRecord* staged = find_revision_record(found->second, candidate.revision);
    if (staged == nullptr) {
      failure = Status(StatusCode::StoreCorrupt, "the staged revision was not applied to the state");
      return failure;
    }
    EnvelopeRevision revised;
    revised.envelope_id = candidate.id;
    revised.previous_revision = previous_revision;
    revised.revision = staged->envelope.revision;
    revised.record_digest = staged->record_digest;
    revised.content_digest = staged->content_digest;
    revised.control_epoch = store_->control_epoch() + 1U;
    revised.replayed = false;
    result = revised;
    return Status{};
  });
  if (!status.ok()) return status;
  if (!result.has_value()) {
    return failure.ok() ? Status(StatusCode::StoreCorrupt, "the revision produced no result") : failure;
  }
  return *result;
}
Result<AuthorizeResult> ServiceImpl::authorize(const EvaluationRequest& request) {
  if (!is_idempotency_key(request.idempotency_key)) {
    return Status(StatusCode::InvalidArgument,
                  "an authorisation requires an idempotency key of at most the permitted length");
  }
  const Digest request_fingerprint = request_digest(request);
  std::optional<AuthorizeResult> result;

  const Status status = store_->with_writer([&](JournalSession& session) {
    const StoreState& state = session.state();
    const Envelope* envelope = nullptr;
    const Envelope* facility = nullptr;
    bool is_current = false;
    std::uint64_t stored_sequence = 0U;
    const Status resolved = resolve_targets(state, request, envelope, is_current, facility, stored_sequence);

    if (resolved.ok()) {
      // Replay resolution is attempted before the evaluation runs, so a lost response
      // is resolved from the recorded decision rather than re-decided against an
      // authority that may since have moved.
      StatusCode conflict = StatusCode::Ok;
      const DecisionRecord* existing =
          latest_decision_for(state, envelope->id, request.idempotency_key, request_fingerprint, conflict);
      if (conflict == StatusCode::IdempotencyConflict) {
        return Status(conflict, "this idempotency key is already recorded against a different request");
      }
      if (existing != nullptr) {
        AuthorizeResult replayed;
        replayed.record = *existing;
        replayed.decision_digest = existing->decision_digest;
        replayed.replayed = true;
        replayed.evaluation.outcome = existing->outcome;
        replayed.evaluation.reason = existing->reason;
        replayed.evaluation.stage = existing->stage;
        replayed.evaluation.blocking_dimension = existing->blocking_dimension;
        replayed.evaluation.decision_digest = existing->decision_digest;
        replayed.evaluation.decision_sequence = existing->sequence;
        replayed.evaluation.envelope_id = existing->envelope_id;
        replayed.evaluation.envelope_revision = existing->envelope_revision;
        replayed.evaluation.envelope_digest = existing->envelope_digest;
        replayed.evaluation.control_epoch = existing->control_epoch;
        replayed.evaluation.request_digest = existing->request_digest;
        replayed.evaluation.evidence_digest = existing->evidence_digest;
        replayed.evaluation.dimensions = existing->dimensions;
        replayed.evaluation.secondary = existing->secondary;
        result = replayed;
        return Status{};
      }
    }

    const std::uint64_t control_epoch = store_->control_epoch() + 1U;
    const Digest authority_digest = store_state_digest(state);
    const std::uint64_t decision_sequence = session.next_sequence();

    EvaluationResult evaluation;
    if (resolved.ok()) {
      evaluation = run_evaluation(state, *envelope, is_current, stored_sequence, facility, control_epoch,
                                   authority_digest, decision_sequence, request);
    } else {
      // The authority could not be resolved, so the evaluation reports exactly that.
      // No decision record is written, because a record that names no authority would
      // be a record nothing can later be fenced against.
      EvaluationContext context;
      context.control_epoch = control_epoch;
      context.authority_state_digest = authority_digest;
      context.decision_sequence = decision_sequence;
      evaluation = evaluate_with_context(context, request);
      if (evaluation.reason == StatusCode::Ok) {
        evaluation.outcome = Outcome::Denied;
        evaluation.reason = resolved.code();
        evaluation.stage = RefusalStage::EnvelopePresence;
      }
      AuthorizeResult unresolved;
      unresolved.evaluation = evaluation;
      unresolved.decision_digest = evaluation.decision_digest;
      unresolved.replayed = false;
      result = unresolved;
      return Status{};
    }

    DecisionRecord record = build_record(evaluation, request, *envelope, control_epoch, authority_digest,
                                         decision_sequence);
    session.stage_decision(record);
    const auto staged = state.decisions.empty() ? nullptr : &state.decisions.back();
    if (staged == nullptr) {
      return Status(StatusCode::StoreCorrupt, "the staged decision was not applied to the state");
    }
    AuthorizeResult authorized;
    authorized.evaluation = evaluation;
    authorized.evaluation.decision_digest = staged->decision_digest;
    authorized.evaluation.decision_sequence = staged->sequence;
    authorized.record = *staged;
    authorized.decision_digest = staged->decision_digest;
    authorized.replayed = false;
    result = authorized;
    return Status(StatusCode::Ok);
  });
  if (!status.ok()) return status;
  if (!result.has_value()) return Status(StatusCode::StoreCorrupt, "the authorisation produced no result");
  return *result;
}

Result<ObservationResult> ServiceImpl::record_observation(const ObservationInput& input) {
  if (!is_idempotency_key(input.idempotency_key)) {
    return Status(StatusCode::InvalidArgument,
                  "recording an observation requires an idempotency key of at most the permitted length");
  }
  if (input.entries.empty()) {
    return Status(StatusCode::InvalidArgument, "no observations were supplied");
  }
  if (input.entries.size() > kMaxDeltasPerCall) {
    return Status(StatusCode::OutOfRange, "more observations were supplied than a single call permits");
  }
  for (const ObservationEntry& entry : input.entries) {
    if (!is_valid(entry.kind)) {
      return Status(StatusCode::InvalidEnumValue, "an observation names an undefined dimension");
    }
    if (entry.value > kQuantityReasonableMax) {
      return Status(StatusCode::OutOfRange, "an observed value exceeds the representable magnitude");
    }
    if (!is_valid_timestamp(entry.observed_at)) {
      return Status(StatusCode::OutOfRange, "an observation timestamp is outside the representable range");
    }
    const Status principal = validate_principal_token(entry.principal, "observation principal");
    if (!principal.ok()) return principal;
  }

  std::optional<ObservationResult> result;
  const Status status = store_->with_writer([&](JournalSession& session) {
    const StoreState& state = session.state();
    const auto found = state.envelopes.find(input.envelope_id);
    if (found == state.envelopes.end() || found->second.tombstoned) {
      return Status(StatusCode::EnvelopeNotFound, "no envelope with this identifier is current");
    }
    const EnvelopeRevisionRecord* current = current_revision_record(found->second);
    if (current == nullptr) {
      return Status(StatusCode::EnvelopeNotCurrent, "the envelope has no current revision");
    }
    if (input.expected_envelope_revision.has_value() &&
        *input.expected_envelope_revision != current->envelope.revision) {
      return Status(StatusCode::StaleRevision, "the expected envelope revision is not the current revision");
    }

    std::vector<ObservationEntry> staged;
    std::uint64_t replayed = 0U;
    for (const ObservationEntry& original : input.entries) {
      ObservationEntry entry = original;
      entry.envelope_id = input.envelope_id;
      entry.envelope_revision = current->envelope.revision;
      if (entry.observed_at == 0) entry.observed_at = original.observed_at;
      const Digest payload = Digest(sha256_domain(
          kDomainIdempotency, canonical_observation_entry(entry)));
      bool already = false;
      for (auto iterator = state.observations.begin(); iterator != state.observations.end(); ++iterator) {
        const Digest existing_payload = Digest(sha256_domain(
            kDomainIdempotency, canonical_observation_entry(iterator->second)));
        if (existing_payload == payload) {
          already = true;
          break;
        }
      }
      if (already) {
        ++replayed;
        staged.push_back(entry);
        continue;
      }
      entry.sequence = session.next_sequence();
      entry.entry_id = "observation-" + to_hex(entry.sequence);
      session.stage_observation(entry);
      staged.push_back(entry);
    }

    ObservationResult recorded;
    recorded.envelope_id = input.envelope_id;
    recorded.stored = staged.size() - replayed;
    recorded.replayed = replayed;
    recorded.entries_digest = observation_entries_digest(staged);
    recorded.last_sequence = session.next_sequence() - 1U;
    result = recorded;
    return Status{};
  });
  if (!status.ok()) return status;
  if (!result.has_value()) return Status(StatusCode::StoreCorrupt, "recording an observation produced no result");
  return *result;
}

Result<TombstoneResult> ServiceImpl::tombstone(const TombstoneInput& input) {
  if (input.request_id == 0U) {
    return Status(StatusCode::InvalidArgument, "retiring an envelope requires a non-zero request identifier");
  }
  if (input.reason.size() > kLabelMaxLength) {
    return Status(StatusCode::OutOfRange, "the retirement reason exceeds the permitted length");
  }
  std::optional<TombstoneResult> result;
  const Status status = store_->with_writer([&](JournalSession& session) {
    const StoreState& state = session.state();
    const auto found = state.envelopes.find(input.envelope_id);
    if (found == state.envelopes.end()) {
      return Status(StatusCode::EnvelopeNotFound, "no envelope with this identifier exists");
    }
    const EnvelopeRevisionRecord* current = current_revision_record(found->second);
    if (found->second.tombstoned) {
      if (current == nullptr) {
        return Status(StatusCode::EnvelopeNotFound, "the envelope is already retired");
      }
    }

    // Replay resolution: the same retirement request is resolved without advancing the
    // epoch, so a retried retirement is idempotent.
    const auto replay_retirement = [&](const EnvelopeTombstoneEntry& existing) {
      TombstoneResult replayed;
      replayed.envelope_id = existing.envelope_id;
      replayed.superseded_revision = existing.superseded_revision;
      replayed.reason = existing.reason;
      replayed.request_id = input.request_id;
      replayed.last_record_digest = existing.last_record_digest;
      replayed.replayed = true;
      result = replayed;
      return Status{};
    };
    for (const EnvelopeTombstoneEntry& existing : state.tombstones) {
      if (existing.envelope_id != input.envelope_id) continue;
      if (existing.reason != input.reason) {
        return Status(StatusCode::IdempotencyConflict,
                      "this envelope was already retired with a different reason");
      }
      return replay_retirement(existing);
    }
    for (const JournalEntry& entry : state.journal) {
      if (entry.kind != JournalEntryKind::EnvelopeTombstone || !entry.tombstone.has_value()) continue;
      if (entry.tombstone->envelope_id != input.envelope_id) continue;
      if (entry.tombstone->reason != input.reason) {
        return Status(StatusCode::IdempotencyConflict,
                      "this envelope was already retired with a different reason");
      }
      return replay_retirement(*entry.tombstone);
    }
    if (found->second.tombstoned) {
      return Status(StatusCode::EnvelopeNotFound, "the envelope is already retired");
    }
    if (current == nullptr) {
      return Status(StatusCode::EnvelopeNotCurrent, "the envelope has no current revision");
    }
    if (input.expected_current_revision.has_value() &&
        *input.expected_current_revision != current->envelope.revision) {
      return Status(StatusCode::StaleRevision, "the expected revision is not the current revision");
    }
    EnvelopeTombstoneEntry entry;
    entry.envelope_id = input.envelope_id;
    entry.superseded_revision = current->envelope.revision;
    entry.last_record_digest = current->record_digest;
    entry.reason = input.reason;
    entry.tombstoned_at = input.requested_at;
    session.stage_envelope_tombstone(std::move(entry));
    TombstoneResult retired;
    retired.envelope_id = input.envelope_id;
    retired.superseded_revision = current->envelope.revision;
    retired.reason = input.reason;
    retired.request_id = input.request_id;
    retired.last_record_digest = current->record_digest;
    retired.replayed = false;
    result = retired;
    return Status(StatusCode::Ok);
  });
  if (!status.ok()) return status;
  if (!result.has_value()) return Status(StatusCode::StoreCorrupt, "retirement produced no result");
  return *result;
}

Result<UsageResult> ServiceImpl::record_usage(const UsageInput& input) {
  if (!is_idempotency_key(input.idempotency_key)) {
    return Status(StatusCode::InvalidArgument,
                  "recording usage requires an idempotency key of at most the permitted length");
  }
  if (input.deltas.empty()) {
    return Status(StatusCode::InvalidArgument, "no usage records were supplied");
  }
  if (input.deltas.size() > kMaxDeltasPerCall) {
    return Status(StatusCode::OutOfRange, "more usage records were supplied than a single call permits");
  }
  if (!is_valid_timestamp(input.requested_at)) {
    return Status(StatusCode::OutOfRange, "the request timestamp is outside the representable range");
  }
  for (const UsageDelta& delta : input.deltas) {
    if (delta.envelope_id != input.envelope_id) {
      return Status(StatusCode::InvalidArgument, "a usage record names an envelope other than the request's");
    }
    if (!is_valid(delta.kind)) {
      return Status(StatusCode::InvalidEnumValue, "a usage record names an undefined dimension");
    }
    const Status principal = validate_principal_token(delta.principal, "usage principal");
    if (!principal.ok()) return principal;
    if (!delta.compatibility_class.empty() &&
        validate_identifier(delta.compatibility_class, IdentifierUse::Canonical) != TextDefect::None) {
      return Status(StatusCode::InvalidIdentifier, "a usage compatibility class is not canonical");
    }
    if (cardinality_of(delta.kind) != Cardinality::Consumable) {
      // Only consumable dimensions accumulate. An exclusive or threshold dimension is
      // declared through evidence, not through a running total.
      return Status(StatusCode::InvalidArgument,
                    "committed usage is only defined for consumable dimensions");
    }
  }

  std::optional<UsageResult> result;
  const Status status = store_->with_writer([&](JournalSession& session) {
    const StoreState& state = session.state();
    const auto found = state.envelopes.find(input.envelope_id);
    if (found == state.envelopes.end() || found->second.tombstoned) {
      return Status(StatusCode::EnvelopeNotFound, "no envelope with this identifier is current");
    }
    const EnvelopeRevisionRecord* current = current_revision_record(found->second);
    if (current == nullptr) {
      return Status(StatusCode::EnvelopeNotCurrent, "the envelope has no current revision");
    }

    // Replay resolution first: a lost response must not double-count a delta.
    std::vector<UsageDelta> appended;
    std::vector<UsageDelta> replayed;
    for (const UsageDelta& original : input.deltas) {
      UsageDelta delta = original;
      delta.envelope_revision = current->envelope.revision;
      delta.recorded_at = input.requested_at;
      const Digest payload = usage_payload_digest(delta);
      const UsageDelta* recorded = find_recorded_delta(state, input.envelope_id, input.idempotency_key, payload);
      if (recorded != nullptr) {
        replayed.push_back(*recorded);
        continue;
      }
      if (usage_key_recorded(state, input.idempotency_key)) {
        return Status(StatusCode::IdempotencyConflict,
                      "this idempotency key is already recorded against a different usage claim");
      }
      delta.idempotency_key = input.idempotency_key;
      delta.payload_digest = payload;
      appended.push_back(std::move(delta));
    }

    if (!appended.empty() && input.expected_envelope_revision.has_value() &&
        *input.expected_envelope_revision != current->envelope.revision) {
      return Status(StatusCode::StaleRevision, "the expected envelope revision is not the current revision");
    }

    const std::uint64_t first_sequence = appended.empty() ? 0U : session.next_sequence();
    for (UsageDelta& delta : appended) {
      delta.sequence = session.next_sequence();
      delta.entry_id = "usage-" + to_hex(delta.sequence);
      session.stage_usage_commit(delta);
    }

    UsageResult recorded;
    recorded.envelope_id = input.envelope_id;
    recorded.envelope_revision = current->envelope.revision;
    recorded.entries_appended = appended.size();
    recorded.entries_replayed = replayed.size();
    recorded.entries_digest = usage_entries_digest(appended);
    recorded.first_sequence = first_sequence;
    recorded.last_sequence = session.next_sequence() - 1U;
    recorded.control_epoch = store_->control_epoch() + (appended.empty() ? 0U : 1U);
    result = recorded;
    return Status{};
  });
  if (!status.ok()) return status;
  if (!result.has_value()) return Status(StatusCode::StoreCorrupt, "recording usage produced no result");
  return *result;
}

Result<DecisionRecord> ServiceImpl::get_decision(const std::string& decision_id) const {
  std::optional<DecisionRecord> record;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (const DecisionRecord& candidate : state.decisions) {
      if (candidate.decision_id == decision_id) {
        record = candidate;
        return Status{};
      }
    }
    return Status(StatusCode::EnvelopeNotFound, "no decision with this identifier is recorded");
  });
  if (!status.ok()) return status;
  if (!record.has_value()) return Status(StatusCode::StoreCorrupt, "the lookup produced no record");
  return *record;
}

Result<std::vector<DecisionRecord>> ServiceImpl::list_decisions(const std::string& envelope_id,
                                                                std::uint64_t limit) const {
  if (limit == 0U || limit > kMaxDecisionCount) {
    return Status(StatusCode::OutOfRange, "the requested decision limit is outside the permitted range");
  }
  std::vector<DecisionRecord> records;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (auto iterator = state.decisions.rbegin(); iterator != state.decisions.rend(); ++iterator) {
      if (!envelope_id.empty() && iterator->envelope_id != envelope_id) continue;
      if (records.size() >= limit) break;
      records.push_back(*iterator);
    }
    return Status{};
  });
  if (!status.ok()) return status;
  return records;
}

Result<DecisionRecord> ServiceImpl::find_decision_by_key(const std::string& envelope_id,
                                                         const std::string& idempotency_key) const {
  std::optional<DecisionRecord> record;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (auto iterator = state.decisions.rbegin(); iterator != state.decisions.rend(); ++iterator) {
      if (iterator->envelope_id != envelope_id) continue;
      if (iterator->idempotency_key != idempotency_key) continue;
      record = *iterator;
      return Status{};
    }
    return Status(StatusCode::EnvelopeNotFound, "no decision is recorded under this idempotency key");
  });
  if (!status.ok()) return status;
  if (!record.has_value()) return Status(StatusCode::StoreCorrupt, "the lookup produced no record");
  return *record;
}

Result<CommittedMap> ServiceImpl::committed_usage(const std::string& envelope_id) const {
  CommittedMap committed;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (const auto& pair : state.committed) {
      if (pair.first.envelope_id == envelope_id) committed.insert(pair);
    }
    return Status{};
  });
  if (!status.ok()) return status;
  return committed;
}

Result<std::vector<ObservationEntry>> ServiceImpl::observations(const std::string& envelope_id) const {
  std::vector<ObservationEntry> entries;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (const auto& pair : state.observations) {
      if (pair.second.envelope_id == envelope_id) entries.push_back(pair.second);
    }
    return Status{};
  });
  if (!status.ok()) return status;
  return entries;
}

Result<VerifyResult> ServiceImpl::verify_authority(const VerifyInput& input) const {
  if (!input.decision_id.has_value() && !input.expected_envelope_digest.has_value()) {
    return Status(StatusCode::InvalidArgument,
                  "verification requires either a decision identifier or an expected envelope digest");
  }
  std::optional<VerifyResult> outcome;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    DecisionRecord record;
    if (input.decision_id.has_value()) {
      bool found = false;
      for (const DecisionRecord& candidate : state.decisions) {
        if (candidate.decision_id == *input.decision_id) {
          record = candidate;
          found = true;
          break;
        }
      }
      if (!found) {
        return Status(StatusCode::EnvelopeNotFound, "no decision with this identifier is recorded");
      }
    }

    AuthorityVerdict verdict;
    verdict.envelope_id = record.envelope_id;
    verdict.granted_revision = record.envelope_revision;
    verdict.granted_control_epoch = record.control_epoch;
    verdict.current_control_epoch = store_->control_epoch();

    if (record.outcome != Outcome::Granted) {
      verdict.state = AuthorityState::NotAGrant;
      verdict.detail = "the recorded decision granted nothing, so there is no authority to verify";
    } else if (record.control_epoch != store_->control_epoch()) {
      // The record relies on a control epoch the store has since retired. This is the
      // fencing case: the authority was replaced, not merely revised.
      verdict.state = AuthorityState::ControlEpochFenced;
      verdict.detail = "the decision was granted under a control epoch that has been superseded";
    } else {
      const auto found = state.envelopes.find(record.envelope_id);
      if (found == state.envelopes.end() || found->second.tombstoned) {
        verdict.state = AuthorityState::EnvelopeMissing;
        verdict.detail = "the envelope the decision relied on is no longer present";
      } else {
        const EnvelopeRevisionRecord* current = current_revision_record(found->second);
        if (current == nullptr) {
          verdict.state = AuthorityState::EnvelopeMissing;
          verdict.detail = "the envelope has no current revision";
        } else {
          verdict.current_revision = current->envelope.revision;
          if (record.envelope_revision != current->envelope.revision) {
            verdict.state = AuthorityState::EnvelopeRevisionAdvanced;
            verdict.detail = "the envelope has advanced past the revision the decision relied on";
          } else if (record.envelope_digest != current->record_digest) {
            verdict.state = AuthorityState::EnvelopeDigestChanged;
            verdict.detail = "the envelope revision carries a different digest than the decision relied on";
          } else if (input.at != 0 && current->envelope.window.ended_at(input.at)) {
            verdict.state = AuthorityState::EnvelopeExpired;
            verdict.detail = "the envelope effective window has ended";
          } else if (input.identity.has_value() &&
                     input.identity->identity != current->envelope.scope.identity) {
            verdict.state = AuthorityState::IdentityBindingChanged;
            verdict.detail = "the supplied identity generation is not the generation the envelope is bound to";
          } else if ((input.service_class.has_value() && input.service_class->state == ResolutionState::Provided &&
                      current->envelope.service_class_digest.known() &&
                      input.service_class->digest != current->envelope.service_class_digest) ||
                     (input.policy.has_value() && input.policy->state == ResolutionState::Provided &&
                      current->envelope.policy_digest.known() &&
                      input.policy->digest != current->envelope.policy_digest)) {
            verdict.state = AuthorityState::ContextBindingChanged;
            verdict.detail = "a referenced context digest no longer matches the envelope binding";
          }
        }
      }
    }

    VerifyResult verified;
    verified.verdict = verdict;
    verified.record = record;
    outcome = verified;
    return Status{};
  });
  if (!status.ok()) return status;
  if (!outcome.has_value()) return Status(StatusCode::StoreCorrupt, "verification produced no verdict");
  return *outcome;
}

Result<EnvelopeView> ServiceImpl::get_envelope(const std::string& envelope_id) const {
  std::optional<EnvelopeView> view;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    const auto found = state.envelopes.find(envelope_id);
    if (found == state.envelopes.end()) {
      return Status(StatusCode::EnvelopeNotFound, "no envelope with this identifier exists");
    }
    const EnvelopeRevisionRecord* current = current_revision_record(found->second);
    if (current == nullptr) {
      return Status(StatusCode::EnvelopeNotCurrent, "the envelope has no current revision");
    }
    view = make_view(found->second, *current);
    return Status{};
  });
  if (!status.ok()) return status;
  if (!view.has_value()) return Status(StatusCode::StoreCorrupt, "the lookup produced no view");
  return *view;
}

Result<EnvelopeView> ServiceImpl::get_envelope_revision(const std::string& envelope_id,
                                                        std::uint64_t revision) const {
  std::optional<EnvelopeView> view;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    const auto found = state.envelopes.find(envelope_id);
    if (found == state.envelopes.end()) {
      return Status(StatusCode::EnvelopeNotFound, "no envelope with this identifier exists");
    }
    const EnvelopeRevisionRecord* record = find_revision_record(found->second, revision);
    if (record == nullptr) {
      // Retention is bounded, so an old revision may legitimately be absent. The
      // diagnostic names that possibility rather than claiming the envelope is gone.
      return Status(StatusCode::EnvelopeNotFound,
                    "the requested revision is not retained; it may have been superseded and compacted away");
    }
    view = make_view(found->second, *record);
    return Status{};
  });
  if (!status.ok()) return status;
  if (!view.has_value()) return Status(StatusCode::StoreCorrupt, "the lookup produced no view");
  return *view;
}

Result<std::vector<EnvelopeView>> ServiceImpl::list_envelopes() const {
  std::vector<EnvelopeView> views;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (const auto& pair : state.envelopes) {
      const EnvelopeRevisionRecord* current = current_revision_record(pair.second);
      if (current == nullptr) continue;
      views.push_back(make_view(pair.second, *current));
    }
    return Status{};
  });
  if (!status.ok()) return status;
  return views;
}

Result<std::vector<EnvelopeView>> ServiceImpl::find_by_scope(const EnvelopeScope& scope) const {
  std::vector<EnvelopeView> views;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (const auto& pair : state.envelopes) {
      if (pair.second.tombstoned) continue;
      const EnvelopeRevisionRecord* current = current_revision_record(pair.second);
      if (current == nullptr) continue;
      if (!(current->envelope.scope == scope)) continue;
      views.push_back(make_view(pair.second, *current));
    }
    return Status{};
  });
  if (!status.ok()) return status;
  return views;
}

Result<std::vector<HistoryEntry>> ServiceImpl::history(const std::string& envelope_id) const {
  std::vector<HistoryEntry> entries;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    for (const JournalEntry& entry : state.journal) {
      HistoryEntry item;
      item.sequence = entry.sequence;
      switch (entry.kind) {
        case JournalEntryKind::EnvelopeDeclaration:
          if (!entry.declaration.has_value() || entry.declaration->envelope.id != envelope_id) continue;
          item.kind = "declaration";
          item.envelope_id = entry.declaration->envelope.id;
          item.revision = entry.declaration->envelope.revision;
          item.digest = record_digest(entry.declaration->envelope);
          item.detail = entry.declaration->envelope.provenance.authority;
          item.at = entry.declaration->declared_at;
          break;
        case JournalEntryKind::EnvelopeRevision:
          if (!entry.revision.has_value() || entry.revision->envelope.id != envelope_id) continue;
          item.kind = "revision";
          item.envelope_id = entry.revision->envelope.id;
          item.revision = entry.revision->envelope.revision;
          item.digest = record_digest(entry.revision->envelope);
          item.detail = entry.revision->envelope.provenance.authority;
          item.at = entry.revision->revised_at;
          break;
        case JournalEntryKind::EnvelopeTombstone:
          if (!entry.tombstone.has_value() || entry.tombstone->envelope_id != envelope_id) continue;
          item.kind = "retirement";
          item.envelope_id = entry.tombstone->envelope_id;
          item.revision = entry.tombstone->superseded_revision;
          item.digest = entry.tombstone->last_record_digest;
          item.detail = entry.tombstone->reason;
          item.at = entry.tombstone->tombstoned_at;
          break;
        case JournalEntryKind::UsageCommit:
          if (!entry.usage.has_value() || entry.usage->envelope_id != envelope_id) continue;
          item.kind = "usage";
          item.envelope_id = entry.usage->envelope_id;
          item.revision = entry.usage->envelope_revision;
          item.digest = entry.usage->payload_digest;
          item.detail = std::string(to_string(entry.usage->kind)) + " " +
                        (entry.usage->delta < 0 ? "-" : "+") +
                        format_quantity(static_cast<Nanounits>(entry.usage->delta < 0 ? -entry.usage->delta
                                                                                       : entry.usage->delta));
          item.at = entry.usage->recorded_at;
          break;
        case JournalEntryKind::Decision:
          if (!entry.decision.has_value() || entry.decision->envelope_id != envelope_id) continue;
          item.kind = "decision";
          item.envelope_id = entry.decision->envelope_id;
          item.revision = entry.decision->envelope_revision;
          item.digest = entry.decision->decision_digest;
          item.detail = std::string(to_string(entry.decision->outcome)) + "/" +
                        to_string(entry.decision->reason);
          item.at = entry.decision->evaluated_at;
          break;
        case JournalEntryKind::Observation:
          if (!entry.observation.has_value() || entry.observation->envelope_id != envelope_id) continue;
          item.kind = "observation";
          item.envelope_id = entry.observation->envelope_id;
          item.revision = entry.observation->envelope_revision;
          item.detail = std::string(to_string(entry.observation->kind)) + " " +
                        (entry.observation->status == MeasureStatus::Measured
                             ? format_quantity(entry.observation->value)
                             : std::string("unknown"));
          item.at = entry.observation->observed_at;
          break;
        case JournalEntryKind::Snapshot:
          continue;
      }
      entries.push_back(std::move(item));
    }
    return Status{};
  });
  if (!status.ok()) return status;
  return entries;
}

Result<EvaluationResult> ServiceImpl::evaluate(const EvaluationRequest& request) const {
  EvaluationResult outcome;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    const Envelope* envelope = nullptr;
    const Envelope* facility = nullptr;
    bool is_current = false;
    std::uint64_t stored_sequence = 0U;
    const Status resolved = resolve_targets(state, request, envelope, is_current, facility, stored_sequence);
    const Digest authority_digest = store_state_digest(state);
    const std::uint64_t control_epoch = store_->control_epoch();
    if (!resolved.ok()) {
      EvaluationContext context;
      context.control_epoch = control_epoch;
      context.authority_state_digest = authority_digest;
      outcome = evaluate_with_context(context, request);
      if (outcome.reason == StatusCode::Ok) {
        outcome.outcome = Outcome::Denied;
        outcome.reason = resolved.code();
        outcome.stage = RefusalStage::EnvelopePresence;
      }
      return Status{};
    }
    outcome = run_evaluation(state, *envelope, is_current, stored_sequence, facility, control_epoch,
                             authority_digest, 0U, request);
    return Status{};
  });
  if (!status.ok()) return status;
  return outcome;
}

Status ServiceImpl::resolve_targets(const StoreState& state, const EvaluationRequest& request,
                                   const Envelope*& envelope, bool& is_current,
                                   const Envelope*& facility, std::uint64_t& stored_sequence) const {
  envelope = nullptr;
  facility = nullptr;
  is_current = false;
  stored_sequence = 0U;

  // The envelope identifier is derived from the scope through the state index, so a
  // request can never name an envelope that does not belong to the scope it claims.
  const Envelope* primary = nullptr;
  std::uint64_t primary_sequence = 0U;
  bool primary_current = false;
  for (const auto& pair : state.envelopes) {
    if (pair.second.tombstoned) continue;
    const EnvelopeRevisionRecord* record = current_revision_record(pair.second);
    if (record == nullptr) continue;
    if (!(record->envelope.scope == request.scope)) continue;
    if (primary != nullptr) {
      return Status(StatusCode::DuplicateIdentity,
                    "more than one current envelope claims this scope; the authority is ambiguous");
    }
    primary = &record->envelope;
    primary_sequence = record->stored_sequence;
    primary_current = true;
  }
  if (primary == nullptr) return Status(StatusCode::EnvelopeNotFound, "no envelope matches the requested scope");
  envelope = primary;
  is_current = primary_current;
  stored_sequence = primary_sequence;

  if (request.facility_composition != FacilityComposition::ConjunctiveWithFacility) return Status{};
  if (request.facility_envelope_id.empty()) {
    return Status(StatusCode::InvalidArgument, "conjunctive composition requires a facility envelope identifier");
  }
  const auto found = state.envelopes.find(request.facility_envelope_id);
  if (found == state.envelopes.end() || found->second.tombstoned) {
    return Status(StatusCode::EnvelopeNotFound, "the named facility envelope does not exist");
  }
  const EnvelopeRevisionRecord* record = current_revision_record(found->second);
  if (record == nullptr) {
    return Status(StatusCode::EnvelopeNotCurrent, "the named facility envelope has no current revision");
  }
  if (record->envelope.scope.kind != EnvelopeScopeKind::Facility) {
    return Status(StatusCode::InvalidArgument, "the named envelope is not scoped to the facility");
  }
  facility = &record->envelope;
  return Status{};
}

EvaluationResult ServiceImpl::run_evaluation(const StoreState& state, const Envelope& envelope,
                                           bool is_current, std::uint64_t stored_sequence,
                                           const Envelope* facility, std::uint64_t control_epoch,
                                           const Digest& authority_digest, std::uint64_t decision_sequence,
                                           const EvaluationRequest& request) const {
  EvaluationContext context;
  context.envelope = &envelope;
  context.facility_envelope = facility;
  context.control_epoch = control_epoch;
  context.authority_state_digest = authority_digest;
  context.committed_envelope_id = envelope.id;
  CommittedMap storage;
  context.committed = committed_for(state, envelope.id, storage);
  context.envelope_is_current = is_current;
  context.envelope_stored_sequence = stored_sequence;
  context.decision_sequence = decision_sequence;
  return evaluate_with_context(context, request);
}

DecisionRecord ServiceImpl::build_record(const EvaluationResult& evaluation,
                                        const EvaluationRequest& request, const Envelope& envelope,
                                        std::uint64_t control_epoch, const Digest& authority_digest,
                                        std::uint64_t decision_sequence) const {
  DecisionRecord record;
  record.idempotency_key = request.idempotency_key;
  record.request_digest = evaluation.request_digest;
  record.evidence_digest = evaluation.evidence_digest;
  record.outcome = evaluation.outcome;
  record.reason = evaluation.reason;
  record.stage = evaluation.stage;
  record.blocking_dimension = evaluation.blocking_dimension;
  record.envelope_id = envelope.id;
  record.envelope_revision = envelope.revision;
  record.envelope_digest = evaluation.envelope_digest;
  record.control_epoch = control_epoch;
  record.authority_state_digest = authority_digest;
  record.scope = envelope.scope;
  record.identity = envelope.scope.identity;
  record.identity_checked = !request.identity.identity.id.empty();
  record.identity_digest = envelope.identity_digest;
  record.service_class_digest = envelope.service_class_digest;
  record.policy_digest = envelope.policy_digest;
  record.evaluated_at = request.at;
  record.dimensions = evaluation.dimensions;
  record.secondary = evaluation.secondary;
  record.sequence = decision_sequence;
  record.decision_id = "decision-" + to_hex(record.sequence) + "-" + to_hex(fnv1a64_id(record));
  if (record.outcome == Outcome::Granted) {
    // A grant is a distinct artefact from the decision that produced it, and it is
    // domain-separated so a grant digest can never collide with a decision digest.
    const Bytes body = canonical_decision_record(record);
    record.grant_digest = Digest(sha256_domain(kDomainGrant, std::span<const std::uint8_t>(body.data(), body.size())));
  }
  record.decision_digest = compute_decision_digest(record);
  return record;
}

// ---------------------------------------------------------------------------
// Public service surface
// ---------------------------------------------------------------------------
// Every method is a single critical section over the store. Nothing is cached between
// calls, so a restart has no live authority to restore and no stale authority can
// survive inside the process.

EnvelopeService::EnvelopeService(std::unique_ptr<ServiceImpl> impl) : impl_(std::move(impl)) {}

EnvelopeService::~EnvelopeService() = default;

Result<std::unique_ptr<EnvelopeService>> EnvelopeService::open(const std::filesystem::path& root,
                                                                const StoreOptions& options) {
  Result<std::unique_ptr<Store>> store = Store::open(root, options);
  if (!store.ok()) return store.status();
  auto impl = std::make_unique<ServiceImpl>(std::move(store.value()), root);
  return std::unique_ptr<EnvelopeService>(new EnvelopeService(std::move(impl)));
}

const RecoveryReport& EnvelopeService::recovery() const noexcept { return impl_->recovery(); }

const std::filesystem::path& EnvelopeService::root() const noexcept { return impl_->root(); }

Result<EnvelopeDeclaration> EnvelopeService::declare(const DeclareInput& input) {
  return impl_->declare(input);
}

Result<EnvelopeRevision> EnvelopeService::revise(const ReviseInput& input) { return impl_->revise(input); }

Result<TombstoneResult> EnvelopeService::tombstone(const TombstoneInput& input) {
  return impl_->tombstone(input);
}

Result<AuthorizeResult> EnvelopeService::authorize(const EvaluationRequest& request) {
  return impl_->authorize(request);
}

Result<UsageResult> EnvelopeService::record_usage(const UsageInput& input) {
  return impl_->record_usage(input);
}

Result<ObservationResult> EnvelopeService::record_observation(const ObservationInput& input) {
  return impl_->record_observation(input);
}

Result<EnvelopeView> EnvelopeService::get_envelope(const std::string& envelope_id) const {
  return impl_->get_envelope(envelope_id);
}

Result<EnvelopeView> EnvelopeService::get_envelope_revision(const std::string& envelope_id,
                                                            std::uint64_t revision) const {
  return impl_->get_envelope_revision(envelope_id, revision);
}

Result<std::vector<EnvelopeView>> EnvelopeService::list_envelopes() const {
  return impl_->list_envelopes();
}

Result<EvaluationResult> EnvelopeService::evaluate(const EvaluationRequest& request) const {
  return impl_->evaluate(request);
}

Result<std::vector<EnvelopeView>> EnvelopeService::find_by_scope(const EnvelopeScope& scope) const {
  return impl_->find_by_scope(scope);
}

Result<std::vector<HistoryEntry>> EnvelopeService::history(const std::string& envelope_id) const {
  return impl_->history(envelope_id);
}

Result<DecisionRecord> EnvelopeService::get_decision(const std::string& decision_id) const {
  return impl_->get_decision(decision_id);
}

Result<std::vector<DecisionRecord>> EnvelopeService::list_decisions(const std::string& envelope_id,
                                                                    std::uint64_t limit) const {
  return impl_->list_decisions(envelope_id, limit);
}

Result<VerifyResult> EnvelopeService::verify_authority(const VerifyInput& input) const {
  return impl_->verify_authority(input);
}

Result<StoreStatistics> EnvelopeService::statistics() const { return impl_->statistics(); }

Result<CommittedMap> EnvelopeService::committed_usage(const std::string& envelope_id) const {
  return impl_->committed_usage(envelope_id);
}

Result<std::vector<ObservationEntry>> EnvelopeService::observations(const std::string& envelope_id) const {
  return impl_->observations(envelope_id);
}

Result<DecisionRecord> EnvelopeService::find_decision_by_key(const std::string& envelope_id,
                                                             const std::string& idempotency_key) const {
  return impl_->find_decision_by_key(envelope_id, idempotency_key);
}

Status EnvelopeService::compact() { return impl_->compact(); }


// ---------------------------------------------------------------------------
// Decision vocabulary
// ---------------------------------------------------------------------------
const char* to_string(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::Granted: return "Granted";
    case Outcome::Denied: return "Denied";
    case Outcome::Indeterminate: return "Indeterminate";
  }
  return "UnknownOutcome";
}

const char* to_string(DimensionOutcome outcome) noexcept {
  switch (outcome) {
    case DimensionOutcome::Satisfied: return "Satisfied";
    case DimensionOutcome::Denied: return "Denied";
    case DimensionOutcome::Indeterminate: return "Indeterminate";
    case DimensionOutcome::NotDeclared: return "NotDeclared";
    case DimensionOutcome::NotRequested: return "NotRequested";
  }
  return "UnknownDimensionOutcome";
}

const char* to_string(RefusalStage stage) noexcept {
  switch (stage) {
    case RefusalStage::None: return "None";
    case RefusalStage::RequestValidation: return "RequestValidation";
    case RefusalStage::EnvelopePresence: return "EnvelopePresence";
    case RefusalStage::ScopeMatch: return "ScopeMatch";
    case RefusalStage::LineageIntegrity: return "LineageIntegrity";
    case RefusalStage::EnvelopeComposition: return "EnvelopeComposition";
    case RefusalStage::EnvelopeCurrent: return "EnvelopeCurrent";
    case RefusalStage::GenerationBinding: return "GenerationBinding";
    case RefusalStage::EffectiveWindow: return "EffectiveWindow";
    case RefusalStage::RequestedRevision: return "RequestedRevision";
    case RefusalStage::ContextBinding: return "ContextBinding";
    case RefusalStage::ProviderAvailability: return "ProviderAvailability";
    case RefusalStage::ReplayResolution: return "ReplayResolution";
    case RefusalStage::Arithmetic: return "Arithmetic";
    case RefusalStage::DimensionConstraint: return "DimensionConstraint";
  }
  return "UnknownRefusalStage";
}

bool refusal_stage_less(RefusalStage a, RefusalStage b) noexcept {
  return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
}

const char* to_string(FacilityComposition composition) noexcept {
  switch (composition) {
    case FacilityComposition::PrincipalOnly: return "principal-only";
    case FacilityComposition::ConjunctiveWithFacility: return "conjunctive";
  }
  return "unknown-composition";
}

bool facility_composition_from_string(std::string_view text, FacilityComposition& out) noexcept {
  if (text == "principal-only" || text == "principal") {
    out = FacilityComposition::PrincipalOnly;
    return true;
  }
  if (text == "conjunctive" || text == "conjunctive-with-facility") {
    out = FacilityComposition::ConjunctiveWithFacility;
    return true;
  }
  return false;
}


Result<StoreStatistics> ServiceImpl::statistics() const {
  std::optional<StoreStatistics> stats;
  const Status status = store_->with_snapshot([&](const StoreState& state) {
    StoreStatistics report;
    report.epoch = store_->control_epoch();
    report.state_epoch = store_->manifest().state_epoch;
    report.state_sequence =
        state.state_sequence == 0U ? store_->manifest().state_sequence : state.state_sequence;
    report.frames_committed = store_->manifest().frames_committed;
    report.envelope_count = state.envelopes.size();
    for (const auto& pair : state.envelopes) {
      report.retained_revision_count += pair.second.revisions.size();
      if (current_revision_record(pair.second) != nullptr) ++report.current_revision_count;
      if (pair.second.tombstoned) ++report.tombstoned_count;
    }
    report.decision_count = state.decisions.size();
    for (const DecisionRecord& record : state.decisions) {
      switch (record.outcome) {
        case Outcome::Granted: ++report.granted_count; break;
        case Outcome::Denied: ++report.denied_count; break;
        case Outcome::Indeterminate: ++report.indeterminate_count; break;
      }
    }
    report.usage_entry_count = state.usage_entry_count;
    report.observation_count = state.observations.size();
    report.committed_key_count = state.committed.size();
    report.compaction_count = store_->manifest().compaction_count;
    report.store_bytes = store_->total_bytes();
    report.state_digest = store_state_digest(state);
    report.chain_digest = store_->manifest().chain_digest;
    stats = report;
    return Status{};
  });
  if (!status.ok()) return status;
  if (!stats.has_value()) return Status(StatusCode::StoreCorrupt, "statistics produced no report");
  return *stats;
}
Status ServiceImpl::compact() { return store_->compact(); }
}  // namespace resource_envelope

