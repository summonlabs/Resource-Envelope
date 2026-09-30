#include "resource_envelope/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <system_error>

#include "file_lock.hpp"
#include "store_io.hpp"

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/service.hpp"
#include "resource_envelope/text.hpp"

namespace resource_envelope {
namespace {

// Magic values are fixed literals, so a foreign or truncated file is rejected on its
// first bytes instead of being parsed as a damaged store.
constexpr std::array<std::uint8_t, 8> kSegmentMagic = {'R', 'E', 'N', 'V', 'S', 'E', 'G', '1'};
constexpr std::array<std::uint8_t, 8> kFrameMagic = {'R', 'E', 'N', 'V', 'F', 'R', 'M', '1'};
constexpr std::array<std::uint8_t, 8> kFenceMagic = {'R', 'E', 'N', 'V', 'F', 'E', 'N', '1'};

constexpr const char* kManifestName = "manifest.renv";
constexpr const char* kManifestTempName = "manifest.part";
constexpr const char* kFenceName = "epoch.fence";
constexpr const char* kLockName = "store.lock";
constexpr const char* kSegmentsDirectoryName = "segments";
constexpr std::size_t kFenceBytes = 48U;

void put_u16(std::span<std::uint8_t> out, std::size_t offset, std::uint16_t value) noexcept {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void put_u32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value) noexcept {
  for (unsigned index = 0; index < 4U; ++index) {
    out[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
  }
}

void put_u64(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value) noexcept {
  for (unsigned index = 0; index < 8U; ++index) {
    out[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
  }
}

std::uint16_t get_u16(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset]) |
                                     static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset + 1U]) << 8U));
}

std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(in[offset + index]) << (8U * index);
  }
  return value;
}

std::uint64_t get_u64(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(in[offset + index]) << (8U * index);
  }
  return value;
}

bool magic_matches(std::span<const std::uint8_t> in, const std::array<std::uint8_t, 8>& magic) noexcept {
  if (in.size() < magic.size()) return false;
  for (std::size_t index = 0; index < magic.size(); ++index) {
    if (in[index] != magic[index]) return false;
  }
  return true;
}

std::array<std::uint8_t, kSegmentHeaderBytes> encode_segment_header(std::uint16_t segment_kind,
                                                                    std::uint64_t epoch,
                                                                    std::uint64_t baseline) noexcept {
  std::array<std::uint8_t, kSegmentHeaderBytes> out{};
  std::copy(kSegmentMagic.begin(), kSegmentMagic.end(), out.begin());
  put_u16(out, 8U, kStoreFormatVersion);
  put_u16(out, 10U, segment_kind);
  put_u32(out, 12U, 0U);
  put_u64(out, 16U, epoch);
  put_u64(out, 24U, baseline);
  return out;
}

std::array<std::uint8_t, kFrameHeaderBytes> encode_frame_header(std::uint16_t kind,
                                                                 std::uint32_t payload_length,
                                                                 std::uint32_t crc, std::uint64_t epoch,
                                                                 std::uint64_t sequence,
                                                                 const Digest& payload_digest) noexcept {
  std::array<std::uint8_t, kFrameHeaderBytes> out{};
  std::copy(kFrameMagic.begin(), kFrameMagic.end(), out.begin());
  put_u16(out, 8U, kStoreFormatVersion);
  put_u16(out, 10U, kind);
  put_u16(out, 12U, 0U);
  put_u16(out, 14U, 0U);
  put_u32(out, 16U, payload_length);
  put_u32(out, 20U, crc);
  put_u64(out, 24U, epoch);
  put_u64(out, 32U, sequence);
  if (payload_digest.known()) {
    std::copy(payload_digest.value().begin(), payload_digest.value().end(), out.begin() + 40U);
  }
  return out;
}

bool parse_fence(const Bytes& bytes, std::uint64_t& epoch) noexcept {
  if (bytes.size() != kFenceBytes) return false;
  if (!magic_matches(bytes, kFenceMagic)) return false;
  if (get_u16(bytes, 8U) != kStoreFormatVersion) return false;
  if (get_u32(bytes, 12U) != kStoreLayoutVersion) return false;
  epoch = get_u64(bytes, 16U);
  return get_u32(bytes, 24U) == crc32_ieee(std::span<const std::uint8_t>(bytes.data(), 24U));
}

Bytes encode_fence(std::uint64_t epoch) {
  Bytes out(kFenceBytes, 0U);
  std::copy(kFenceMagic.begin(), kFenceMagic.end(), out.begin());
  put_u16(out, 8U, kStoreFormatVersion);
  put_u32(out, 12U, kStoreLayoutVersion);
  put_u64(out, 16U, epoch);
  put_u32(out, 24U, crc32_ieee(std::span<const std::uint8_t>(out.data(), 24U)));
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Journal codec
// ---------------------------------------------------------------------------
void encode_journal_entry(ByteWriter& writer, const JournalEntry& entry) {
  writer.u8(static_cast<std::uint8_t>(entry.kind));
  writer.u64(entry.sequence);
  writer.u64(entry.epoch);
  switch (entry.kind) {
    case JournalEntryKind::Snapshot: {
      const SnapshotEntry snapshot = entry.snapshot.value_or(SnapshotEntry{});
      writer.u64(snapshot.snapshot_epoch);
      writer.u64(snapshot.source_epoch);
      break;
    }
    case JournalEntryKind::EnvelopeDeclaration: {
      const EnvelopeDeclarationEntry declaration =
          entry.declaration.value_or(EnvelopeDeclarationEntry{});
      writer.blob(canonical_envelope(declaration.envelope));
      writer.text(declaration.idempotency_key);
      encode_digest(writer, declaration.payload_digest);
      writer.i64(declaration.declared_at);
      break;
    }
    case JournalEntryKind::EnvelopeRevision: {
      const EnvelopeRevisionEntry revision = entry.revision.value_or(EnvelopeRevisionEntry{});
      writer.blob(canonical_envelope(revision.envelope));
      writer.text(revision.idempotency_key);
      encode_digest(writer, revision.payload_digest);
      writer.i64(revision.revised_at);
      break;
    }
    case JournalEntryKind::EnvelopeTombstone: {
      const EnvelopeTombstoneEntry tombstone = entry.tombstone.value_or(EnvelopeTombstoneEntry{});
      writer.text(tombstone.envelope_id);
      writer.u64(tombstone.superseded_revision);
      encode_digest(writer, tombstone.last_record_digest);
      writer.text(tombstone.reason);
      writer.i64(tombstone.tombstoned_at);
      break;
    }
    case JournalEntryKind::UsageCommit:
      writer.blob(canonical_usage_delta(entry.usage.value_or(UsageDelta{})));
      break;
    case JournalEntryKind::Decision:
      writer.blob(canonical_decision_record(entry.decision.value_or(DecisionRecord{})));
      break;
    case JournalEntryKind::Observation:
      writer.blob(canonical_observation_entry(entry.observation.value_or(ObservationEntry{})));
      break;
  }
}

Bytes canonical_journal_entry(const JournalEntry& entry) {
  ByteWriter writer;
  writer.u16(kCanonicalFormatVersion);
  encode_journal_entry(writer, entry);
  return writer.take();
}

Result<JournalEntry> decode_journal_entry(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalRecordBytes) {
    return Status(StatusCode::OutOfRange, "journal entry exceeds the canonical size bound");
  }
  ByteReader reader(data);
  JournalEntry entry;
  const std::uint16_t version = reader.u16();
  if (!reader.ok() || version != kCanonicalFormatVersion) {
    return Status(StatusCode::UnsupportedVersion, "journal entry carries an unsupported format version");
  }
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<JournalEntryKind>(kind))) {
    return Status(StatusCode::InvalidEnumValue, "journal entry kind is not a defined value");
  }
  entry.kind = static_cast<JournalEntryKind>(kind);
  entry.sequence = reader.u64();
  entry.epoch = reader.u64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "journal entry header is truncated");

  switch (entry.kind) {
    case JournalEntryKind::Snapshot: {
      SnapshotEntry snapshot;
      snapshot.snapshot_epoch = reader.u64();
      snapshot.source_epoch = reader.u64();
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "snapshot entry is truncated");
      entry.snapshot = snapshot;
      break;
    }
    case JournalEntryKind::EnvelopeDeclaration: {
      const Bytes payload = reader.blob(kMaxCanonicalEnvelopeBytes);
      EnvelopeDeclarationEntry declaration;
      declaration.idempotency_key = reader.text(kIdempotencyKeyMaxLength);
      if (!reader.ok() || !decode_digest(reader, declaration.payload_digest)) {
        return Status(StatusCode::TruncatedPayload, "declaration entry is truncated");
      }
      declaration.declared_at = reader.i64();
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "declaration entry is truncated");
      Result<Envelope> envelope = decode_envelope(payload);
      if (!envelope.ok()) return envelope.status();
      declaration.envelope = std::move(envelope.value());
      entry.declaration = std::move(declaration);
      break;
    }
    case JournalEntryKind::EnvelopeRevision: {
      const Bytes payload = reader.blob(kMaxCanonicalEnvelopeBytes);
      EnvelopeRevisionEntry revision;
      revision.idempotency_key = reader.text(kIdempotencyKeyMaxLength);
      if (!reader.ok() || !decode_digest(reader, revision.payload_digest)) {
        return Status(StatusCode::TruncatedPayload, "revision entry is truncated");
      }
      revision.revised_at = reader.i64();
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "revision entry is truncated");
      Result<Envelope> envelope = decode_envelope(payload);
      if (!envelope.ok()) return envelope.status();
      revision.envelope = std::move(envelope.value());
      entry.revision = std::move(revision);
      break;
    }
    case JournalEntryKind::EnvelopeTombstone: {
      EnvelopeTombstoneEntry tombstone;
      tombstone.envelope_id = reader.text(kIdentifierMaxLength);
      tombstone.superseded_revision = reader.u64();
      if (!reader.ok() || !decode_digest(reader, tombstone.last_record_digest)) {
        return Status(StatusCode::TruncatedPayload, "tombstone entry is truncated");
      }
      tombstone.reason = reader.text(kLabelMaxLength);
      tombstone.tombstoned_at = reader.i64();
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "tombstone entry is truncated");
      entry.tombstone = std::move(tombstone);
      break;
    }
    case JournalEntryKind::UsageCommit: {
      const Bytes payload = reader.blob(kMaxCanonicalRecordBytes);
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "usage entry is truncated");
      Result<UsageDelta> delta = decode_usage_delta(payload);
      if (!delta.ok()) return delta.status();
      entry.usage = std::move(delta.value());
      break;
    }
    case JournalEntryKind::Decision: {
      const Bytes payload = reader.blob(kMaxCanonicalDecisionBytes);
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "decision entry is truncated");
      Result<DecisionRecord> record = decode_decision_record(payload);
      if (!record.ok()) return record.status();
      entry.decision = std::move(record.value());
      break;
    }
    case JournalEntryKind::Observation: {
      const Bytes payload = reader.blob(kMaxCanonicalRecordBytes);
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "observation entry is truncated");
      Result<ObservationEntry> observation = decode_observation_entry(payload);
      if (!observation.ok()) return observation.status();
      entry.observation = std::move(observation.value());
      break;
    }
  }
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "journal entry did not decode exactly");
  }
  return entry;
}

Status apply_journal_entry(StoreState& state, const JournalEntry& entry, std::uint64_t expected_sequence) {
  if (entry.sequence != expected_sequence) {
    return Status(StatusCode::StoreCorrupt, "journal sequence gap detected while replaying the store");
  }
  switch (entry.kind) {
    case JournalEntryKind::Snapshot:
      // A snapshot frame carries control information; the state itself is reconstructed
      // from the containing snapshot segment.
      return Status{};
    case JournalEntryKind::EnvelopeDeclaration: {
      if (!entry.declaration.has_value()) {
        return Status(StatusCode::StoreCorrupt, "declaration frame carries no declaration");
      }
      const Envelope& envelope = entry.declaration->envelope;
      EnvelopeEntry& target = state.envelopes[envelope.id];
      if (target.tombstoned) {
        return Status(StatusCode::StoreCorrupt, "declaration names a retired envelope");
      }
      for (EnvelopeRevisionRecord& existing : target.revisions) {
        if (existing.envelope.revision == envelope.revision) {
          return Status(StatusCode::StoreCorrupt, "declaration repeats an existing revision");
        }
        existing.current = false;
      }
      EnvelopeRevisionRecord record;
      record.record_digest = record_digest(envelope);
      record.content_digest = content_digest(envelope);
      record.current = true;
      record.stored_sequence = entry.sequence;
      record.envelope = envelope;
      target.current_revision_key = envelope.id + "@" + std::to_string(envelope.revision);
      if (!entry.declaration->idempotency_key.empty()) {
        target.revision_keys[entry.declaration->idempotency_key] = envelope.revision;
      }
      target.revisions.push_back(std::move(record));
      ++state.revision_count;
      return Status{};
    }
    case JournalEntryKind::EnvelopeRevision: {
      if (!entry.revision.has_value()) {
        return Status(StatusCode::StoreCorrupt, "revision frame carries no revision");
      }
      const Envelope& envelope = entry.revision->envelope;
      const auto found = state.envelopes.find(envelope.id);
      if (found == state.envelopes.end() || found->second.tombstoned) {
        return Status(StatusCode::StoreCorrupt, "revision names an envelope that is not present");
      }
      std::uint64_t previous = 0;
      for (EnvelopeRevisionRecord& existing : found->second.revisions) {
        if (existing.current) previous = existing.envelope.revision;
        if (existing.envelope.revision == envelope.revision) {
          return Status(StatusCode::StoreCorrupt, "revision repeats an existing revision number");
        }
        existing.current = false;
      }
      if (envelope.revision != previous + 1U) {
        return Status(StatusCode::StoreCorrupt, "revision does not advance by exactly one");
      }
      EnvelopeRevisionRecord record;
      record.record_digest = record_digest(envelope);
      record.content_digest = content_digest(envelope);
      record.current = true;
      record.stored_sequence = entry.sequence;
      record.envelope = envelope;
      found->second.current_revision_key = envelope.id + "@" + std::to_string(envelope.revision);
      if (!entry.revision->idempotency_key.empty()) {
        found->second.revision_keys[entry.revision->idempotency_key] = envelope.revision;
      }
      found->second.revisions.push_back(std::move(record));
      ++state.revision_count;
      while (found->second.revisions.size() > kMaxRetainedRevisionsPerEnvelope) {
        // Retention is bounded. A decision that named a dropped revision is reported as
        // EnvelopeMissing rather than being re-pointed at a newer revision.
        const auto oldest =
            std::find_if(found->second.revisions.begin(), found->second.revisions.end(),
                         [](const EnvelopeRevisionRecord& candidate) { return !candidate.current; });
        if (oldest == found->second.revisions.end()) break;
        found->second.revisions.erase(oldest);
      }
      return Status{};
    }
    case JournalEntryKind::EnvelopeTombstone: {
      if (!entry.tombstone.has_value()) {
        return Status(StatusCode::StoreCorrupt, "tombstone frame carries no tombstone");
      }
      const auto found = state.envelopes.find(entry.tombstone->envelope_id);
      if (found == state.envelopes.end()) {
        return Status(StatusCode::StoreCorrupt, "tombstone names an envelope that is not present");
      }
      for (EnvelopeRevisionRecord& existing : found->second.revisions) existing.current = false;
      found->second.tombstoned = true;
      found->second.current_revision_key.clear();
      state.tombstones.push_back(*entry.tombstone);
      return Status{};
    }
    case JournalEntryKind::UsageCommit: {
      if (!entry.usage.has_value()) {
        return Status(StatusCode::StoreCorrupt, "usage frame carries no usage record");
      }
      const UsageDelta& delta = *entry.usage;
      const CommittedKey key{delta.envelope_id, delta.kind, delta.principal, delta.compatibility_class};
      CommittedValue& value = state.committed[key];
      // Committed usage is a fold over deltas. A delta that would take the running
      // total outside the representable range is refused rather than wrapped.
      const std::int64_t current = value.value;
      if (delta.delta > 0 && current > std::numeric_limits<std::int64_t>::max() - delta.delta) {
        return Status(StatusCode::StoreCorrupt, "committed usage would overflow while replaying");
      }
      if (delta.delta < 0 && current < std::numeric_limits<std::int64_t>::min() - delta.delta) {
        return Status(StatusCode::StoreCorrupt, "committed usage would underflow while replaying");
      }
      value.value = current + delta.delta;
      if (delta.recorded_at > value.observed_at) value.observed_at = delta.recorded_at;
      if (!delta.source.empty()) value.source = delta.source;
      // The claim is recorded beside the aggregate it contributed to. It is the claim as the
      // caller stated it, so a retry after a compaction - which drops the journal - is still
      // recognised as the same claim rather than counted a second time.
      UsageClaim claim;
      claim.envelope_id = delta.envelope_id;
      claim.idempotency_key = delta.idempotency_key;
      claim.claim_digest = usage_claim_digest(delta);
      claim.envelope_revision = delta.envelope_revision;
      state.usage_claims.insert(std::move(claim));
      ++state.usage_entry_count;
      if (state.usage_entry_count > kMaxUsageEntryCount) {
        return Status(StatusCode::StoreLimitExceeded, "the store holds more usage records than permitted");
      }
      return Status{};
    }
    case JournalEntryKind::Decision: {
      if (!entry.decision.has_value()) {
        return Status(StatusCode::StoreCorrupt, "decision frame carries no decision record");
      }
      if (state.decisions.size() >= kMaxDecisionCount) {
        return Status(StatusCode::StoreLimitExceeded, "the store holds more decisions than permitted");
      }
      state.decisions.push_back(*entry.decision);
      return Status{};
    }
    case JournalEntryKind::Observation: {
      if (!entry.observation.has_value()) {
        return Status(StatusCode::StoreCorrupt, "observation frame carries no observation");
      }
      const bool is_new = state.observations.find(entry.observation->entry_id) == state.observations.end();
      if (is_new && state.observations.size() >= kMaxObservationCount) {
        return Status(StatusCode::StoreLimitExceeded, "the store holds more observations than permitted");
      }
      state.observations[entry.observation->entry_id] = *entry.observation;
      return Status{};
    }
  }
  return Status(StatusCode::StoreCorrupt, "journal entry kind is not a defined value");
}

// The effective state, encoded canonically. The journal is excluded, because the state
// a reader observes is what is digested: replaying an idempotent request that appends
// nothing leaves the digest unchanged, while any change to an envelope, a decision, a
// commitment or an observation changes it.
Bytes canonical_store_state(const StoreState& state) {
  ByteWriter writer;
  writer.u16(kCanonicalFormatVersion);
  writer.u32(kStoreLayoutVersion);
  writer.u32(static_cast<std::uint32_t>(state.envelopes.size()));
  for (const auto& pair : state.envelopes) {
    writer.text(pair.first);
    writer.bool_value(pair.second.tombstoned);
    writer.text(pair.second.current_revision_key);
    writer.u32(static_cast<std::uint32_t>(pair.second.revisions.size()));
    for (const EnvelopeRevisionRecord& record : pair.second.revisions) {
      writer.blob(canonical_envelope(record.envelope));
      writer.bool_value(record.current);
      writer.u64(record.stored_sequence);
    }
    writer.u32(static_cast<std::uint32_t>(pair.second.revision_keys.size()));
    for (const auto& key : pair.second.revision_keys) {
      writer.text(key.first);
      writer.u64(key.second);
    }
  }
  writer.u32(static_cast<std::uint32_t>(state.decisions.size()));
  for (const DecisionRecord& record : state.decisions) writer.blob(canonical_decision_record(record));
  writer.u32(static_cast<std::uint32_t>(state.observations.size()));
  for (const auto& pair : state.observations) {
    writer.text(pair.first);
    writer.blob(canonical_observation_entry(pair.second));
  }
  writer.u32(static_cast<std::uint32_t>(state.committed.size()));
  for (const auto& pair : state.committed) {
    writer.text(pair.first.envelope_id);
    writer.u8(static_cast<std::uint8_t>(pair.first.kind));
    writer.text(pair.first.principal);
    writer.text(pair.first.compatibility_class);
    writer.i64(pair.second.value);
    writer.i64(pair.second.observed_at);
    writer.text(pair.second.source);
  }
  writer.u32(static_cast<std::uint32_t>(state.usage_claims.size()));
  for (const UsageClaim& claim : state.usage_claims) {
    writer.text(claim.envelope_id);
    writer.text(claim.idempotency_key);
    encode_digest(writer, claim.claim_digest);
    writer.u64(claim.envelope_revision);
  }
  writer.u32(static_cast<std::uint32_t>(state.tombstones.size()));
  for (const EnvelopeTombstoneEntry& entry : state.tombstones) {
    writer.text(entry.envelope_id);
    writer.u64(entry.superseded_revision);
    encode_digest(writer, entry.last_record_digest);
    writer.text(entry.reason);
    writer.i64(entry.tombstoned_at);
  }
  writer.u64(state.state_sequence);
  writer.u64(state.revision_count);
  writer.u64(state.usage_entry_count);
  writer.u64(state.compaction_count);
  return writer.take();
}

Digest store_state_digest(const StoreState& state) noexcept {
  // The digest covers the effective envelope state only. The journal and the compaction
  // counter are store bookkeeping: folding either in would make the digest depend on how a
  // state was reached, so two states a reader cannot tell apart would compare unequal, and an
  // unrelated compaction would change the commitment to constraints that did not change.
  StoreState effective = state;
  effective.journal.clear();
  effective.compaction_count = 0U;
  const Bytes body = canonical_store_state(effective);
  return Digest(sha256_domain(kDomainState, std::span<const std::uint8_t>(body.data(), body.size())));
}

namespace {

// The snapshot's own metadata block. It is recorded beside the state so that the counters a
// snapshot reports can be cross-checked against the state it carries, and so that a future
// reader can trust the snapshot alone.
struct SnapshotMetadata {
  std::uint64_t snapshot_epoch = 0;
  std::uint64_t source_epoch = 0;
  std::uint64_t envelope_count = 0;
  std::uint64_t revision_count = 0;
  std::uint64_t decision_count = 0;
  std::uint64_t usage_entry_count = 0;
  std::uint64_t observation_count = 0;
  std::uint64_t compaction_count = 0;
};

struct DecodedSnapshot {
  StoreState state;
  SnapshotMetadata metadata;
};

std::uint64_t count_revisions(const StoreState& state) noexcept {
  std::uint64_t total = 0U;
  for (const auto& pair : state.envelopes) total += pair.second.revisions.size();
  return total;
}

Result<DecodedSnapshot> decode_store_state(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxFramePayloadBytes) {
    return Status(StatusCode::OutOfRange, "the snapshot payload exceeds the frame bound");
  }
  // The frame payload is the canonical state wrapped in a length-prefixed blob, followed by
  // the snapshot's own metadata. The blob is unwrapped before the state is decoded: reading the
  // state directly from the frame payload would interpret the blob length as the format version.
  ByteReader outer(data);
  const Bytes body = outer.blob(kMaxFramePayloadBytes);
  if (!outer.ok()) {
    return Status(StatusCode::TruncatedPayload, "the snapshot payload does not hold a bounded state blob");
  }
  if (outer.remaining() < 8U * 8U) {
    return Status(StatusCode::TruncatedPayload, "the snapshot payload is missing its metadata block");
  }
  SnapshotMetadata metadata;
  metadata.snapshot_epoch = outer.u64();
  metadata.source_epoch = outer.u64();
  metadata.envelope_count = outer.u64();
  metadata.revision_count = outer.u64();
  metadata.decision_count = outer.u64();
  metadata.usage_entry_count = outer.u64();
  metadata.observation_count = outer.u64();
  metadata.compaction_count = outer.u64();
  if (!outer.ok() || !outer.require_exhausted()) {
    return Status(StatusCode::TrailingBytes, "the snapshot payload holds more than one state blob and one metadata block");
  }
  ByteReader reader(body);
  StoreState state;
  const std::uint16_t version = reader.u16();
  const std::uint32_t layout = reader.u32();
  if (!reader.ok() || version != kCanonicalFormatVersion || layout != kStoreLayoutVersion) {
    return Status(StatusCode::UnsupportedVersion, "the snapshot payload carries an unsupported version");
  }
  const std::uint32_t envelope_count = reader.u32();
  if (!reader.ok() || envelope_count > kMaxEnvelopeCount) {
    return Status(StatusCode::OutOfRange, "the snapshot declares an implausible envelope count");
  }
  for (std::uint32_t index = 0; index < envelope_count; ++index) {
    const std::string id = reader.text(kIdentifierMaxLength);
    EnvelopeEntry entry;
    entry.tombstoned = reader.bool_value();
    entry.current_revision_key = reader.text(kIdentifierMaxLength * 2U + 2U);
    const std::uint32_t revision_count = reader.u32();
    if (!reader.ok() || revision_count > kMaxRetainedRevisionsPerEnvelope) {
      return Status(StatusCode::OutOfRange, "the snapshot declares too many retained revisions");
    }
    for (std::uint32_t revision_index = 0; revision_index < revision_count; ++revision_index) {
      const Bytes payload = reader.blob(kMaxCanonicalEnvelopeBytes);
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored envelope is truncated");
      Result<Envelope> envelope = decode_envelope(payload);
      if (!envelope.ok()) return envelope.status();
      EnvelopeRevisionRecord record;
      record.current = reader.bool_value();
      record.stored_sequence = reader.u64();
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored revision is truncated");
      record.record_digest = record_digest(envelope.value());
      record.content_digest = content_digest(envelope.value());
      record.envelope = std::move(envelope.value());
      entry.revisions.push_back(std::move(record));
    }
    const std::uint32_t key_count = reader.u32();
    if (!reader.ok() || key_count > kMaxRetainedRevisionsPerEnvelope) {
      return Status(StatusCode::OutOfRange, "the snapshot declares too many revision keys");
    }
    for (std::uint32_t key_index = 0; key_index < key_count; ++key_index) {
      const std::string key = reader.text(kIdempotencyKeyMaxLength);
      const std::uint64_t revision = reader.u64();
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored revision key is truncated");
      entry.revision_keys[key] = revision;
    }
    state.envelopes[id] = std::move(entry);
  }
  const std::uint32_t decision_count = reader.u32();
  if (!reader.ok() || decision_count > kMaxDecisionCount) {
    return Status(StatusCode::OutOfRange, "the snapshot declares an implausible decision count");
  }
  for (std::uint32_t index = 0; index < decision_count; ++index) {
    const Bytes payload = reader.blob(kMaxCanonicalDecisionBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored decision is truncated");
    Result<DecisionRecord> record = decode_decision_record(payload);
    if (!record.ok()) return record.status();
    state.decisions.push_back(std::move(record.value()));
  }
  const std::uint32_t observation_count = reader.u32();
  if (!reader.ok() || observation_count > kMaxObservationCount) {
    return Status(StatusCode::OutOfRange, "the snapshot declares an implausible observation count");
  }
  for (std::uint32_t index = 0; index < observation_count; ++index) {
    const std::string key = reader.text(kDecisionRefMaxLength);
    const Bytes payload = reader.blob(kMaxCanonicalRecordBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored observation is truncated");
    Result<ObservationEntry> observation = decode_observation_entry(payload);
    if (!observation.ok()) return observation.status();
    state.observations[key] = std::move(observation.value());
  }
  const std::uint32_t committed_count = reader.u32();
  if (!reader.ok() || committed_count > kMaxUsageEntryCount) {
    return Status(StatusCode::OutOfRange, "the snapshot declares an implausible commitment count");
  }
  for (std::uint32_t index = 0; index < committed_count; ++index) {
    CommittedKey key;
    key.envelope_id = reader.text(kIdentifierMaxLength);
    const std::uint8_t kind = reader.u8();
    if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
      return Status(StatusCode::InvalidEnumValue, "a stored commitment names an undefined dimension");
    }
    key.kind = static_cast<DimensionKind>(kind);
    key.principal = reader.text(kMaxPrincipalTokenLength);
    key.compatibility_class = reader.text(kCompatibilityClassMaxLength);
    CommittedValue value;
    value.value = reader.i64();
    value.observed_at = reader.i64();
    value.source = reader.text(kProvenanceMaxLength);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored commitment is truncated");
    // A commitment is a fold over deltas and a release can take it below zero. That is
    // recorded evidence of an over-release, not corruption, and the evaluator already refuses
    // to interpret it as capacity. Refusing it here instead made a state the runtime had
    // accepted impossible to load back, which is the one thing a store must never do.
    state.committed[key] = value;
  }
  const std::uint32_t claim_count = reader.u32();
  if (!reader.ok() || claim_count > kMaxUsageEntryCount) {
    return Status(StatusCode::OutOfRange, "the snapshot declares an implausible usage claim count");
  }
  for (std::uint32_t index = 0; index < claim_count; ++index) {
    UsageClaim claim;
    claim.envelope_id = reader.text(kIdentifierMaxLength);
    claim.idempotency_key = reader.text(kIdempotencyKeyMaxLength);
    if (!reader.ok() || !decode_digest(reader, claim.claim_digest)) {
      return Status(StatusCode::TruncatedPayload, "a stored usage claim is truncated");
    }
    claim.envelope_revision = reader.u64();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored usage claim is truncated");
    if (!state.usage_claims.insert(std::move(claim)).second) {
      // The encoding is a set, so a duplicate is a malformed body rather than something to
      // collapse silently: two identical claims are indistinguishable from a corrupted count.
      return Status(StatusCode::StoreCorrupt, "the snapshot declares the same usage claim twice");
    }
  }
  const std::uint32_t tombstone_count = reader.u32();
  if (!reader.ok() || tombstone_count > kMaxEnvelopeCount) {
    return Status(StatusCode::OutOfRange, "the snapshot declares an implausible retirement count");
  }
  for (std::uint32_t index = 0; index < tombstone_count; ++index) {
    EnvelopeTombstoneEntry entry;
    entry.envelope_id = reader.text(kIdentifierMaxLength);
    entry.superseded_revision = reader.u64();
    if (!reader.ok() || !decode_digest(reader, entry.last_record_digest)) {
      return Status(StatusCode::TruncatedPayload, "a stored retirement is truncated");
    }
    entry.reason = reader.text(kLabelMaxLength);
    entry.tombstoned_at = reader.i64();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "a stored retirement is truncated");
    state.tombstones.push_back(std::move(entry));
  }
  state.state_sequence = reader.u64();
  state.revision_count = reader.u64();
  state.usage_entry_count = reader.u64();
  state.compaction_count = reader.u64();
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "the snapshot payload did not decode exactly");
  }
  // The state and the metadata that accompanies it must agree, so a snapshot cannot report
  // one set of counters and carry another.
  if (state.revision_count != count_revisions(state)) {
    return Status(StatusCode::StoreIntegrityMismatch, "the snapshot revision count disagrees with its content");
  }
  if (metadata.envelope_count != state.envelopes.size() ||
      metadata.revision_count != state.revision_count ||
      metadata.decision_count != state.decisions.size() ||
      metadata.usage_entry_count != state.usage_entry_count ||
      metadata.observation_count != state.observations.size() ||
      metadata.compaction_count != state.compaction_count) {
    return Status(StatusCode::StoreIntegrityMismatch,
                  "the snapshot metadata disagrees with the state it carries");
  }
  DecodedSnapshot decoded;
  decoded.state = std::move(state);
  decoded.metadata = metadata;
  return decoded;
}

}  // namespace

JournalSession::~JournalSession() = default;

JournalSession::JournalSession(StoreState& state, std::uint64_t next_sequence, Validator validator)
    : state_(state), next_sequence_(next_sequence), validator_(std::move(validator)) {}

void JournalSession::append(JournalEntry entry) {
  if (!failure_.ok()) return;
  entry.sequence = next_sequence_;
  if (validator_) {
    const Status validated = validator_(entry);
    if (!validated.ok()) {
      failure_ = validated;
      return;
    }
  }
  appended_.push_back(std::move(entry));
  ++next_sequence_;
}

void JournalSession::stage_envelope_declaration(EnvelopeDeclarationEntry entry) {
  JournalEntry journal;
  journal.kind = JournalEntryKind::EnvelopeDeclaration;
  journal.declaration = std::move(entry);
  append(std::move(journal));
}

void JournalSession::stage_envelope_revision(EnvelopeRevisionEntry entry) {
  JournalEntry journal;
  journal.kind = JournalEntryKind::EnvelopeRevision;
  journal.revision = std::move(entry);
  append(std::move(journal));
}

void JournalSession::stage_envelope_tombstone(EnvelopeTombstoneEntry entry) {
  JournalEntry journal;
  journal.kind = JournalEntryKind::EnvelopeTombstone;
  journal.tombstone = std::move(entry);
  append(std::move(journal));
}

void JournalSession::stage_usage_commit(UsageDelta entry) {
  JournalEntry journal;
  journal.kind = JournalEntryKind::UsageCommit;
  journal.usage = std::move(entry);
  append(std::move(journal));
}

void JournalSession::stage_decision(DecisionRecord entry) {
  JournalEntry journal;
  journal.kind = JournalEntryKind::Decision;
  journal.decision = std::move(entry);
  append(std::move(journal));
}

void JournalSession::stage_observation(ObservationEntry entry) {
  JournalEntry journal;
  journal.kind = JournalEntryKind::Observation;
  journal.observation = std::move(entry);
  append(std::move(journal));
}

namespace {

std::string quote_for_diagnostic(const std::filesystem::path& path) {
  // Diagnostics name a file by its own name only: an absolute path would embed the
  // host layout in a message that is printed and persisted.
  return path.filename().string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Store implementation
// ---------------------------------------------------------------------------
// The implementation owns the file lock, the loaded manifest and the open segment
// handle, and it is the only place in the runtime that performs a durable write.
class Store::Impl {
 public:
  Impl(std::filesystem::path root_path, StoreOptions store_options)
      : root_(std::move(root_path)), options_(store_options) {}

  ~Impl() { close(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  [[nodiscard]] Status open();
  void close() noexcept;

  [[nodiscard]] Status read_snapshot(const std::function<Status(const StoreState&)>& body);
  [[nodiscard]] Status write_transaction(const std::function<Status(JournalSession&)>& body);
  [[nodiscard]] Status compact();
  [[nodiscard]] Status collect_garbage();

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] OpenMode mode() const noexcept { return options_.mode; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] std::uint64_t control_epoch() const noexcept { return manifest_.epoch; }
  [[nodiscard]] const Manifest& manifest() const noexcept { return manifest_; }
  [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_bytes_; }

 private:
  [[nodiscard]] std::filesystem::path manifest_path() const { return root_ / kManifestName; }
  [[nodiscard]] std::filesystem::path manifest_temp_path() const { return root_ / kManifestTempName; }
  [[nodiscard]] std::filesystem::path fence_path() const { return root_ / kFenceName; }
  [[nodiscard]] std::filesystem::path lock_path() const { return root_ / kLockName; }
  [[nodiscard]] std::filesystem::path segments_path() const { return root_ / kSegmentsDirectoryName; }

  [[nodiscard]] Status ensure_layout();
  [[nodiscard]] Status acquire(internal::LockMode mode);
  [[nodiscard]] Status create_store();
  [[nodiscard]] Status load_manifest();
  [[nodiscard]] Status verify_fence() const;
  [[nodiscard]] Status write_fence(std::uint64_t epoch, bool create_only);
  [[nodiscard]] Status publish_manifest(const Manifest& manifest);
  [[nodiscard]] Status scan_segments(std::vector<std::uint64_t>& epochs) const;
  [[nodiscard]] Status replay_into(StoreState& state, bool& from_snapshot);
  [[nodiscard]] Status read_snapshot_segment(std::uint64_t epoch, StoreState& state, std::uint64_t& frames,
                                            SnapshotMetadata& metadata) const;
  [[nodiscard]] Status read_delta_segment(std::uint64_t epoch, StoreState& state, std::uint64_t& frames,
                                          std::size_t& tail_bytes) const;
  [[nodiscard]] Status cut_segment_to_committed(std::uint64_t delta_epoch);
  [[nodiscard]] Status write_snapshot_segment(std::uint64_t epoch, const StoreState& state,
                                              std::uint64_t& frames);
  [[nodiscard]] Status remove_file_if_present(const std::filesystem::path& path, bool& removed) const;
  [[nodiscard]] Status refresh_total_bytes();

  std::filesystem::path root_;
  StoreOptions options_;
  internal::FileLock lock_;
  internal::FileHandle delta_file_;
  Manifest manifest_;
  RecoveryReport recovery_;
  std::uint64_t total_bytes_ = 0;
  std::uint64_t delta_epoch_ = 0;
  bool delta_open_ = false;
};

Status Store::Impl::ensure_layout() {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::status(root_, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return Status(StatusCode::StoreIoError, "the store directory could not be inspected");
  }
  if (std::filesystem::exists(status)) {
    if (!std::filesystem::is_directory(status)) {
      return Status(StatusCode::StoreIoError, "the store root exists but is not a directory");
    }
  } else {
    if (options_.mode != OpenMode::OpenOrCreate) {
      return Status(StatusCode::StoreNotFound, "no store exists at the supplied root");
    }
    std::filesystem::create_directories(root_, error);
    if (error) return Status(StatusCode::StoreIoError, "the store directory could not be created");
  }
  if (options_.mode != OpenMode::ReadOnly) {
    std::filesystem::create_directories(segments_path(), error);
    if (error) return Status(StatusCode::StoreIoError, "the segment directory could not be created");
  }
  return Status{};
}

Status Store::Impl::acquire(internal::LockMode mode) {
  const Result<internal::FileLock> acquired =
      internal::FileLock::acquire(lock_path(), mode, options_.lock.acquire_timeout_ms,
                                  options_.lock.retry_interval_ms);
  if (!acquired.ok()) return acquired.status();
  // FileLock is move-only: the temporary that holds the acquired handle transfers it
  // to the member through move assignment, which also releases any previous handle.
  lock_ = std::move(const_cast<internal::FileLock&>(acquired.value()));
  return Status{};
}

void Store::Impl::close() noexcept {
  delta_file_ = internal::FileHandle();
  delta_open_ = false;
  lock_ = internal::FileLock();
}

Status Store::Impl::write_fence(std::uint64_t epoch, bool create_only) {
  std::error_code error;
  if (create_only && std::filesystem::exists(fence_path(), error)) {
    return Status(StatusCode::StoreExists, "a store fence already exists");
  }
  const Bytes payload = encode_fence(epoch);
  Result<internal::FileHandle> handle = internal::FileHandle::create_truncate(fence_path());
  if (!handle.ok()) return handle.status();
  Status status = handle.value().write_at(0U, payload);
  if (!status.ok()) return status;
  status = handle.value().flush();
  if (!status.ok()) return status;
  std::vector<std::uint8_t> readback;
  status = handle.value().read_at(0U, payload.size(), readback);
  if (!status.ok()) return status;
  if (!constant_time_equal(readback, payload)) {
    return Status(StatusCode::StoreUnverified, "the epoch fence did not read back as written");
  }
  return Status{};
}

Status Store::Impl::verify_fence() const {
  Result<internal::FileHandle> handle = internal::FileHandle::open_read(fence_path());
  if (!handle.ok()) {
    return Status(StatusCode::StoreUnverified, "the epoch fence is missing from an existing store");
  }
  if (handle.value().size() != kFenceBytes) {
    return Status(StatusCode::StoreUnverified, "the epoch fence has an unexpected length");
  }
  std::vector<std::uint8_t> bytes;
  const Status status = handle.value().read_at(0U, kFenceBytes, bytes);
  if (!status.ok()) return status;
  std::uint64_t fence_epoch = 0;
  if (!parse_fence(bytes, fence_epoch)) {
    return Status(StatusCode::StoreUnverified, "the epoch fence failed its integrity check");
  }
  if (manifest_.epoch > fence_epoch) {
    // The manifest claims an epoch the fence never recorded. A store rewound to an
    // older manifest, or a manifest fabricated outside the protocol, lands here, so
    // the store is refused rather than opened at a rolled-back epoch.
    return Status(StatusCode::StoreRolledBack,
                  "the manifest epoch is ahead of the recorded epoch fence");
  }
  return Status{};
}

Status Store::Impl::load_manifest() {
  std::error_code error;
  if (!std::filesystem::exists(manifest_path(), error)) {
    return Status(StatusCode::StoreNotFound, "the store has no manifest");
  }
  Result<internal::FileHandle> handle = internal::FileHandle::open_read(manifest_path());
  if (!handle.ok()) return handle.status();
  const std::uint64_t length = handle.value().size();
  if (length == 0U || length > 4096U) {
    return Status(StatusCode::StoreCorrupt, "the manifest has an implausible length");
  }
  std::vector<std::uint8_t> bytes;
  const Status status = handle.value().read_at(0U, static_cast<std::size_t>(length), bytes);
  if (!status.ok()) return status;
  Result<Manifest> decoded = decode_manifest(bytes);
  if (!decoded.ok()) return decoded.status();
  manifest_ = decoded.value();
  return Status{};
}

Status Store::Impl::create_store() {
  Status status = write_fence(0U, true);
  if (!status.ok()) return status;
  Manifest manifest;
  manifest.layout_version = kStoreLayoutVersion;
  status = publish_manifest(manifest);
  if (!status.ok()) return status;
  manifest_ = manifest;
  recovery_.created = true;
  return Status{};
}

Status Store::Impl::publish_manifest(const Manifest& manifest) {
  const Bytes payload = canonical_manifest(manifest);
  Result<internal::FileHandle> temp = internal::FileHandle::create_truncate(manifest_temp_path());
  if (!temp.ok()) return temp.status();
  Status status = temp.value().write_at(0U, payload);
  if (!status.ok()) return status;
  status = temp.value().flush();
  if (!status.ok()) return status;
  std::vector<std::uint8_t> readback;
  status = temp.value().read_at(0U, payload.size(), readback);
  if (!status.ok()) return status;
  if (!constant_time_equal(readback, payload)) {
    return Status(StatusCode::StoreUnverified, "the staged manifest did not read back as written");
  }
  temp.value() = internal::FileHandle();
  // Publication. Everything the manifest names is already durable and verified, so this
  // replacement is the atomic commit point of the whole transaction.
  status = internal::replace_file(manifest_temp_path(), manifest_path());
  if (!status.ok()) return status;
  return internal::flush_directory(root_);
}

Status Store::Impl::scan_segments(std::vector<std::uint64_t>& epochs) const {
  epochs.clear();
  std::error_code error;
  if (!std::filesystem::exists(segments_path(), error)) return Status{};
  for (const std::filesystem::directory_entry& item :
       std::filesystem::directory_iterator(segments_path(), error)) {
    if (error) return Status(StatusCode::StoreIoError, "the segment directory could not be enumerated");
    if (!item.is_regular_file(error) || error) continue;
    const std::string name = item.path().filename().string();
    if (!ends_with(name, ".renv")) continue;
    std::uint64_t epoch = 0;
    if (!parse_segment_file_name(name, epoch)) {
      return Status(StatusCode::StoreCorrupt,
                    "the segment directory holds an unparseable segment name");
    }
    epochs.push_back(epoch);
  }
  std::sort(epochs.begin(), epochs.end());
  if (std::adjacent_find(epochs.begin(), epochs.end()) != epochs.end()) {
    return Status(StatusCode::StoreCorrupt, "two segments claim the same epoch");
  }
  if (epochs.size() > kMaxSegmentCount) {
    return Status(StatusCode::StoreLimitExceeded, "the store holds more segments than permitted");
  }
  return Status{};
}

Status Store::Impl::read_snapshot_segment(std::uint64_t epoch, StoreState& state, std::uint64_t& frames,
                                          SnapshotMetadata& metadata) const {
  frames = 0U;
  metadata = SnapshotMetadata{};
  const std::filesystem::path path = segments_path() / segment_file_name(epoch);
  Result<internal::FileHandle> handle = internal::FileHandle::open_read(path);
  if (!handle.ok()) {
    return Status(StatusCode::StoreCorrupt,
                  "the snapshot segment named by the manifest could not be opened");
  }
  const std::uint64_t length = handle.value().size();
  if (length < kSegmentHeaderBytes + kFrameHeaderBytes) {
    return Status(StatusCode::StoreCorrupt, "the snapshot segment is shorter than its header");
  }
  std::vector<std::uint8_t> header;
  Status status = handle.value().read_at(0U, kSegmentHeaderBytes, header);
  if (!status.ok()) return status;
  if (!magic_matches(header, kSegmentMagic) || get_u16(header, 8U) != kStoreFormatVersion) {
    return Status(StatusCode::StoreCorrupt, "the snapshot segment header is invalid");
  }
  if (get_u16(header, 10U) != kSegmentKindSnapshot || get_u64(header, 16U) != epoch) {
    return Status(StatusCode::StoreCorrupt, "the snapshot segment header disagrees with its name");
  }
  std::uint64_t offset = kSegmentHeaderBytes;
  std::uint64_t frames_read = 0U;
  while (offset < length) {
    const std::uint64_t remaining = length - offset;
    if (remaining < kFrameHeaderBytes) {
      return Status(StatusCode::StoreCorrupt, "the snapshot segment ends inside a frame header");
    }
    std::vector<std::uint8_t> frame_header;
    status = handle.value().read_at(offset, kFrameHeaderBytes, frame_header);
    if (!status.ok()) return status;
    if (!magic_matches(frame_header, kFrameMagic) || get_u16(frame_header, 8U) != kStoreFormatVersion) {
      return Status(StatusCode::StoreCorrupt, "a snapshot frame header is invalid");
    }
    if (get_u16(frame_header, 12U) != 0U || get_u16(frame_header, 14U) != 0U) {
      return Status(StatusCode::ReservedFieldNotZero, "a snapshot frame sets a reserved field");
    }
    const std::uint32_t payload_length = get_u32(frame_header, 16U);
    if (payload_length > kMaxFramePayloadBytes) {
      return Status(StatusCode::StoreLimitExceeded, "a snapshot frame declares an oversized payload");
    }
    if (remaining < kFrameHeaderBytes + payload_length) {
      return Status(StatusCode::StoreCorrupt, "the snapshot segment ends inside a frame payload");
    }
    std::vector<std::uint8_t> payload;
    status = handle.value().read_at(offset + kFrameHeaderBytes, payload_length, payload);
    if (!status.ok()) return status;
    if (crc32_ieee(payload) != get_u32(frame_header, 20U)) {
      return Status(StatusCode::StoreCorrupt, "a snapshot frame failed its payload checksum");
    }
    Digest::Value expected{};
    for (std::size_t index = 0; index < expected.size(); ++index) expected[index] = frame_header[40U + index];
    const Digest computed(
        sha256_domain(kDomainFrame, std::span<const std::uint8_t>(payload.data(), payload.size())));
    if (computed.value() != expected) {
      return Status(StatusCode::StoreIntegrityMismatch, "a snapshot frame failed its payload digest");
    }
    if (get_u16(frame_header, 10U) != static_cast<std::uint16_t>(JournalEntryKind::Snapshot)) {
      return Status(StatusCode::StoreCorrupt, "a snapshot segment holds a non-snapshot frame");
    }
    Result<DecodedSnapshot> decoded = decode_store_state(payload);
    if (!decoded.ok()) return decoded.status();
    state = std::move(decoded.value().state);
    metadata = decoded.value().metadata;
    ++frames_read;
    offset += kFrameHeaderBytes + payload_length;
  }
  if (frames_read == 0U) {
    return Status(StatusCode::StoreCorrupt, "the snapshot segment holds no frame");
  }
  frames = frames_read;
  return Status{};
}

Status Store::Impl::read_delta_segment(std::uint64_t epoch, StoreState& state, std::uint64_t& frames,
                                       std::size_t& tail_bytes) const {
  frames = 0U;
  tail_bytes = 0U;
  const std::filesystem::path path = segments_path() / segment_file_name(epoch);
  Result<internal::FileHandle> handle = internal::FileHandle::open_read(path);
  if (!handle.ok()) {
    return Status(StatusCode::StoreCorrupt,
                  "the delta segment named by the manifest could not be opened");
  }
  const std::uint64_t length = handle.value().size();
  if (length < kSegmentHeaderBytes) {
    return Status(StatusCode::StoreCorrupt, "the delta segment is shorter than its header");
  }
  std::vector<std::uint8_t> header;
  Status status = handle.value().read_at(0U, kSegmentHeaderBytes, header);
  if (!status.ok()) return status;
  if (!magic_matches(header, kSegmentMagic) || get_u16(header, 8U) != kStoreFormatVersion) {
    return Status(StatusCode::StoreCorrupt, "the delta segment header is invalid");
  }
  if (get_u16(header, 10U) != kSegmentKindDelta || get_u64(header, 16U) != epoch) {
    return Status(StatusCode::StoreCorrupt, "the delta segment header disagrees with its name");
  }
  const std::uint64_t baseline = get_u64(header, 24U);
  // The manifest is the commit point, so the segment may physically hold more bytes
  // than the manifest names. Anything past the committed length is an unpublished tail
  // from an interrupted commit, and it is never read as authority.
  //
  // The delta holds exactly the sequences above the one the snapshot covers, so the frame
  // count is committed_sequence minus covered_sequence. Counting snapshot *frames* instead
  // would be wrong for every compaction that folded more than one frame into the snapshot.
  const std::uint64_t committed_frames = manifest_.frames_committed - manifest_.snapshot_sequence;
  std::uint64_t offset = kSegmentHeaderBytes;
  std::uint64_t frames_read = 0U;
  std::uint64_t expected_sequence = manifest_.snapshot_sequence;
  while (frames_read < committed_frames) {
    const std::uint64_t remaining = length - offset;
    if (remaining < kFrameHeaderBytes) {
      return Status(StatusCode::StoreCorrupt, "a committed delta frame is missing from the segment");
    }
    std::vector<std::uint8_t> frame_header;
    status = handle.value().read_at(offset, kFrameHeaderBytes, frame_header);
    if (!status.ok()) return status;
    if (!magic_matches(frame_header, kFrameMagic) || get_u16(frame_header, 8U) != kStoreFormatVersion) {
      return Status(StatusCode::StoreCorrupt, "a delta frame header is invalid");
    }
    if (get_u16(frame_header, 12U) != 0U || get_u16(frame_header, 14U) != 0U) {
      return Status(StatusCode::ReservedFieldNotZero, "a delta frame sets a reserved field");
    }
    const std::uint32_t payload_length = get_u32(frame_header, 16U);
    if (payload_length > kMaxFramePayloadBytes) {
      return Status(StatusCode::StoreLimitExceeded, "a delta frame declares an oversized payload");
    }
    if (remaining < kFrameHeaderBytes + payload_length) {
      return Status(StatusCode::StoreCorrupt, "a committed delta frame is truncated");
    }
    std::vector<std::uint8_t> payload;
    status = handle.value().read_at(offset + kFrameHeaderBytes, payload_length, payload);
    if (!status.ok()) return status;
    if (crc32_ieee(payload) != get_u32(frame_header, 20U)) {
      return Status(StatusCode::StoreCorrupt, "a delta frame failed its payload checksum");
    }
    Digest::Value expected{};
    for (std::size_t index = 0; index < expected.size(); ++index) expected[index] = frame_header[40U + index];
    const Digest computed(
        sha256_domain(kDomainFrame, std::span<const std::uint8_t>(payload.data(), payload.size())));
    if (computed.value() != expected) {
      return Status(StatusCode::StoreIntegrityMismatch, "a delta frame failed its payload digest");
    }
    Result<JournalEntry> entry = decode_journal_entry(payload);
    if (!entry.ok()) return entry.status();
    if (entry.value().sequence < baseline) {
      return Status(StatusCode::StoreCorrupt, "a delta frame carries an out-of-range sequence");
    }
    // The journal sequence is one monotonic series dense across the snapshot and the delta,
    // so a gap is detected here instead of being tolerated: a missing frame would otherwise
    // be read as a shorter journal whose state digest happened not to be checked against it.
    if (entry.value().sequence != expected_sequence + 1U) {
      return Status(StatusCode::StoreCorrupt, "the delta segment does not continue the journal sequence");
    }
    expected_sequence = entry.value().sequence;
    const Status applied = apply_journal_entry(state, entry.value(), entry.value().sequence);
    if (!applied.ok()) return applied;
    state.journal.push_back(entry.value());
    state.state_sequence = entry.value().sequence;
    ++frames_read;
    offset += kFrameHeaderBytes + payload_length;
  }
  if (length > offset) tail_bytes = static_cast<std::size_t>(length - offset);
  frames = frames_read;
  return Status{};
}

Status Store::Impl::replay_into(StoreState& state, bool& from_snapshot) {
  state = StoreState();
  from_snapshot = false;
  recovery_.unreadable_tail_bytes = 0U;
  if (manifest_.snapshot_segment != 0U) {
    std::uint64_t frames = 0U;
    SnapshotMetadata metadata;
    const Status status = read_snapshot_segment(manifest_.snapshot_segment, state, frames, metadata);
    if (!status.ok()) return status;
    if (frames != manifest_.snapshot_frames) {
      return Status(StatusCode::StoreIntegrityMismatch,
                    "the snapshot holds a different frame count than the manifest records");
    }
    // The snapshot's own metadata must agree with the manifest that names it, so a segment
    // and a manifest from different generations cannot be combined into one state. The
    // sequence the snapshot covers is checked against the manifest and against the state it
    // carries, because it is what tells the reader which delta frames follow it.
    if (metadata.snapshot_epoch != manifest_.snapshot_segment ||
        metadata.compaction_count != manifest_.compaction_count) {
      return Status(StatusCode::StoreIntegrityMismatch,
                    "the snapshot metadata does not match the manifest that names it");
    }
    if (state.state_sequence != manifest_.snapshot_sequence) {
      return Status(StatusCode::StoreIntegrityMismatch,
                    "the snapshot covers a different journal sequence than the manifest records");
    }
    from_snapshot = true;
    recovery_.recovered_from_snapshot = true;
  }
  if (manifest_.delta_segment != 0U) {
    std::uint64_t frames = 0U;
    std::size_t tail = 0U;
    const Status status = read_delta_segment(manifest_.delta_segment, state, frames, tail);
    if (!status.ok()) return status;
    recovery_.unreadable_tail_bytes = tail;
  }
  // The compaction count is part of the state and is carried inside the snapshot, so it is
  // not re-derived here: the digest that follows has to be computed over the state exactly as
  // it was written, or a store would refuse a snapshot it produced itself.
  if (manifest_.snapshot_segment == 0U) state.compaction_count = manifest_.compaction_count;
  const Digest computed = store_state_digest(state);
  if (manifest_.state_digest.known() && manifest_.state_digest != computed) {
    // The recorded digest is a commitment to the exact state those frames produce. A
    // disagreement is corruption or tampering, and the store fails closed rather than
    // serving a state that no publication ever authorised.
    return Status(StatusCode::StoreIntegrityMismatch,
                  "the recovered state digest does not match the digest the manifest recorded");
  }
  if (options_.verify_chain_digest && manifest_.chain_digest.known()) {
    if (manifest_digest(manifest_) != manifest_.chain_digest) {
      return Status(StatusCode::StoreIntegrityMismatch,
                    "the manifest does not match the chain digest it records");
    }
  }
  recovery_.recovered_epoch = manifest_.epoch;
  recovery_.recovered_frames = manifest_.frames_committed;
  recovery_.state_digest = computed;
  recovery_.envelopes_loaded = state.envelopes.size();
  recovery_.decisions_loaded = state.decisions.size();
  recovery_.usage_entries_loaded = state.usage_entry_count;
  recovery_.observations_loaded = state.observations.size();
  return Status{};
}

Status Store::Impl::remove_file_if_present(const std::filesystem::path& path, bool& removed) const {
  removed = false;
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return Status{};
  if (!std::filesystem::is_regular_file(path, error)) return Status{};
  if (!std::filesystem::remove(path, error) || error) {
    return Status(StatusCode::StoreIoError, "a store file could not be removed");
  }
  removed = true;
  return Status{};
}

Status Store::Impl::refresh_total_bytes() {
  std::uint64_t total = 0U;
  std::error_code error;
  if (std::filesystem::exists(manifest_path(), error)) {
    const std::uintmax_t size = std::filesystem::file_size(manifest_path(), error);
    if (!error) total += size;
  }
  if (std::filesystem::exists(segments_path(), error)) {
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(segments_path(), error)) {
      if (error) break;
      if (!item.is_regular_file(error) || error) continue;
      const std::uintmax_t size = item.file_size(error);
      if (!error) total += size;
    }
  }
  total_bytes_ = total;
  if (total > kMaxTotalStoreBytes) {
    return Status(StatusCode::StoreLimitExceeded, "the store exceeds the maximum supported size");
  }
  return Status{};
}

Status Store::Impl::cut_segment_to_committed(std::uint64_t delta_epoch) {
  const std::uint64_t committed_delta_frames = manifest_.frames_committed - manifest_.snapshot_sequence;
  // A generation whose committed sequence is entirely covered by its snapshot has no delta at
  // all: there is nothing to cut, and the segment the manifest names as its snapshot must never
  // be opened for writing. Without this guard the pre-flight truncation runs against the
  // snapshot segment itself and destroys the generation the manifest is committed to.
  if (committed_delta_frames == 0U) {
    if (delta_epoch != 0U) {
      return Status(StatusCode::StoreCorrupt,
                    "the manifest accounts for every frame inside its snapshot yet names a delta segment");
    }
    return Status{};
  }
  if (delta_epoch == 0U) {
    return Status(StatusCode::StoreCorrupt, "the manifest commits delta frames without naming a delta segment");
  }
  const std::uint64_t epoch = delta_epoch;
  const std::filesystem::path path = segments_path() / segment_file_name(epoch);
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (committed_delta_frames != 0U) {
      return Status(StatusCode::StoreCorrupt, "the delta segment named by the manifest is absent");
    }
    delta_epoch_ = epoch;
    return Status{};
  }
  Result<internal::FileHandle> handle = internal::FileHandle::open_write_append(path);
  if (!handle.ok()) return handle.status();
  const std::uint64_t physical = handle.value().size();
  std::uint64_t consumed = kSegmentHeaderBytes;
  std::uint64_t counted = 0U;
  while (counted < committed_delta_frames) {
    std::vector<std::uint8_t> frame_header;
    const Status status = handle.value().read_at(consumed, kFrameHeaderBytes, frame_header);
    if (!status.ok()) return status;
    consumed += kFrameHeaderBytes + get_u32(frame_header, 16U);
    ++counted;
  }
  if (physical > consumed) {
    // Unpublished tail bytes are removed. They were never named by a manifest, so
    // removing them cannot retract anything that was ever published.
    const Status status = handle.value().truncate(consumed);
    if (!status.ok()) return status;
    const Status flushed = handle.value().flush();
    if (!flushed.ok()) return flushed;
    recovery_.unreadable_tail_bytes = physical - consumed;
  }
  delta_file_ = std::move(handle.value());
  delta_epoch_ = epoch;
  delta_open_ = true;
  return Status{};
}

Status Store::Impl::write_snapshot_segment(std::uint64_t epoch, const StoreState& state,
                                            std::uint64_t& frames) {
  frames = 0U;
  const Bytes state_payload = canonical_store_state(state);
  // The snapshot frame carries the sequence the snapshot covers, which is zero when the store
  // holds no journal entries at all.
  const std::uint64_t sequence = state.state_sequence;
  ByteWriter writer;
  writer.blob(state_payload);
  writer.u64(epoch);
  writer.u64(manifest_.epoch);
  writer.u64(static_cast<std::uint64_t>(state.envelopes.size()));
  writer.u64(state.revision_count);
  writer.u64(static_cast<std::uint64_t>(state.decisions.size()));
  writer.u64(state.usage_entry_count);
  writer.u64(static_cast<std::uint64_t>(state.observations.size()));
  writer.u64(state.compaction_count);
  const Bytes payload = writer.take();
  if (payload.size() > kMaxFramePayloadBytes) {
    return Status(StatusCode::StoreLimitExceeded, "the snapshot payload exceeds the maximum frame size");
  }
  const std::filesystem::path path = segments_path() / segment_file_name(epoch);
  Result<internal::FileHandle> handle = internal::FileHandle::create_truncate(path);
  if (!handle.ok()) return handle.status();
  const std::array<std::uint8_t, kSegmentHeaderBytes> segment_header =
      encode_segment_header(kSegmentKindSnapshot, epoch, sequence);
  Status status = handle.value().write_at(0U, segment_header);
  if (!status.ok()) return status;
  const Digest payload_digest =
      Digest(sha256_domain(kDomainFrame, std::span<const std::uint8_t>(payload.data(), payload.size())));
  const std::array<std::uint8_t, kFrameHeaderBytes> frame_header =
      encode_frame_header(static_cast<std::uint16_t>(JournalEntryKind::Snapshot),
                          static_cast<std::uint32_t>(payload.size()), crc32_ieee(payload), epoch,
                          sequence, payload_digest);
  status = handle.value().write_at(kSegmentHeaderBytes, frame_header);
  if (!status.ok()) return status;
  status = handle.value().write_at(kSegmentHeaderBytes + kFrameHeaderBytes, payload);
  if (!status.ok()) return status;
  status = handle.value().flush();
  if (!status.ok()) return status;
  // Read back and verify before the manifest is allowed to name this generation.
  std::vector<std::uint8_t> read_header;
  status = handle.value().read_at(0U, kSegmentHeaderBytes, read_header);
  if (!status.ok()) return status;
  if (!magic_matches(read_header, kSegmentMagic) ||
      get_u16(read_header, 10U) != kSegmentKindSnapshot || get_u64(read_header, 16U) != epoch) {
    return Status(StatusCode::StoreUnverified,
                  "the snapshot segment header did not read back as written");
  }
  std::vector<std::uint8_t> read_frame;
  status = handle.value().read_at(kSegmentHeaderBytes, kFrameHeaderBytes, read_frame);
  if (!status.ok()) return status;
  if (get_u32(read_frame, 16U) != static_cast<std::uint32_t>(payload.size()) ||
      get_u64(read_frame, 32U) != sequence) {
    return Status(StatusCode::StoreUnverified, "the snapshot frame header did not read back as written");
  }
  std::vector<std::uint8_t> read_payload;
  status = handle.value().read_at(kSegmentHeaderBytes + kFrameHeaderBytes,
                                  static_cast<std::size_t>(payload.size()), read_payload);
  if (!status.ok()) return status;
  if (!constant_time_equal(read_payload, payload)) {
    return Status(StatusCode::StoreUnverified, "the snapshot payload did not read back as written");
  }
  if (crc32_ieee(read_payload) != get_u32(read_frame, 20U)) {
    return Status(StatusCode::StoreUnverified, "the snapshot payload checksum did not verify");
  }
  frames = 1U;
  return Status{};
}

Status Store::Impl::open() {
  Status status = ensure_layout();
  if (!status.ok()) return status;
  // A read-only handle takes a shared lock and a writable handle takes an exclusive one, which
  // is what makes the store single-writer and multi-reader rather than single-handle.
  status = acquire(options_.mode == OpenMode::ReadOnly ? internal::LockMode::Shared
                                                       : internal::LockMode::Exclusive);
  if (!status.ok()) return status;
  std::error_code error;
  const bool manifest_present = std::filesystem::exists(manifest_path(), error) && !error;
  const bool fence_present = std::filesystem::exists(fence_path(), error) && !error;
  if (!manifest_present && !fence_present) {
    if (options_.mode != OpenMode::OpenOrCreate) {
      return Status(StatusCode::StoreNotFound, "no store was found at the supplied root");
    }
    status = create_store();
    if (!status.ok()) return status;
  } else if (!manifest_present) {
    // A fence without a manifest means publication was interrupted before the manifest
    // became authoritative. There is nothing to recover and the directory is not empty,
    // so the store is refused rather than started empty.
    return Status(StatusCode::StoreUnverified, "a store fence exists but no manifest does");
  } else {
    status = load_manifest();
    if (!status.ok()) return status;
    if (!fence_present) {
      if (manifest_.epoch != 0U || manifest_.frames_committed != 0U) {
        return Status(StatusCode::StoreUnverified,
                      "the epoch fence is missing from a published store");
      }
    } else {
      status = verify_fence();
      if (!status.ok()) return status;
    }
    StoreState ignored;
    bool from_snapshot = false;
    status = replay_into(ignored, from_snapshot);
    if (!status.ok()) return status;
    // A crash between the frame writes and the manifest publication leaves frames that
    // no publication named. They are removed so the segment holds exactly the committed
    // prefix, which is what makes the next commit verifiable.
    status = cut_segment_to_committed(manifest_.delta_segment);
    if (!status.ok()) return status;
  }
  if (options_.mode != OpenMode::ReadOnly && options_.remove_orphan_segments) {
    // A segment no manifest references is never authority: it is the residue of a commit that
    // died before it published. It is removed before anything can append to it, so no commit
    // can write after bytes that no publication ever named - which would leave the frames the
    // manifest counts starting at the wrong offset, and the store unreadable ever after.
    // A read-only handle collects nothing: it mutates nothing.
    status = collect_garbage();
    if (!status.ok()) return status;
  }
  if (options_.mode == OpenMode::ReadOnly) {
    // A reader keeps no write handle. Nothing in the read path writes, and holding no write
    // handle makes that a property of the process rather than of the code path. The shared lock
    // is kept for the lifetime of the handle, so a reader neither excludes another reader nor
    // has to acquire anything later.
    delta_file_ = internal::FileHandle();
    delta_open_ = false;
  }
  return refresh_total_bytes();
}

Status Store::Impl::read_snapshot(const std::function<Status(const StoreState&)>& body) {
  // The lock is acquired once, at open, and held for the lifetime of the handle: exclusively for
  // a writable handle and shared for a read-only one. It is never acquired twice, never upgraded
  // and never taken on demand, so no path can deadlock against itself and a reader never
  // excludes another reader.
  if (!lock_.held()) {
    const Status status = acquire(options_.mode == OpenMode::ReadOnly ? internal::LockMode::Shared
                                                                     : internal::LockMode::Exclusive);
    if (!status.ok()) return status;
  }
  StoreState state;
  bool from_snapshot = false;
  const Status status = replay_into(state, from_snapshot);
  if (!status.ok()) return status;
  return body(state);
}

// The status a fault-injected commit exits with. It is deliberately not a status the library
// ever returns, so a validation run can tell an injected abort from a refused operation.
constexpr int kCommitAbortExitCode = 3;

// A fault-injection directive used by the crash-recovery validation. It is a documented
// environment variable rather than a hidden hook: it only ever terminates the process, it never
// changes what is written or what is decided, and it is inert unless a caller sets it. It exists
// so that an interrupted commit can be produced on demand, at the exact stage boundary, instead
// of being approximated.
void terminate_at_commit_stage(const char* stage) noexcept {
  const char* const directive = std::getenv("RESOURCE_ENVELOPE_ABORT_AT");
  if (directive == nullptr) return;
  if (std::strcmp(directive, stage) != 0) return;
  std::_Exit(kCommitAbortExitCode);
}

Status Store::Impl::write_transaction(const std::function<Status(JournalSession&)>& body) {
  if (options_.mode == OpenMode::ReadOnly) {
    return Status(StatusCode::UnsupportedOperation,
                  "a read-only store handle cannot be written through");
  }
  if (!lock_.held()) {
    const Status status = acquire(internal::LockMode::Exclusive);
    if (!status.ok()) return status;
  }
  StoreState state;
  bool from_snapshot = false;
  Status status = replay_into(state, from_snapshot);
  if (!status.ok()) return status;

  const std::uint64_t base_sequence = state.state_sequence + 1U;
  JournalSession session(state, base_sequence, [&state](const JournalEntry& entry) {
    // Every staged record is applied to the in-memory state before anything is written,
    // so a transaction the store would refuse cannot reach the disk.
    const Status applied = apply_journal_entry(state, entry, entry.sequence);
    if (applied.ok()) state.journal.push_back(entry);
    return applied;
  });
  status = body(session);
  if (!status.ok()) return status;
  if (!session.failure().ok()) return session.failure();
  if (session.empty()) return Status{};
  state.state_sequence = session.next_sequence() - 1U;

  // Stage one: allocate the epoch and record it in the fence. The fence is always at
  // least as new as any published manifest, which is what makes a rewind detectable.
  if (manifest_.epoch == kStoreEpochMax) {
    return Status(StatusCode::StoreLimitExceeded, "the store control epoch is exhausted");
  }
  const std::uint64_t new_epoch = manifest_.epoch + 1U;
  status = write_fence(new_epoch, false);
  if (!status.ok()) return status;

  terminate_at_commit_stage("fence");
  // Stage two: append every frame to the delta segment.
  //
  // A generation that already names a delta appends to it, so a segment is only allocated for a
  // generation that has none: a store that has just been created, or one whose last commit was a
  // compaction covering every committed frame. The allocation is the commit epoch, which is
  // strictly above the manifest epoch and therefore above every generation the manifest names.
  std::uint64_t delta_epoch = manifest_.delta_segment;
  if (delta_epoch == 0U) {
    // This generation has no delta: either the store is new, or the last commit was a
    // compaction whose snapshot covers every committed frame. The segment is allocated at the
    // commit epoch, which is strictly above the manifest epoch and therefore above every
    // generation the manifest names. Deriving it from the snapshot instead - as an earlier
    // revision of this code did - names the snapshot segment itself, and appending delta
    // frames to it makes the store unreadable.
    delta_epoch = new_epoch;
  }
  if (!delta_open_ || delta_epoch_ != delta_epoch) {
    delta_file_ = internal::FileHandle();
    const std::filesystem::path path = segments_path() / segment_file_name(delta_epoch);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error) && !error;
    if (exists) {
      Result<internal::FileHandle> handle = internal::FileHandle::open_write_append(path);
      if (!handle.ok()) return handle.status();
      delta_file_ = std::move(handle.value());
    } else {
      Result<internal::FileHandle> created = internal::FileHandle::create_truncate(path);
      if (!created.ok()) return created.status();
      const std::array<std::uint8_t, kSegmentHeaderBytes> header =
          encode_segment_header(kSegmentKindDelta, delta_epoch, manifest_.snapshot_sequence + 1U);
      const Status written = created.value().write_at(0U, header);
      if (!written.ok()) return written;
      const Status flushed = created.value().flush();
      if (!flushed.ok()) return flushed;
      delta_file_ = std::move(created.value());
    }
    delta_epoch_ = delta_epoch;
    delta_open_ = true;
  }

  struct PendingFrame {
    std::uint64_t offset = 0U;
    Digest digest;
    std::uint32_t length = 0U;
  };
  std::vector<PendingFrame> pending;
  pending.reserve(session.appended().size());
  for (const JournalEntry& entry : session.appended()) {
    const Bytes payload = canonical_journal_entry(entry);
    if (payload.size() > kMaxFramePayloadBytes) {
      return Status(StatusCode::StoreLimitExceeded, "the record exceeds the maximum frame payload");
    }
    const Digest payload_digest =
        Digest(sha256_domain(kDomainFrame, std::span<const std::uint8_t>(payload.data(), payload.size())));
    const std::array<std::uint8_t, kFrameHeaderBytes> header =
        encode_frame_header(static_cast<std::uint16_t>(entry.kind),
                            static_cast<std::uint32_t>(payload.size()), crc32_ieee(payload), new_epoch,
                            entry.sequence, payload_digest);
    PendingFrame record;
    record.offset = delta_file_.size();
    record.length = static_cast<std::uint32_t>(payload.size());
    record.digest = payload_digest;
    status = delta_file_.write_at(record.offset, header);
    if (!status.ok()) return status;
    status = delta_file_.write_at(record.offset + kFrameHeaderBytes, payload);
    if (!status.ok()) return status;
    pending.push_back(record);
  }

  // Stage three: flush the segment and read every frame back through the same handle,
  // verifying the bytes that were actually written.
  terminate_at_commit_stage("frames");
  status = delta_file_.flush();
  if (!status.ok()) return status;
  for (const PendingFrame& record : pending) {
    std::vector<std::uint8_t> header;
    status = delta_file_.read_at(record.offset, kFrameHeaderBytes, header);
    if (!status.ok()) return status;
    if (!magic_matches(header, kFrameMagic) || get_u32(header, 16U) != record.length) {
      return Status(StatusCode::StoreUnverified, "a frame header did not read back as written");
    }
    std::vector<std::uint8_t> payload;
    status = delta_file_.read_at(record.offset + kFrameHeaderBytes, record.length, payload);
    if (!status.ok()) return status;
    if (crc32_ieee(payload) != get_u32(header, 20U)) {
      return Status(StatusCode::StoreUnverified, "a frame checksum did not read back as written");
    }
    Digest::Value stored{};
    for (std::size_t index = 0; index < stored.size(); ++index) stored[index] = header[40U + index];
    const Digest computed(
        sha256_domain(kDomainFrame, std::span<const std::uint8_t>(payload.data(), payload.size())));
    if (computed != record.digest || stored != record.digest.value()) {
      return Status(StatusCode::StoreUnverified, "a frame digest did not read back as written");
    }
  }

  // Stage four: publish the manifest. This replacement is the commit point.
  terminate_at_commit_stage("verify");
  Manifest next = manifest_;
  next.epoch = new_epoch;
  next.state_epoch = new_epoch;
  next.state_sequence = state.state_sequence;
  next.delta_segment = delta_epoch;
  terminate_at_commit_stage("publish");
  next.frames_committed = state.state_sequence;
  next.envelope_count = state.envelopes.size();
  next.revision_count = state.revision_count;
  next.decision_count = state.decisions.size();
  next.usage_entry_count = state.usage_entry_count;
  next.observation_count = state.observations.size();
  next.compaction_count = state.compaction_count;
  next.state_digest = store_state_digest(state);
  next.chain_digest = manifest_digest(next);
  status = publish_manifest(next);
  if (!status.ok()) return status;
  manifest_ = next;
  return refresh_total_bytes();
}

Status Store::Impl::collect_garbage() {
  std::error_code error;
  bool removed = false;
  Status status = remove_file_if_present(manifest_temp_path(), removed);
  if (!status.ok()) return status;
  recovery_.temp_files_removed += removed ? 1U : 0U;

  std::vector<std::uint64_t> epochs;
  status = scan_segments(epochs);
  if (!status.ok()) return status;
  for (const std::uint64_t epoch : epochs) {
    if (epoch == manifest_.snapshot_segment || epoch == manifest_.delta_segment) continue;
    // An unreferenced segment is never read as authority. Under a read-only handle it
    // is reported and left alone, because removing it would mutate the store.
    if (options_.mode == OpenMode::ReadOnly) {
      ++recovery_.orphan_segments;
      continue;
    }
    status = remove_file_if_present(segments_path() / segment_file_name(epoch), removed);
    if (!status.ok()) return status;
    if (removed) ++recovery_.orphan_segments;
  }
  return Status{};
}

Status Store::Impl::compact() {
  if (options_.mode == OpenMode::ReadOnly) {
    return Status(StatusCode::UnsupportedOperation, "a read-only store handle cannot compact");
  }
  if (!lock_.held()) {
    const Status status = acquire(internal::LockMode::Exclusive);
    if (!status.ok()) return status;
  }
  StoreState state;
  bool from_snapshot = false;
  Status status = replay_into(state, from_snapshot);
  if (!status.ok()) return status;
  delta_file_ = internal::FileHandle();
  delta_open_ = false;
  ++state.compaction_count;
  // The journal sequence is not advanced by a compaction. It is left exactly as it was, including
  // zero for a store that holds no records at all, because a rewrite that changed a counter would
  // change the state commitment of a state that did not change.
  if (manifest_.epoch == kStoreEpochMax) {
    return Status(StatusCode::StoreLimitExceeded, "the store control epoch is exhausted");
  }
  const std::uint64_t new_epoch = manifest_.epoch + 1U;
  status = write_fence(new_epoch, false);
  if (!status.ok()) return status;
  std::uint64_t frames = 0U;
  status = write_snapshot_segment(new_epoch, state, frames);
  if (!status.ok()) return status;
  Manifest next = manifest_;
  next.epoch = new_epoch;
  next.state_epoch = new_epoch;
  next.snapshot_segment = new_epoch;
  next.snapshot_frames = frames;
  next.snapshot_sequence = state.state_sequence;
  next.delta_segment = 0U;
  next.frames_committed = state.state_sequence;
  next.state_sequence = state.state_sequence;
  next.envelope_count = state.envelopes.size();
  next.revision_count = state.revision_count;
  next.decision_count = state.decisions.size();
  next.usage_entry_count = state.usage_entry_count;
  next.observation_count = state.observations.size();
  next.compaction_count = state.compaction_count;
  next.state_digest = store_state_digest(state);
  next.chain_digest = manifest_digest(next);
  status = publish_manifest(next);
  if (!status.ok()) return status;
  const std::uint64_t previous_snapshot = manifest_.snapshot_segment;
  const std::uint64_t previous_delta = manifest_.delta_segment;
  manifest_ = next;
  // The snapshot epoch is not a delta segment, so no open delta is carried over: the next
  // append allocates one of its own.
  delta_open_ = false;
  delta_epoch_ = 0U;

  // Superseded segments are removed only after the manifest that stops naming them is
  // durable. A crash before this point leaves them present and unreferenced, which the
  // next open collects.
  std::vector<std::uint64_t> epochs;
  status = scan_segments(epochs);
  if (!status.ok()) return status;
  for (const std::uint64_t epoch : epochs) {
    if (epoch == new_epoch || epoch == previous_snapshot || epoch == previous_delta) continue;
    bool removed = false;
    status = remove_file_if_present(segments_path() / segment_file_name(epoch), removed);
    if (!status.ok()) return status;
  }
  status = collect_garbage();

  if (!status.ok()) return status;
  return refresh_total_bytes();
}

// ---------------------------------------------------------------------------
// Public store handle
// ---------------------------------------------------------------------------
Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::open(const std::filesystem::path& root, const StoreOptions& options) {
  if (root.empty()) {
    return Status(StatusCode::InvalidArgument, "a store root path is required");
  }
  if (options.lock.retry_interval_ms == 0U) {
    return Status(StatusCode::InvalidArgument, "the lock retry interval must be positive");
  }
  auto impl = std::make_unique<Impl>(root, options);
  const Status status = impl->open();
  if (!status.ok()) return status;
  return std::unique_ptr<Store>(new Store(std::move(impl)));
}

const std::filesystem::path& Store::root() const noexcept { return impl_->root(); }

OpenMode Store::mode() const noexcept { return impl_->mode(); }

const RecoveryReport& Store::recovery() const noexcept { return impl_->recovery(); }

std::uint64_t Store::control_epoch() const noexcept { return impl_->control_epoch(); }

const Manifest& Store::manifest() const noexcept { return impl_->manifest(); }

std::uint64_t Store::total_bytes() const noexcept { return impl_->total_bytes(); }

Status Store::with_snapshot(const std::function<Status(const StoreState&)>& body) const {
  return impl_->read_snapshot(body);
}

Status Store::with_writer(const std::function<Status(JournalSession&)>& body) {
  return impl_->write_transaction(body);
}

Status Store::compact() { return impl_->compact(); }

Status Store::collect_garbage() { return impl_->collect_garbage(); }

}  // namespace resource_envelope
