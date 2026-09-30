#ifndef RESOURCE_ENVELOPE_STORE_HPP
#define RESOURCE_ENVELOPE_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "resource_envelope/bytes.hpp"
#include "resource_envelope/decision.hpp"
#include "resource_envelope/digest.hpp"
#include "resource_envelope/envelope.hpp"
#include "resource_envelope/export.hpp"
#include "resource_envelope/record.hpp"
#include "resource_envelope/result.hpp"
#include "resource_envelope/status.hpp"
#include "resource_envelope/time.hpp"

namespace resource_envelope {

// ---------------------------------------------------------------------------
// Format and bound constants
// ---------------------------------------------------------------------------
// Every bound below is enforced before an allocation or a write, so a hostile or
// corrupt input can never cause unbounded work.
inline constexpr std::uint32_t kStoreFormatVersion = 1;
inline constexpr std::uint32_t kStoreLayoutVersion = 1;
inline constexpr std::uint64_t kMaxFramePayloadBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxSegmentBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxTotalStoreBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxSegmentCount = 4096;
inline constexpr std::uint64_t kMaxEnvelopeCount = 1000000ULL;
inline constexpr std::uint64_t kMaxDecisionCount = 10000000ULL;
inline constexpr std::uint64_t kMaxUsageEntryCount = 100000000ULL;
inline constexpr std::uint64_t kMaxObservationCount = 100000000ULL;
inline constexpr std::uint64_t kMaxJournalEntries = 4000000ULL;
// Retained superseded revisions per envelope. A decision that named a revision this
// bound has dropped reports EnvelopeMissing rather than being re-pointed silently.
inline constexpr std::size_t kMaxRetainedRevisionsPerEnvelope = 64;

inline constexpr std::uint16_t kSegmentKindDelta = 1;
inline constexpr std::uint16_t kSegmentKindSnapshot = 2;
inline constexpr std::size_t kFrameHeaderBytes = 88;
inline constexpr std::size_t kSegmentHeaderBytes = 32;

// ---------------------------------------------------------------------------
// Wire structures
// ---------------------------------------------------------------------------
// The 88-byte frame header. payload_length is validated against kMaxFramePayloadBytes
// before any allocation, and payload_crc32 is verified before the payload is parsed,
// so a torn or corrupted frame is detected before any structured decode runs. The
// digest binds the exact payload bytes.
struct RESOURCE_ENVELOPE_API FrameHeader {
  std::uint8_t magic[8] = {};
  std::uint16_t format_version = 0;
  std::uint16_t kind = 0;
  std::uint16_t flags = 0;
  std::uint16_t reserved = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t payload_crc32 = 0;
  std::uint64_t epoch = 0;
  std::uint64_t sequence = 0;
  std::uint8_t payload_digest[32] = {};
};

struct RESOURCE_ENVELOPE_API SegmentHeader {
  std::uint8_t magic[8] = {};
  std::uint16_t format_version = 0;
  std::uint16_t segment_kind = 0;
  std::uint32_t reserved = 0;
  std::uint64_t epoch = 0;
  std::uint64_t baseline_sequence = 0;
};
// The manifest is the single atomic commit point. It names the active snapshot and
// delta segments, the highest committed frame sequence, the store control epoch, and
// the digest of the state those frames produce.
struct RESOURCE_ENVELOPE_API Manifest {
  std::uint32_t layout_version = kStoreLayoutVersion;
  std::uint64_t epoch = 0;
  std::uint64_t state_epoch = 0;
  std::uint64_t snapshot_segment = 0;
  std::uint64_t delta_segment = 0;
  std::uint64_t frames_committed = 0;
  std::uint64_t state_sequence = 0;
  std::uint64_t snapshot_frames = 0;
  Digest state_digest;
  Digest chain_digest;
  std::uint64_t envelope_count = 0;
  std::uint64_t revision_count = 0;
  std::uint64_t decision_count = 0;
  std::uint64_t usage_entry_count = 0;
  std::uint64_t observation_count = 0;
  std::uint64_t compaction_count = 0;
};

RESOURCE_ENVELOPE_API Bytes canonical_manifest(const Manifest& manifest);
RESOURCE_ENVELOPE_API Result<Manifest> decode_manifest(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Digest manifest_digest(const Manifest& manifest) noexcept;

RESOURCE_ENVELOPE_API std::uint32_t crc32_ieee(std::span<const std::uint8_t> data) noexcept;
RESOURCE_ENVELOPE_API std::uint32_t crc32_ieee_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;

// Durable file names are generated from validated numeric identifiers only. Nothing
// inside a store is ever addressed by a caller-supplied string, which removes path
// traversal, alternate-data-stream syntax, reserved device names and reparse-point
// tricks from the durable layer by construction rather than by filtering.
RESOURCE_ENVELOPE_API std::string segment_file_name(std::uint64_t epoch);
RESOURCE_ENVELOPE_API bool parse_segment_file_name(std::string_view name, std::uint64_t& epoch) noexcept;

// ---------------------------------------------------------------------------
// Journal records
// ---------------------------------------------------------------------------
enum class JournalEntryKind : std::uint16_t {
  Snapshot = 1,
  EnvelopeDeclaration = 2,
  EnvelopeRevision = 3,
  UsageCommit = 4,
  Decision = 5,
  Observation = 6,
  EnvelopeTombstone = 7,
};

struct RESOURCE_ENVELOPE_API EnvelopeDeclarationEntry {
  Envelope envelope;
  std::string idempotency_key;
  Digest payload_digest;
  Timestamp declared_at = 0;
};

struct RESOURCE_ENVELOPE_API EnvelopeRevisionEntry {
  Envelope envelope;
  std::string idempotency_key;
  Digest payload_digest;
  Timestamp revised_at = 0;
};

struct RESOURCE_ENVELOPE_API EnvelopeTombstoneEntry {
  std::string envelope_id;
  std::uint64_t superseded_revision = 0;
  Digest last_record_digest;
  std::string reason;
  Timestamp tombstoned_at = 0;
};

struct RESOURCE_ENVELOPE_API SnapshotEntry {
  std::uint64_t snapshot_epoch = 0;
  std::uint64_t source_epoch = 0;
};

// One durable record. Exactly one optional member is engaged, selected by its kind;
// a decode that finds any other combination is refused rather than interpreted.
struct RESOURCE_ENVELOPE_API JournalEntry {
  JournalEntryKind kind = JournalEntryKind::Snapshot;
  std::uint64_t sequence = 0;
  std::uint64_t epoch = 0;
  std::optional<EnvelopeDeclarationEntry> declaration;
  std::optional<EnvelopeRevisionEntry> revision;
  std::optional<EnvelopeTombstoneEntry> tombstone;
  std::optional<UsageDelta> usage;
  std::optional<DecisionRecord> decision;
  std::optional<ObservationEntry> observation;
  std::optional<SnapshotEntry> snapshot;
};

RESOURCE_ENVELOPE_API const char* to_string(JournalEntryKind kind) noexcept;
RESOURCE_ENVELOPE_API bool is_valid(JournalEntryKind kind) noexcept;
RESOURCE_ENVELOPE_API void encode_journal_entry(ByteWriter& writer, const JournalEntry& entry);
RESOURCE_ENVELOPE_API Result<JournalEntry> decode_journal_entry(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Bytes canonical_journal_entry(const JournalEntry& entry);
// ---------------------------------------------------------------------------
// In-memory state
// ---------------------------------------------------------------------------
struct RESOURCE_ENVELOPE_API EnvelopeRevisionRecord {
  Envelope envelope;
  Digest record_digest;
  Digest content_digest;
  bool current = false;
  std::uint64_t stored_sequence = 0;
};

struct RESOURCE_ENVELOPE_API EnvelopeEntry {
  std::vector<EnvelopeRevisionRecord> revisions;
  std::string current_revision_key;
  bool tombstoned = false;
  // Idempotency key to revision number, for every revision of this envelope. A caller retries a
  // claim it cannot see the outcome of, so the key that produced a revision is recorded beside
  // it and a retry is resolved by key rather than by reconstructing a derived revision number.
  std::map<std::string, std::uint64_t> revision_keys;
};

struct RESOURCE_ENVELOPE_API CommittedKey {
  std::string envelope_id;
  DimensionKind kind = DimensionKind::CountRackPositions;
  std::string principal;
  std::string compatibility_class;

  [[nodiscard]] friend bool operator<(const CommittedKey& left, const CommittedKey& right) noexcept {
    if (left.envelope_id != right.envelope_id) return left.envelope_id < right.envelope_id;
    if (left.kind != right.kind) {
      return static_cast<std::uint8_t>(left.kind) < static_cast<std::uint8_t>(right.kind);
    }
    if (left.principal != right.principal) return left.principal < right.principal;
    return left.compatibility_class < right.compatibility_class;
  }
};

struct RESOURCE_ENVELOPE_API CommittedValue {
  std::int64_t value = 0;
  Timestamp observed_at = 0;
  std::string source;
};

using CommittedMap = std::map<CommittedKey, CommittedValue>;

// The effective state. The journal is part of it so that idempotent replay and audit
// history stay answerable after a restart; the journal is excluded from the state
// digest, which covers effective values only. An idempotent replay that appends
// nothing therefore cannot change the digest, while any change to an envelope, a
// decision, a commitment or an observation does change it.
struct RESOURCE_ENVELOPE_API StoreState {
  std::map<std::string, EnvelopeEntry> envelopes;
  std::vector<DecisionRecord> decisions;
  std::map<std::string, ObservationEntry> observations;
  CommittedMap committed;
  std::vector<JournalEntry> journal;
  // Committed retirements, in sequence order. A snapshot does not carry the journal, so
  // retirements are kept explicitly: without them a restarted process would treat a
  // retired envelope as active and could accept a second retirement under a new reason.
  std::vector<EnvelopeTombstoneEntry> tombstones;
  std::uint64_t state_sequence = 0;
  std::uint64_t revision_count = 0;
  std::uint64_t usage_entry_count = 0;
  std::uint64_t compaction_count = 0;
};

RESOURCE_ENVELOPE_API Bytes canonical_store_state(const StoreState& state);
RESOURCE_ENVELOPE_API Digest store_state_digest(const StoreState& state) noexcept;

// Applies one record to the state. expected_sequence must equal the record's own
// sequence; a gap is refused rather than tolerated.
RESOURCE_ENVELOPE_API Status apply_journal_entry(StoreState& state, const JournalEntry& entry,
                           std::uint64_t expected_sequence);

// ---------------------------------------------------------------------------
// Open options
// ---------------------------------------------------------------------------
enum class OpenMode : std::uint8_t {
  // Creates the store when the directory holds no store; fails closed when a store
  // exists but cannot be verified.
  OpenOrCreate = 0,
  // Fails when no store exists.
  OpenExisting = 1,
  // Inspection only: identical integrity and rollback rules, and no mutation of any
  // kind is permitted through the returned handle.
  ReadOnly = 2,
};

struct RESOURCE_ENVELOPE_API LockOptions {
  // Total time to wait for the file lock before failing. Zero fails immediately. This
  // is a lock-acquisition policy, not a watchdog over the work the lock protects.
  std::uint64_t acquire_timeout_ms = 2000;
  std::uint64_t retry_interval_ms = 5;
};

struct RESOURCE_ENVELOPE_API RecoveryReport {
  bool created = false;
  std::uint64_t recovered_epoch = 0;
  std::uint64_t recovered_frames = 0;
  std::uint64_t unreadable_tail_bytes = 0;
  std::uint64_t orphan_segments = 0;
  std::uint64_t stray_files_removed = 0;
  std::uint64_t temp_files_removed = 0;
  std::uint64_t envelopes_loaded = 0;
  std::uint64_t decisions_loaded = 0;
  std::uint64_t usage_entries_loaded = 0;
  std::uint64_t observations_loaded = 0;
  Digest state_digest;
  bool recovered_from_snapshot = false;
};

struct RESOURCE_ENVELOPE_API StoreOptions {
  OpenMode mode = OpenMode::OpenOrCreate;
  LockOptions lock;
  // When true, a store whose recorded state digest does not match the recomputed
  // state is refused rather than repaired.
  bool verify_chain_digest = true;
  // When true, an unreferenced segment found in the store directory is reported and
  // removed. An unreferenced segment is never read as authority.
  bool remove_orphan_segments = true;
};

// ---------------------------------------------------------------------------
// Durable write session
// ---------------------------------------------------------------------------
// Sequences are assigned monotonically by the session, and every appended record is
// validated and applied to the state before anything is written, so a record the
// store would refuse can never reach the journal.
class RESOURCE_ENVELOPE_API JournalSession {
 public:
  virtual ~JournalSession();
  JournalSession(const JournalSession&) = delete;
  JournalSession& operator=(const JournalSession&) = delete;

  [[nodiscard]] const StoreState& state() const noexcept { return state_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }

  // Validation hook installed by the store. It is what applies each record to the
  // state, so a refusal reported here abandons the whole critical section.
  using Validator = std::function<Status(const JournalEntry&)>;

  void stage_envelope_declaration(EnvelopeDeclarationEntry entry);
  void stage_envelope_revision(EnvelopeRevisionEntry entry);
  void stage_envelope_tombstone(EnvelopeTombstoneEntry entry);
  void stage_usage_commit(UsageDelta entry);
  void stage_decision(DecisionRecord entry);
  void stage_observation(ObservationEntry entry);

  // True when nothing was appended, which lets a read-only critical section avoid a
  // durable write and an epoch advance entirely.
  [[nodiscard]] bool empty() const noexcept { return appended_.empty(); }
  [[nodiscard]] const std::vector<JournalEntry>& appended() const noexcept { return appended_; }

  // A record that fails validation is not retained. The failure is reported here and
  // the whole critical section is abandoned, so a partially staged transaction can
  // never be published.
  [[nodiscard]] Status failure() const noexcept { return failure_; }

  // Constructed by the store on behalf of exactly one critical section. The state the
  // session refers to is owned by the store and lives no longer than the lock that
  // protects it, which is what keeps every staged record inside the transaction.
  JournalSession(StoreState& state, std::uint64_t next_sequence, Validator validator);

 private:
  void append(JournalEntry entry);

  StoreState& state_;
  std::uint64_t next_sequence_;
  Validator validator_;
  std::vector<JournalEntry> appended_;
  Status failure_;
};

// ---------------------------------------------------------------------------
// Store handle
// ---------------------------------------------------------------------------
// Concurrency contract:
//  * At most one process holds the writer lock, enforced by a real operating-system
//    file lock on a store-owned lock file. The kernel releases it when the process
//    dies, so a crash cannot leave the store permanently locked.
//  * Readers take a shared lock. Any number of readers proceed concurrently, no reader
//    observes a half-published commit because the manifest is replaced atomically
//    after the frames it names are durable and verified, and no reader blocks another.
//  * A writer loads its own state after taking the exclusive lock. There is no
//    read-to-write upgrade path and no method acquires the lock twice.
//  * No callback runs while an internal library lock is held; the only lock held
//    across a callback is the store file lock, which is what makes the critical
//    section exclusive across processes.
class RESOURCE_ENVELOPE_API Store {
 public:
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<Store>> open(
      const std::filesystem::path& root, const StoreOptions& options);

  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] OpenMode mode() const noexcept;
  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] std::uint64_t control_epoch() const noexcept;
  [[nodiscard]] const Manifest& manifest() const noexcept;
  [[nodiscard]] std::uint64_t total_bytes() const noexcept;

  // Read path: shared lock, verified consistent state, callback runs under it.
  [[nodiscard]] Status with_snapshot(
      const std::function<Status(const StoreState&)>& body) const;

  // Write path: exclusive lock for the whole call, state loaded after the lock is
  // taken, published only when records were staged.
  [[nodiscard]] Status with_writer(const std::function<Status(JournalSession&)>& body);

  // Rewrites live state into a fresh snapshot segment and publishes a manifest that
  // names it, then removes superseded segments. The snapshot is written, flushed, read
  // back and verified before the manifest names it, so a reader can never observe a
  // state it would refuse.
  [[nodiscard]] Status compact();

  // Removes segments and temporary files that no manifest references.
  [[nodiscard]] Status collect_garbage();

 private:
  class Impl;
  explicit Store(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_STORE_HPP
