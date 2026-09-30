#ifndef RESOURCE_ENVELOPE_SERVICE_HPP
#define RESOURCE_ENVELOPE_SERVICE_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "resource_envelope/decision.hpp"
#include "resource_envelope/digest.hpp"
#include "resource_envelope/envelope.hpp"
#include "resource_envelope/export.hpp"
#include "resource_envelope/record.hpp"
#include "resource_envelope/result.hpp"
#include "resource_envelope/store.hpp"

namespace resource_envelope {

// Internal service state. It is declared here so the service handle can hold it
// without exposing any of it; the definition lives entirely in the implementation
// file. It holds no authority of its own: state is loaded, decided and published
// inside one call, so nothing survives a restart.
class ServiceImpl;

inline constexpr std::uint64_t kMaxPrincipalTokenLength = 64;
inline constexpr std::uint64_t kMaxDeltasPerCall = 4096;

// ---------------------------------------------------------------------------
// Operation inputs
// ---------------------------------------------------------------------------
// A declaration that arrives with the same idempotency key and the same payload
// digest as a recorded one is resolved as a replay before any freshness check runs,
// which is what makes a lost response safe to retry. A replayed call returns the
// original revision unchanged and never advances the store epoch.
struct RESOURCE_ENVELOPE_API DeclareInput {
  Envelope envelope;
  std::string idempotency_key;
  Timestamp requested_at = 0;
};

struct RESOURCE_ENVELOPE_API EnvelopeDeclaration {
  std::string envelope_id;
  std::uint64_t revision = 0;
  Digest record_digest;
  Digest content_digest;
  std::uint64_t control_epoch = 0;
  bool replayed = false;
};

struct RESOURCE_ENVELOPE_API ReviseInput {
  Envelope envelope;
  std::optional<std::uint64_t> expected_current_revision;
  std::optional<Digest> expected_current_digest;
  std::string idempotency_key;
  Timestamp requested_at = 0;
};

struct RESOURCE_ENVELOPE_API EnvelopeRevision {
  std::string envelope_id;
  std::uint64_t previous_revision = 0;
  std::uint64_t revision = 0;
  Digest record_digest;
  Digest content_digest;
  std::uint64_t control_epoch = 0;
  bool replayed = false;
};

struct RESOURCE_ENVELOPE_API TombstoneInput {
  std::string envelope_id;
  std::optional<std::uint64_t> expected_current_revision;
  std::string reason;
  std::uint64_t request_id = 0;
  Timestamp requested_at = 0;
};

struct RESOURCE_ENVELOPE_API TombstoneResult {
  std::string envelope_id;
  std::uint64_t superseded_revision = 0;
  std::string reason;
  std::uint64_t request_id = 0;
  Digest last_record_digest;
  bool replayed = false;
};

struct RESOURCE_ENVELOPE_API UsageInput {
  std::string envelope_id;
  std::optional<std::uint64_t> expected_envelope_revision;
  std::vector<UsageDelta> deltas;
  std::string idempotency_key;
  Timestamp requested_at = 0;
};

struct RESOURCE_ENVELOPE_API UsageResult {
  std::string envelope_id;
  std::uint64_t envelope_revision = 0;
  std::uint64_t entries_appended = 0;
  std::uint64_t entries_replayed = 0;
  Digest entries_digest;
  std::uint64_t first_sequence = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t control_epoch = 0;
};

struct RESOURCE_ENVELOPE_API ObservationInput {
  std::string envelope_id;
  std::optional<std::uint64_t> expected_envelope_revision;
  std::vector<ObservationEntry> entries;
  std::string idempotency_key;
  Timestamp requested_at = 0;
};

struct RESOURCE_ENVELOPE_API ObservationResult {
  std::string envelope_id;
  std::uint64_t stored = 0;
  std::uint64_t replayed = 0;
  Digest entries_digest;
  std::uint64_t last_sequence = 0;
};

// An authorisation request is an evaluation that is also recorded. The decision and
// its durable record are produced inside one exclusive critical section, so the
// record can never describe an authority revision that was already superseded when
// the decision was made.
struct RESOURCE_ENVELOPE_API AuthorizeResult {
  EvaluationResult evaluation;
  DecisionRecord record;
  Digest decision_digest;
  bool replayed = false;
};

// Reporting views.
struct RESOURCE_ENVELOPE_API EnvelopeView {
  Envelope envelope;
  Digest record_digest;
  Digest content_digest;
  std::uint64_t stored_sequence = 0;
  bool current = false;
  bool tombstoned = false;
};

struct RESOURCE_ENVELOPE_API HistoryEntry {
  std::string kind;
  std::uint64_t sequence = 0;
  std::string envelope_id;
  std::uint64_t revision = 0;
  std::string detail;
  Digest digest;
  Timestamp at = 0;
};

struct RESOURCE_ENVELOPE_API StoreStatistics {
  std::uint64_t epoch = 0;
  std::uint64_t state_epoch = 0;
  std::uint64_t state_sequence = 0;
  std::uint64_t frames_committed = 0;
  std::uint64_t envelope_count = 0;
  std::uint64_t current_revision_count = 0;
  std::uint64_t retained_revision_count = 0;
  std::uint64_t tombstoned_count = 0;
  std::uint64_t decision_count = 0;
  std::uint64_t granted_count = 0;
  std::uint64_t denied_count = 0;
  std::uint64_t indeterminate_count = 0;
  std::uint64_t usage_entry_count = 0;
  std::uint64_t observation_count = 0;
  std::uint64_t committed_key_count = 0;
  std::uint64_t compaction_count = 0;
  std::uint64_t store_bytes = 0;
  Digest state_digest;
  Digest chain_digest;
};

struct RESOURCE_ENVELOPE_API VerifyInput {
  std::optional<std::string> decision_id;
  std::optional<Digest> expected_envelope_digest;
  std::optional<std::uint64_t> expected_envelope_revision;
  std::optional<IdentitySnapshot> identity;
  std::optional<ServiceClassResolution> service_class;
  std::optional<PolicyResolution> policy;
  Timestamp at = 0;
};

struct RESOURCE_ENVELOPE_API VerifyResult {
  AuthorityVerdict verdict;
  DecisionRecord record;
};

// ---------------------------------------------------------------------------
// Service
// ---------------------------------------------------------------------------
// Every method is a single critical section. `authorize` and `revise` are the only
// operations that advance authority; the read methods never mutate, and no read
// method can be starved by a writer because the lock is released between calls.
//
// The service holds no mutable in-memory authority: it loads state, decides, and
// publishes inside one call, so a restart has nothing live to preserve and stale
// authority cannot survive in a process.
class RESOURCE_ENVELOPE_API EnvelopeService {
 public:
  [[nodiscard]] static Result<std::unique_ptr<EnvelopeService>> open(const std::filesystem::path& root,
                                                                    const StoreOptions& options);

  ~EnvelopeService();
  EnvelopeService(const EnvelopeService&) = delete;
  EnvelopeService& operator=(const EnvelopeService&) = delete;
  EnvelopeService(EnvelopeService&&) = delete;
  EnvelopeService& operator=(EnvelopeService&&) = delete;

  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] const std::filesystem::path& root() const noexcept;

  // Authority advances.
  [[nodiscard]] Result<EnvelopeDeclaration> declare(const DeclareInput& input);
  [[nodiscard]] Result<EnvelopeRevision> revise(const ReviseInput& input);
  [[nodiscard]] Result<TombstoneResult> tombstone(const TombstoneInput& input);
  [[nodiscard]] Result<AuthorizeResult> authorize(const EvaluationRequest& request);
  [[nodiscard]] Result<UsageResult> record_usage(const UsageInput& input);
  [[nodiscard]] Result<ObservationResult> record_observation(const ObservationInput& input);

  // Read-only paths.
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

  // Result code reported when a replayed call matched an existing record.
  static constexpr StatusCode kReplayCode = StatusCode::ReplayedRequest;

 private:
  explicit EnvelopeService(std::unique_ptr<ServiceImpl> impl);
  std::unique_ptr<ServiceImpl> impl_;
};

// ---------------------------------------------------------------------------
// Pure evaluation entry points
// ---------------------------------------------------------------------------
// Both operate on data the caller supplies and never touch durable state, which is
// what allows the evaluation semantics to be tested in isolation and reused by an
// owner that keeps its own storage.
struct RESOURCE_ENVELOPE_API EvaluationContext {
  const Envelope* envelope = nullptr;
  // Optional facility envelope evaluated conjunctively. No synthetic merged limit is
  // ever constructed from the two envelopes.
  const Envelope* facility_envelope = nullptr;
  std::uint64_t control_epoch = 0;
  Digest authority_state_digest;
  // Committed usage observed from durable state, keyed by dimension.
  const CommittedMap* committed = nullptr;
  // Envelope the committed keys belong to; empty means "the primary envelope".
  std::string committed_envelope_id;
  // True when the primary envelope was found in durable state and is current. A
  // caller that evaluates a detached envelope leaves this false.
  bool envelope_is_current = true;
  std::uint64_t envelope_stored_sequence = 0;
  std::uint64_t decision_sequence = 0;
};

RESOURCE_ENVELOPE_API EvaluationResult evaluate(const Envelope& envelope,
                                                const EvaluationRequest& request);
RESOURCE_ENVELOPE_API EvaluationResult evaluate_with_context(const EvaluationContext& context,
                                                            const EvaluationRequest& request);

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_SERVICE_HPP
