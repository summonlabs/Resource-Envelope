#include "resource_envelope/canonical.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "resource_envelope/service.hpp"
#include "resource_envelope/text.hpp"

namespace resource_envelope {
namespace {

// Small encoding primitives shared by every canonical form. Every variable-size
// value is length-prefixed, so a byte stream can never be re-parsed into a
// different logical value.
void put_optional_nanounits(ByteWriter& writer, const std::optional<Nanounits>& value) {
  writer.bool_value(value.has_value());
  if (value.has_value()) writer.u64(*value);
}

std::optional<Nanounits> get_optional_nanounits(ByteReader& reader) {
  const bool present = reader.bool_value();
  if (!reader.ok()) return std::nullopt;
  if (!present) return std::nullopt;
  const Nanounits value = reader.u64();
  if (!reader.ok()) return std::nullopt;
  return value;
}

void put_optional_timestamp(ByteWriter& writer, const std::optional<Timestamp>& value) {
  writer.bool_value(value.has_value());
  if (value.has_value()) writer.i64(*value);
}

bool get_optional_timestamp(ByteReader& reader, std::optional<Timestamp>& out) {
  const bool present = reader.bool_value();
  if (!reader.ok()) return false;
  if (!present) {
    out.reset();
    return true;
  }
  const Timestamp value = reader.i64();
  if (!reader.ok()) return false;
  out = value;
  return true;
}

void put_measure_status(ByteWriter& writer, MeasureStatus status) { writer.u8(static_cast<std::uint8_t>(status)); }

bool get_measure_status(ByteReader& reader, MeasureStatus& out) {
  const std::uint8_t raw = reader.u8();
  if (!reader.ok()) return false;
  if (raw > static_cast<std::uint8_t>(MeasureStatus::Measured)) return false;
  out = static_cast<MeasureStatus>(raw);
  return true;
}

constexpr std::size_t kMaxTextBytes = 4096;
constexpr std::size_t kMaxTokenBytes = 256;

}  // namespace

void encode_digest(ByteWriter& writer, const Digest& digest) {
  writer.bool_value(digest.known());
  if (digest.known()) {
    writer.raw(std::span<const std::uint8_t>(digest.value().data(), digest.value().size()));
  }
}

bool decode_digest(ByteReader& reader, Digest& out) {
  const bool known = reader.bool_value();
  if (!reader.ok()) return false;
  if (!known) {
    out = Digest::unknown();
    return true;
  }
  const Bytes raw = reader.raw(32);
  if (!reader.ok()) return false;
  Digest::Value value{};
  for (std::size_t index = 0; index < value.size(); ++index) value[index] = raw[index];
  out = Digest(value);
  return true;
}

void encode_identity_ref(ByteWriter& writer, const IdentityRef& identity) {
  writer.text(identity.id);
  writer.u64(identity.generation);
}

void encode_envelope_ref(ByteWriter& writer, const EnvelopeRef& reference) {
  writer.text(reference.envelope_id);
  writer.u64(reference.revision);
  encode_digest(writer, reference.digest);
}

void encode_provenance(ByteWriter& writer, const Provenance& provenance) {
  writer.text(provenance.authority);
  writer.text(provenance.actor);
  writer.text(provenance.reason);
  writer.i64(provenance.declared_at);
}

void encode_dimension_spec(ByteWriter& writer, const DimensionSpec& spec) {
  writer.u8(static_cast<std::uint8_t>(spec.kind));
  put_optional_nanounits(writer, spec.hard_limit);
  writer.u64(spec.reserved);
  writer.u64(spec.quantum);
  writer.text(spec.compatibility_class);
  writer.u8(static_cast<std::uint8_t>(spec.indexing));
  writer.bool_value(spec.committed.has_value());
  if (spec.committed.has_value()) {
    writer.u64(spec.committed->value);
    writer.i64(spec.committed->observed_at);
    writer.text(spec.committed->source);
  }
}

Result<DimensionSpec> decode_dimension_spec(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalDimspecBytes) {
    return Status(StatusCode::OutOfRange, "dimension spec exceeds the canonical size bound");
  }
  ByteReader reader(data);
  DimensionSpec spec;
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
    return Status(StatusCode::InvalidEnumValue, "dimension kind is not a defined value");
  }
  spec.kind = static_cast<DimensionKind>(kind);
  spec.hard_limit = get_optional_nanounits(reader);
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension limit is truncated");
  spec.reserved = reader.u64();
  spec.quantum = reader.u64();
  spec.compatibility_class = reader.text(kCompatibilityClassMaxLength);
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension spec text is truncated");
  const std::uint8_t indexing = reader.u8();
  if (!reader.ok() || indexing > static_cast<std::uint8_t>(DimensionIndexing::PerPrincipal)) {
    return Status(StatusCode::InvalidEnumValue, "dimension indexing is not a defined value");
  }
  spec.indexing = static_cast<DimensionIndexing>(indexing);
  const bool has_committed = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension committed flag is truncated");
  if (has_committed) {
    CommittedUsage committed;
    committed.status = MeasureStatus::Measured;
    committed.value = reader.u64();
    committed.observed_at = reader.i64();
    committed.source = reader.text(kMaxTokenBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "committed usage is truncated");
    spec.committed = std::move(committed);
  }
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "dimension spec did not decode exactly");
  }
  return spec;
}

Bytes canonical_dimension_spec(const DimensionSpec& spec) {
  ByteWriter writer;
  encode_dimension_spec(writer, spec);
  return writer.take();
}

void encode_dimension_request(ByteWriter& writer, const DimensionRequest& request) {
  writer.u8(static_cast<std::uint8_t>(request.kind));
  writer.u64(request.quantity);
  writer.u32(request.principals);
  writer.text(request.compatibility_class);
  writer.text(request.compatibility_classes);
  writer.text(request.principal);
  writer.u32(request.level);
  writer.bool_value(request.operational_spare.has_value());
  if (request.operational_spare.has_value()) writer.bool_value(*request.operational_spare);
}

Result<DimensionRequest> decode_dimension_request(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalDimspecBytes) {
    return Status(StatusCode::OutOfRange, "dimension request exceeds the canonical size bound");
  }
  ByteReader reader(data);
  DimensionRequest request;
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
    return Status(StatusCode::InvalidEnumValue, "dimension kind is not a defined value");
  }
  request.kind = static_cast<DimensionKind>(kind);
  request.quantity = reader.u64();
  request.principals = reader.u32();
  request.compatibility_class = reader.text(kCompatibilityClassMaxLength);
  request.compatibility_classes = reader.text(kMaxTextBytes);
  request.principal = reader.text(kMaxPrincipalTokenLength);
  request.level = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension request is truncated");
  const bool has_spare = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "operational spare flag is truncated");
  if (has_spare) {
    const bool spare = reader.bool_value();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "operational spare is truncated");
    request.operational_spare = spare;
  }
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "dimension request did not decode exactly");
  }
  return request;
}

Bytes canonical_dimension_request(const DimensionRequest& request) {
  ByteWriter writer;
  encode_dimension_request(writer, request);
  return writer.take();
}

void encode_evidence(ByteWriter& writer, const Evidence& evidence) {
  writer.u8(static_cast<std::uint8_t>(evidence.kind));
  writer.bool_value(evidence.observation.has_value());
  if (evidence.observation.has_value()) {
    put_measure_status(writer, evidence.observation->status);
    writer.u64(evidence.observation->value);
    writer.i64(evidence.observation->observed_at);
    writer.text(evidence.observation->source);
  }
  writer.bool_value(evidence.committed.has_value());
  if (evidence.committed.has_value()) {
    put_measure_status(writer, evidence.committed->status);
    writer.u64(evidence.committed->value);
    writer.i64(evidence.committed->observed_at);
    writer.text(evidence.committed->source);
  }
  writer.bool_value(evidence.exclusive.has_value());
  if (evidence.exclusive.has_value()) {
    put_measure_status(writer, evidence.exclusive->status);
    writer.text(evidence.exclusive->compatibility_class);
    writer.u32(evidence.exclusive->holders);
    writer.text(evidence.exclusive->holder_identity);
    writer.i64(evidence.exclusive->observed_at);
    writer.text(evidence.exclusive->source);
  }
  writer.bool_value(evidence.threshold.has_value());
  if (evidence.threshold.has_value()) {
    put_measure_status(writer, evidence.threshold->status);
    writer.u64(evidence.threshold->required);
    writer.i64(evidence.threshold->observed_at);
    writer.text(evidence.threshold->source);
  }
}

Result<Evidence> decode_evidence(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalEvidenceBytes) {
    return Status(StatusCode::OutOfRange, "evidence item exceeds the canonical size bound");
  }
  ByteReader reader(data);
  Evidence evidence;
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
    return Status(StatusCode::InvalidEnumValue, "evidence dimension kind is not a defined value");
  }
  evidence.kind = static_cast<DimensionKind>(kind);

  if (reader.bool_value()) {
    Measurement observation;
    if (!get_measure_status(reader, observation.status)) {
      return Status(StatusCode::InvalidEnumValue, "observation status is not a defined value");
    }
    observation.value = reader.u64();
    observation.observed_at = reader.i64();
    observation.source = reader.text(kMaxTokenBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "observation is truncated");
    evidence.observation = std::move(observation);
  }
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "evidence observation flag is truncated");

  if (reader.bool_value()) {
    CommittedUsage committed;
    if (!get_measure_status(reader, committed.status)) {
      return Status(StatusCode::InvalidEnumValue, "committed status is not a defined value");
    }
    committed.value = reader.u64();
    committed.observed_at = reader.i64();
    committed.source = reader.text(kMaxTokenBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "committed usage is truncated");
    evidence.committed = std::move(committed);
  }
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "evidence committed flag is truncated");

  if (reader.bool_value()) {
    ExclusiveDeclaration exclusive;
    if (!get_measure_status(reader, exclusive.status)) {
      return Status(StatusCode::InvalidEnumValue, "exclusive status is not a defined value");
    }
    exclusive.compatibility_class = reader.text(kCompatibilityClassMaxLength);
    exclusive.holders = reader.u32();
    exclusive.holder_identity = reader.text(kMaxPrincipalTokenLength);
    exclusive.observed_at = reader.i64();
    exclusive.source = reader.text(kMaxTokenBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "exclusive declaration is truncated");
    evidence.exclusive = std::move(exclusive);
  }
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "evidence exclusive flag is truncated");

  if (reader.bool_value()) {
    ThresholdDeclaration threshold;
    if (!get_measure_status(reader, threshold.status)) {
      return Status(StatusCode::InvalidEnumValue, "threshold status is not a defined value");
    }
    threshold.required = reader.u64();
    threshold.observed_at = reader.i64();
    threshold.source = reader.text(kMaxTokenBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "threshold declaration is truncated");
    evidence.threshold = std::move(threshold);
  }
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "evidence threshold flag is truncated");

  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "evidence item did not decode exactly");
  }
  return evidence;
}

Bytes canonical_evidence(const Evidence& evidence) {
  ByteWriter writer;
  encode_evidence(writer, evidence);
  return writer.take();
}

Bytes canonical_identity(const IdentityRef& identity) {
  ByteWriter writer;
  encode_identity_ref(writer, identity);
  return writer.take();
}

Bytes canonical_scope(const EnvelopeScope& scope) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(scope.kind));
  encode_identity_ref(writer, scope.identity);
  return writer.take();
}

Bytes canonical_evidence_set(const EvidenceSet& set) {
  // The evidence set is canonically ordered by dimension kind, then by the
  // canonical encoding of the item itself. Reordering the same items therefore
  // produces the same digest, while adding or changing an item does not.
  std::vector<Bytes> items;
  items.reserve(set.items.size());
  for (const Evidence& item : set.items) items.push_back(canonical_evidence(item));
  std::sort(items.begin(), items.end());
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(items.size()));
  for (const Bytes& item : items) writer.blob(item);
  return writer.take();
}

Result<EvidenceSet> decode_evidence_set(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalRequestBytes) {
    return Status(StatusCode::OutOfRange, "evidence set exceeds the canonical size bound");
  }
  ByteReader reader(data);
  EvidenceSet set;
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "evidence set header is truncated");
  if (count > kMaxEvidencePerRequest) {
    return Status(StatusCode::OutOfRange, "evidence set declares more items than permitted");
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const Bytes item = reader.blob(kMaxCanonicalEvidenceBytes);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "evidence item is truncated");
    Result<Evidence> decoded = decode_evidence(item);
    if (!decoded.ok()) return decoded.status();
    set.items.push_back(std::move(decoded.value()));
  }
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "evidence set did not decode exactly");
  }
  return set;
}

void encode_envelope(ByteWriter& writer, const Envelope& envelope) {
  writer.text(envelope.id);
  writer.u64(envelope.revision);
  writer.bool_value(envelope.supersedes.has_value());
  if (envelope.supersedes.has_value()) writer.text(*envelope.supersedes);
  writer.u8(static_cast<std::uint8_t>(envelope.kind));
  writer.u8(static_cast<std::uint8_t>(envelope.scope.kind));
  encode_identity_ref(writer, envelope.scope.identity);
  writer.text(envelope.site_id);
  encode_digest(writer, envelope.identity_digest);
  encode_digest(writer, envelope.service_class_digest);
  encode_digest(writer, envelope.policy_digest);
  put_optional_timestamp(writer, envelope.window.effective_from);
  put_optional_timestamp(writer, envelope.window.effective_until);
  writer.bool_value(envelope.require_binding_confirmation);

  // Precedence list.
  writer.u32(static_cast<std::uint32_t>(envelope.precedence.size()));
  for (const DimensionKind kind : envelope.precedence) writer.u8(static_cast<std::uint8_t>(kind));

  // Lineage, sorted by identifier then revision. A composition lists its sources in a
  // canonical order so that the same composition always produces the same bytes and
  // therefore the same digest, whatever order the sources were supplied in.
  std::vector<EnvelopeRef> lineage = envelope.merged_from;
  std::sort(lineage.begin(), lineage.end(), [](const EnvelopeRef& left, const EnvelopeRef& right) {
    if (left.envelope_id != right.envelope_id) return left.envelope_id < right.envelope_id;
    return left.revision < right.revision;
  });
  writer.u32(static_cast<std::uint32_t>(lineage.size()));
  for (const EnvelopeRef& reference : lineage) encode_envelope_ref(writer, reference);

  // Policy in force for composition.
  writer.u8(static_cast<std::uint8_t>(envelope.merge_policy));

  // Dimensions, sorted by kind so that declaration order cannot change the digest.
  std::vector<const DimensionSpec*> ordered;
  ordered.reserve(envelope.dimensions.size());
  for (const DimensionSpec& spec : envelope.dimensions) ordered.push_back(&spec);
  std::sort(ordered.begin(), ordered.end(), [](const DimensionSpec* left, const DimensionSpec* right) {
    return static_cast<std::uint8_t>(left->kind) < static_cast<std::uint8_t>(right->kind);
  });
  writer.u32(static_cast<std::uint32_t>(ordered.size()));
  for (const DimensionSpec* spec : ordered) encode_dimension_spec(writer, *spec);

  // Provenance.
  encode_provenance(writer, envelope.provenance);
}

Result<Envelope> decode_envelope(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalEnvelopeBytes) {
    return Status(StatusCode::OutOfRange, "envelope exceeds the canonical size bound");
  }
  ByteReader reader(data);
  Envelope envelope;

  envelope.id = reader.text(kIdentifierMaxLength);
  envelope.revision = reader.u64();
  const bool has_supersedes = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "envelope header is truncated");
  if (has_supersedes) {
    envelope.supersedes = reader.text(kIdentifierMaxLength);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "envelope supersedes is truncated");
  }
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || kind > static_cast<std::uint8_t>(EnvelopeKind::Composite)) {
    return Status(StatusCode::InvalidEnumValue, "envelope kind is not a defined value");
  }
  envelope.kind = static_cast<EnvelopeKind>(kind);
  const std::uint8_t scope_kind = reader.u8();
  if (!reader.ok() || scope_kind > static_cast<std::uint8_t>(EnvelopeScopeKind::Facility)) {
    return Status(StatusCode::InvalidEnumValue, "scope kind is not a defined value");
  }
  envelope.scope.kind = static_cast<EnvelopeScopeKind>(scope_kind);
  envelope.scope.identity.id = reader.text(kIdentifierMaxLength);
  envelope.scope.identity.generation = reader.u64();
  envelope.site_id = reader.text(kIdentifierMaxLength);
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "envelope scope is truncated");
  if (!decode_digest(reader, envelope.identity_digest)) return Status(StatusCode::TruncatedPayload, "identity digest is truncated");
  if (!decode_digest(reader, envelope.service_class_digest)) return Status(StatusCode::TruncatedPayload, "service class digest is truncated");
  if (!decode_digest(reader, envelope.policy_digest)) return Status(StatusCode::TruncatedPayload, "policy digest is truncated");
  if (!get_optional_timestamp(reader, envelope.window.effective_from)) {
    return Status(StatusCode::TruncatedPayload, "effective window start is truncated");
  }
  if (!get_optional_timestamp(reader, envelope.window.effective_until)) {
    return Status(StatusCode::TruncatedPayload, "effective window end is truncated");
  }
  envelope.require_binding_confirmation = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "binding confirmation flag is truncated");

  const std::uint32_t precedence_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "precedence count is truncated");
  if (precedence_count > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "precedence list exceeds the permitted size");
  }
  for (std::uint32_t index = 0; index < precedence_count; ++index) {
    const std::uint8_t raw = reader.u8();
    if (!reader.ok() || !is_valid(static_cast<DimensionKind>(raw))) {
      return Status(StatusCode::InvalidEnumValue, "precedence entry is not a defined dimension kind");
    }
    envelope.precedence.push_back(static_cast<DimensionKind>(raw));
  }

  const std::uint32_t lineage_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "lineage count is truncated");
  if (lineage_count > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "lineage list exceeds the permitted size");
  }
  for (std::uint32_t index = 0; index < lineage_count; ++index) {
    EnvelopeRef reference;
    reference.envelope_id = reader.text(kIdentifierMaxLength);
    reference.revision = reader.u64();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "lineage entry is truncated");
    if (!decode_digest(reader, reference.digest)) return Status(StatusCode::TruncatedPayload, "lineage digest is truncated");
    envelope.merged_from.push_back(std::move(reference));
  }

  const std::uint8_t merge_policy = reader.u8();
  if (!reader.ok() || merge_policy > static_cast<std::uint8_t>(MergePolicy::TightestWins)) {
    return Status(StatusCode::InvalidEnumValue, "merge policy is not a defined value");
  }
  envelope.merge_policy = static_cast<MergePolicy>(merge_policy);

  const std::uint32_t dimension_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension count is truncated");
  if (dimension_count > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "dimension list exceeds the permitted size");
  }
  for (std::uint32_t index = 0; index < dimension_count; ++index) {
    const std::size_t begin = reader.offset();
    DimensionSpec spec;
    const std::uint8_t raw_kind = reader.u8();
    if (!reader.ok() || !is_valid(static_cast<DimensionKind>(raw_kind))) {
      return Status(StatusCode::InvalidEnumValue, "dimension kind is not a defined value");
    }
    spec.kind = static_cast<DimensionKind>(raw_kind);
    spec.hard_limit = get_optional_nanounits(reader);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension limit is truncated");
    spec.reserved = reader.u64();
    spec.quantum = reader.u64();
    spec.compatibility_class = reader.text(kCompatibilityClassMaxLength);
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension text is truncated");
    const std::uint8_t indexing = reader.u8();
    if (!reader.ok() || indexing > static_cast<std::uint8_t>(DimensionIndexing::PerPrincipal)) {
      return Status(StatusCode::InvalidEnumValue, "dimension indexing is not a defined value");
    }
    spec.indexing = static_cast<DimensionIndexing>(indexing);
    const bool has_committed = reader.bool_value();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension committed flag is truncated");
    if (has_committed) {
      CommittedUsage committed;
      committed.status = MeasureStatus::Measured;
      committed.value = reader.u64();
      committed.observed_at = reader.i64();
      committed.source = reader.text(kMaxTokenBytes);
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "committed usage is truncated");
      spec.committed = std::move(committed);
    }
    (void)(begin);
    for (const DimensionSpec& existing : envelope.dimensions) {
      if (existing.kind == spec.kind) {
        return Status(StatusCode::DuplicateDimension, "envelope declares the same dimension twice");
      }
    }
    envelope.dimensions.push_back(std::move(spec));
  }

  envelope.provenance.authority = reader.text(kMaxTokenBytes);
  envelope.provenance.actor = reader.text(kMaxTokenBytes);
  envelope.provenance.reason = reader.text(kMaxTextBytes);
  envelope.provenance.declared_at = reader.i64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "provenance is truncated");

  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "envelope did not decode exactly");
  }
  return envelope;
}

Bytes canonical_envelope(const Envelope& envelope) {
  ByteWriter writer;
  encode_envelope(writer, envelope);
  return writer.take();
}

// The request body is the digest-bearing projection of an evaluation request.
// Every field that can change a decision is included. The evidence set is digested
// separately, so the recorded request identity stays stable when only evidence
// changes, and the idempotency key is included because it is part of the recorded
// request identity.
void encode_evaluation_request_body(ByteWriter& writer, const EvaluationRequest& request) {
  writer.u8(static_cast<std::uint8_t>(request.scope.kind));
  encode_identity_ref(writer, request.scope.identity);
  writer.text(request.idempotency_key);
  writer.i64(request.at);
  writer.bool_value(request.expected_envelope_revision.has_value());
  if (request.expected_envelope_revision.has_value()) writer.u64(*request.expected_envelope_revision);
  writer.bool_value(request.expected_envelope_digest.has_value());
  if (request.expected_envelope_digest.has_value()) encode_digest(writer, *request.expected_envelope_digest);

  // The identity binding always encodes the generation, including the value zero,
  // so that "no snapshot supplied" can never be read as "generation zero
  // confirmed".
  encode_identity_ref(writer, request.identity.identity);
  encode_digest(writer, request.identity.digest);
  writer.u8(static_cast<std::uint8_t>(request.service_class.state));
  encode_digest(writer, request.service_class.digest);
  writer.u8(static_cast<std::uint8_t>(request.policy.state));
  encode_digest(writer, request.policy.digest);

  std::vector<std::string> classes = request.accepted_classes;
  std::sort(classes.begin(), classes.end());
  classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
  writer.u32(static_cast<std::uint32_t>(classes.size()));
  for (const std::string& token : classes) writer.text(token);

  // Requested dimensions in canonical kind order.
  std::vector<const DimensionRequest*> ordered;
  ordered.reserve(request.dimensions.size());
  for (const DimensionRequest& item : request.dimensions) ordered.push_back(&item);
  std::sort(ordered.begin(), ordered.end(),
            [](const DimensionRequest* left, const DimensionRequest* right) {
              return static_cast<std::uint8_t>(left->kind) < static_cast<std::uint8_t>(right->kind);
            });
  writer.u32(static_cast<std::uint32_t>(ordered.size()));
  for (const DimensionRequest* item : ordered) encode_dimension_request(writer, *item);

  writer.u8(static_cast<std::uint8_t>(request.facility_composition));
  writer.text(request.facility_envelope_id);
}

Bytes canonical_evaluation_request_body(const EvaluationRequest& request) {
  ByteWriter writer;
  encode_evaluation_request_body(writer, request);
  return writer.take();
}

Digest request_digest(const EvaluationRequest& request) noexcept {
  const Bytes body = canonical_evaluation_request_body(request);
  return Digest(sha256_domain(kDomainRequest, std::span<const std::uint8_t>(body.data(), body.size())));
}

Digest evidence_digest(const EvidenceSet& set) noexcept {
  const Bytes body = canonical_evidence_set(set);
  return Digest(sha256_domain(kDomainEvidence, std::span<const std::uint8_t>(body.data(), body.size())));
}

Result<EvaluationRequest> decode_evaluation_request_body(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalRequestBytes) {
    return Status(StatusCode::OutOfRange, "evaluation request exceeds the canonical size bound");
  }
  ByteReader reader(data);
  EvaluationRequest request;
  const std::uint8_t scope_kind = reader.u8();
  if (!reader.ok() || scope_kind > static_cast<std::uint8_t>(EnvelopeScopeKind::Facility)) {
    return Status(StatusCode::InvalidEnumValue, "request scope kind is not a defined value");
  }
  request.scope.kind = static_cast<EnvelopeScopeKind>(scope_kind);
  request.scope.identity.id = reader.text(kIdentifierMaxLength);
  request.scope.identity.generation = reader.u64();
  request.idempotency_key = reader.text(kIdempotencyKeyMaxLength);
  request.at = reader.i64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "request header is truncated");

  const bool has_revision = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "expected revision flag is truncated");
  if (has_revision) {
    request.expected_envelope_revision = reader.u64();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "expected revision is truncated");
  }
  const bool has_digest = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "expected digest flag is truncated");
  if (has_digest) {
    Digest digest;
    if (!decode_digest(reader, digest)) {
      return Status(StatusCode::TruncatedPayload, "expected envelope digest is truncated");
    }
    request.expected_envelope_digest = digest;
  }

  request.identity.identity.id = reader.text(kIdentifierMaxLength);
  request.identity.identity.generation = reader.u64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "identity binding is truncated");
  if (!decode_digest(reader, request.identity.digest)) {
    return Status(StatusCode::TruncatedPayload, "identity digest is truncated");
  }
  const std::uint8_t service_state = reader.u8();
  if (!reader.ok() || service_state > static_cast<std::uint8_t>(ResolutionState::Unavailable)) {
    return Status(StatusCode::InvalidEnumValue, "service class resolution state is not defined");
  }
  request.service_class.state = static_cast<ResolutionState>(service_state);
  if (!decode_digest(reader, request.service_class.digest)) {
    return Status(StatusCode::TruncatedPayload, "service class digest is truncated");
  }
  const std::uint8_t policy_state = reader.u8();
  if (!reader.ok() || policy_state > static_cast<std::uint8_t>(ResolutionState::Unavailable)) {
    return Status(StatusCode::InvalidEnumValue, "policy resolution state is not defined");
  }
  request.policy.state = static_cast<ResolutionState>(policy_state);
  if (!decode_digest(reader, request.policy.digest)) {
    return Status(StatusCode::TruncatedPayload, "policy digest is truncated");
  }

  const std::uint32_t class_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "accepted class count is truncated");
  if (class_count > kMaxEvidencePerRequest) {
    return Status(StatusCode::OutOfRange, "accepted class list exceeds the permitted size");
  }
  for (std::uint32_t index = 0; index < class_count; ++index) {
    request.accepted_classes.push_back(reader.text(kCompatibilityClassMaxLength));
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "accepted class token is truncated");
  }

  const std::uint32_t dimension_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension count is truncated");
  if (dimension_count > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "requested dimension count exceeds the permitted size");
  }
  for (std::uint32_t index = 0; index < dimension_count; ++index) {
    DimensionRequest item;
    const std::uint8_t kind = reader.u8();
    if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
      return Status(StatusCode::InvalidEnumValue, "requested dimension kind is not defined");
    }
    item.kind = static_cast<DimensionKind>(kind);
    item.quantity = reader.u64();
    item.principals = reader.u32();
    item.compatibility_class = reader.text(kCompatibilityClassMaxLength);
    item.compatibility_classes = reader.text(kMaxTextBytes);
    item.principal = reader.text(kMaxPrincipalTokenLength);
    item.level = reader.u32();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "requested dimension is truncated");
    const bool has_spare = reader.bool_value();
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "operational spare flag is truncated");
    if (has_spare) {
      item.operational_spare = reader.bool_value();
      if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "operational spare is truncated");
    }
    request.dimensions.push_back(std::move(item));
  }

  const std::uint8_t composition = reader.u8();
  if (!reader.ok() ||
      composition > static_cast<std::uint8_t>(FacilityComposition::ConjunctiveWithFacility)) {
    return Status(StatusCode::InvalidEnumValue, "facility composition is not a defined value");
  }
  request.facility_composition = static_cast<FacilityComposition>(composition);
  request.facility_envelope_id = reader.text(kIdentifierMaxLength);
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "request trailer is truncated");

  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "evaluation request did not decode exactly");
  }
  return request;
}

void encode_dimension_result(ByteWriter& writer, const DimensionResult& item) {
  writer.u8(static_cast<std::uint8_t>(item.kind));
  writer.u8(static_cast<std::uint8_t>(item.outcome));
  put_optional_nanounits(writer, item.limit);
  writer.u64(item.reserved);
  put_optional_nanounits(writer, item.committed);
  put_optional_nanounits(writer, item.effective_committed);
  put_optional_nanounits(writer, item.residual);
  writer.bool_value(item.over_committed);
  writer.u64(item.over_committed_by);
  writer.u64(item.requested);
  writer.u32(item.principals);
  put_optional_nanounits(writer, item.headroom_after);
  put_optional_nanounits(writer, item.observed);
  writer.bool_value(item.observation_present);
  writer.text(item.required_class);
  writer.text(item.held_class);
  writer.u32(item.holders);
  put_optional_nanounits(writer, item.required_threshold);
  put_optional_nanounits(writer, item.declared_threshold);
  put_optional_nanounits(writer, item.shortfall);
  writer.u32(item.desired_level);
  writer.u32(item.declared_level);
  writer.bool_value(item.operational_spare.has_value());
  if (item.operational_spare.has_value()) writer.bool_value(*item.operational_spare);
  writer.u64(item.quantum);
  put_optional_nanounits(writer, item.quantized_quantity);
  writer.u32(static_cast<std::uint32_t>(item.secondary.size()));
  for (const StatusCode code : item.secondary) writer.u32(static_cast<std::uint32_t>(code));
}

void encode_evaluation_result(ByteWriter& writer, const EvaluationResult& result) {
  writer.u8(static_cast<std::uint8_t>(result.outcome));
  writer.u32(static_cast<std::uint32_t>(result.reason));
  writer.u8(static_cast<std::uint8_t>(result.stage));
  writer.bool_value(result.blocking_dimension.has_value());
  if (result.blocking_dimension.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*result.blocking_dimension));
  }
  writer.text(result.envelope_id);
  writer.u64(result.envelope_revision);
  encode_digest(writer, result.envelope_digest);
  writer.u64(result.control_epoch);
  encode_digest(writer, result.request_digest);
  encode_digest(writer, result.evidence_digest);
  writer.u64(result.decision_sequence);
  encode_digest(writer, result.decision_digest);

  writer.u32(static_cast<std::uint32_t>(result.dimensions.size()));
  for (const DimensionResult& item : result.dimensions) encode_dimension_result(writer, item);

  writer.u32(static_cast<std::uint32_t>(result.secondary.size()));
  for (const StatusCode code : result.secondary) writer.u32(static_cast<std::uint32_t>(code));
}

Bytes canonical_evaluation_result(const EvaluationResult& result) {
  ByteWriter writer;
  encode_evaluation_result(writer, result);
  return writer.take();
}

// Shared dimension-result codec used by the evaluation result and the durable
// decision record, so the two forms can never drift apart.
bool decode_dimension_result(ByteReader& reader, DimensionResult& item) {
  const std::uint8_t kind = reader.u8();
  if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) return false;
  item.kind = static_cast<DimensionKind>(kind);
  const std::uint8_t outcome = reader.u8();
  if (!reader.ok() || outcome > static_cast<std::uint8_t>(DimensionOutcome::NotRequested)) return false;
  item.outcome = static_cast<DimensionOutcome>(outcome);

  item.limit = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.reserved = reader.u64();
  item.committed = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.effective_committed = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.residual = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.over_committed = reader.bool_value();
  item.over_committed_by = reader.u64();
  item.requested = reader.u64();
  item.principals = reader.u32();
  item.headroom_after = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.observed = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.observation_present = reader.bool_value();
  item.required_class = reader.text(kCompatibilityClassMaxLength);
  item.held_class = reader.text(kCompatibilityClassMaxLength);
  item.holders = reader.u32();
  item.required_threshold = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.declared_threshold = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.shortfall = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  item.desired_level = reader.u32();
  item.declared_level = reader.u32();
  const bool has_spare = reader.bool_value();
  if (!reader.ok()) return false;
  if (has_spare) {
    item.operational_spare = reader.bool_value();
    if (!reader.ok()) return false;
  }
  item.quantum = reader.u64();
  item.quantized_quantity = get_optional_nanounits(reader);
  if (!reader.ok()) return false;
  const std::uint32_t secondary_count = reader.u32();
  if (!reader.ok() || secondary_count > kMaxEvidencePerRequest) return false;
  for (std::uint32_t index = 0; index < secondary_count; ++index) {
    const std::uint32_t code = reader.u32();
    if (!reader.ok()) return false;
    item.secondary.push_back(static_cast<StatusCode>(code));
  }
  return reader.ok();
}

Result<EvaluationResult> decode_evaluation_result(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalDecisionBytes) {
    return Status(StatusCode::OutOfRange, "evaluation result exceeds the canonical size bound");
  }
  ByteReader reader(data);
  EvaluationResult result;
  const std::uint8_t outcome = reader.u8();
  if (!reader.ok() || outcome > static_cast<std::uint8_t>(Outcome::Indeterminate)) {
    return Status(StatusCode::InvalidEnumValue, "evaluation outcome is not a defined value");
  }
  result.outcome = static_cast<Outcome>(outcome);
  result.reason = static_cast<StatusCode>(reader.u32());
  const std::uint8_t stage = reader.u8();
  if (!reader.ok() || stage > static_cast<std::uint8_t>(RefusalStage::DimensionConstraint)) {
    return Status(StatusCode::InvalidEnumValue, "refusal stage is not a defined value");
  }
  result.stage = static_cast<RefusalStage>(stage);
  const bool has_blocking = reader.bool_value();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "blocking dimension flag is truncated");
  if (has_blocking) {
    const std::uint8_t kind = reader.u8();
    if (!reader.ok() || !is_valid(static_cast<DimensionKind>(kind))) {
      return Status(StatusCode::InvalidEnumValue, "blocking dimension is not a defined value");
    }
    result.blocking_dimension = static_cast<DimensionKind>(kind);
  }
  result.envelope_id = reader.text(kIdentifierMaxLength);
  result.envelope_revision = reader.u64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "result header is truncated");
  if (!decode_digest(reader, result.envelope_digest)) return Status(StatusCode::TruncatedPayload, "envelope digest is truncated");
  result.control_epoch = reader.u64();
  if (!decode_digest(reader, result.request_digest)) return Status(StatusCode::TruncatedPayload, "request digest is truncated");
  if (!decode_digest(reader, result.evidence_digest)) return Status(StatusCode::TruncatedPayload, "evidence digest is truncated");
  result.decision_sequence = reader.u64();
  if (!decode_digest(reader, result.decision_digest)) return Status(StatusCode::TruncatedPayload, "decision digest is truncated");
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "result authority block is truncated");

  const std::uint32_t dimension_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "dimension result count is truncated");
  if (dimension_count > kMaxDimensionsPerEnvelope) {
    return Status(StatusCode::OutOfRange, "dimension result count exceeds the permitted size");
  }
  for (std::uint32_t index = 0; index < dimension_count; ++index) {
    DimensionResult item;
    if (!decode_dimension_result(reader, item)) {
      return Status(StatusCode::TruncatedPayload, "dimension result is truncated or invalid");
    }
    result.dimensions.push_back(std::move(item));
  }

  const std::uint32_t secondary_count = reader.u32();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "secondary code count is truncated");
  if (secondary_count > kMaxEvidencePerRequest) {
    return Status(StatusCode::OutOfRange, "secondary code list exceeds the permitted size");
  }
  for (std::uint32_t index = 0; index < secondary_count; ++index) {
    result.secondary.push_back(static_cast<StatusCode>(reader.u32()));
    if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "secondary code is truncated");
  }

  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "evaluation result did not decode exactly");
  }
  return result;
}

namespace {

// Dimensions are encoded in canonical kind order so that the declaration order in
// a request or an envelope cannot change a digest.
std::vector<const DimensionSpec*> sorted_specs(const std::vector<DimensionSpec>& specs) {
  std::vector<const DimensionSpec*> ordered;
  ordered.reserve(specs.size());
  for (const DimensionSpec& spec : specs) ordered.push_back(&spec);
  std::sort(ordered.begin(), ordered.end(), [](const DimensionSpec* left, const DimensionSpec* right) {
    return static_cast<std::uint8_t>(left->kind) < static_cast<std::uint8_t>(right->kind);
  });
  return ordered;
}

}  // namespace

Digest content_digest(const Envelope& envelope) noexcept {
  const Bytes body = content_digest_input(envelope);
  return Digest(sha256_domain(kDomainEnvelopeContent, std::span<const std::uint8_t>(body.data(), body.size())));
}

Digest record_digest(const Envelope& envelope) noexcept {
  const Bytes body = record_digest_input(envelope);
  return Digest(sha256_domain(kDomainEnvelopeRecord, std::span<const std::uint8_t>(body.data(), body.size())));
}

Bytes content_digest_input(const Envelope& envelope) {
  ByteWriter writer;
  writer.u16(kCanonicalFormatVersion);
  writer.text(envelope.id);
  writer.u64(envelope.revision);
  writer.u8(static_cast<std::uint8_t>(envelope.kind));
  writer.u8(static_cast<std::uint8_t>(envelope.scope.kind));
  encode_identity_ref(writer, envelope.scope.identity);
  writer.text(envelope.site_id);
  encode_digest(writer, envelope.identity_digest);
  encode_digest(writer, envelope.service_class_digest);
  encode_digest(writer, envelope.policy_digest);
  put_optional_timestamp(writer, envelope.window.effective_from);
  put_optional_timestamp(writer, envelope.window.effective_until);
  writer.bool_value(envelope.require_binding_confirmation);
  writer.u8(static_cast<std::uint8_t>(envelope.merge_policy));
  writer.u32(static_cast<std::uint32_t>(envelope.precedence.size()));
  for (const DimensionKind kind : envelope.precedence) writer.u8(static_cast<std::uint8_t>(kind));
  const std::vector<const DimensionSpec*> ordered = sorted_specs(envelope.dimensions);
  writer.u32(static_cast<std::uint32_t>(ordered.size()));
  for (const DimensionSpec* spec : ordered) encode_dimension_spec(writer, *spec);
  return writer.take();
}

Bytes record_digest_input(const Envelope& envelope) {
  ByteWriter writer;
  writer.u16(kCanonicalFormatVersion);
  const Bytes content = content_digest_input(envelope);
  writer.blob(content);
  writer.bool_value(envelope.supersedes.has_value());
  if (envelope.supersedes.has_value()) writer.text(*envelope.supersedes);
  std::vector<EnvelopeRef> lineage = envelope.merged_from;
  std::sort(lineage.begin(), lineage.end(), [](const EnvelopeRef& left, const EnvelopeRef& right) {
    if (left.envelope_id != right.envelope_id) return left.envelope_id < right.envelope_id;
    return left.revision < right.revision;
  });
  writer.u32(static_cast<std::uint32_t>(lineage.size()));
  for (const EnvelopeRef& reference : lineage) encode_envelope_ref(writer, reference);
  encode_provenance(writer, envelope.provenance);
  return writer.take();
}

}  // namespace resource_envelope
