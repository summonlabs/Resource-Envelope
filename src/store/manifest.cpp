#include "resource_envelope/store.hpp"

#include "resource_envelope/canonical.hpp"

namespace resource_envelope {
namespace {

void encode_manifest_body(ByteWriter& writer, const Manifest& manifest) {
  writer.u16(kCanonicalFormatVersion);
  writer.u32(manifest.layout_version);
  writer.u64(manifest.epoch);
  writer.u64(manifest.state_epoch);
  writer.u64(manifest.snapshot_segment);
  writer.u64(manifest.delta_segment);
  writer.u64(manifest.frames_committed);
  writer.u64(manifest.state_sequence);
  writer.u64(manifest.snapshot_frames);
  encode_digest(writer, manifest.state_digest);
  encode_digest(writer, manifest.chain_digest);
  writer.u64(manifest.envelope_count);
  writer.u64(manifest.revision_count);
  writer.u64(manifest.decision_count);
  writer.u64(manifest.usage_entry_count);
  writer.u64(manifest.observation_count);
  writer.u64(manifest.compaction_count);
}

}  // namespace

Bytes canonical_manifest(const Manifest& manifest) {
  ByteWriter writer;
  encode_manifest_body(writer, manifest);
  return writer.take();
}

Digest manifest_digest(const Manifest& manifest) noexcept {
  // The chain digest covers the manifest with its own chain-digest field cleared, so the
  // value is a stable commitment to everything else the manifest says. Hashing the field
  // that holds the result would make the digest depend on itself.
  Manifest copy = manifest;
  copy.chain_digest = Digest::unknown();
  const Bytes body = canonical_manifest(copy);
  return Digest(sha256_domain(kDomainState, std::span<const std::uint8_t>(body.data(), body.size())));
}

Result<Manifest> decode_manifest(std::span<const std::uint8_t> data) {
  // The manifest is small and fixed-shape. Anything larger than the canonical form
  // cannot be a manifest, so the bound is enforced before parsing.
  constexpr std::size_t kMaxManifestBytes = 512U;
  if (data.size() > kMaxManifestBytes) {
    return Status(StatusCode::OutOfRange, "manifest payload exceeds the canonical size bound");
  }
  ByteReader reader(data);
  Manifest manifest;
  const std::uint16_t version = reader.u16();
  if (!reader.ok() || version != kCanonicalFormatVersion) {
    return Status(StatusCode::UnsupportedVersion, "manifest carries an unsupported format version");
  }
  manifest.layout_version = reader.u32();
  if (!reader.ok() || manifest.layout_version != kStoreLayoutVersion) {
    return Status(StatusCode::UnsupportedVersion, "manifest carries an unsupported store layout version");
  }
  manifest.epoch = reader.u64();
  manifest.state_epoch = reader.u64();
  manifest.snapshot_segment = reader.u64();
  manifest.delta_segment = reader.u64();
  manifest.frames_committed = reader.u64();
  manifest.state_sequence = reader.u64();
  manifest.snapshot_frames = reader.u64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "manifest header is truncated");
  if (!decode_digest(reader, manifest.state_digest)) {
    return Status(StatusCode::TruncatedPayload, "manifest state digest is truncated");
  }
  if (!decode_digest(reader, manifest.chain_digest)) {
    return Status(StatusCode::TruncatedPayload, "manifest chain digest is truncated");
  }
  manifest.envelope_count = reader.u64();
  manifest.revision_count = reader.u64();
  manifest.decision_count = reader.u64();
  manifest.usage_entry_count = reader.u64();
  manifest.observation_count = reader.u64();
  manifest.compaction_count = reader.u64();
  if (!reader.ok()) return Status(StatusCode::TruncatedPayload, "manifest counters are truncated");
  if (!reader.require_exhausted()) {
    return Status(reader.ok() ? StatusCode::TrailingBytes : StatusCode::TruncatedPayload,
                  "manifest did not decode exactly");
  }
  if (manifest.state_epoch > manifest.epoch) {
    return Status(StatusCode::StoreCorrupt, "manifest state epoch is ahead of its control epoch");
  }
  if (manifest.snapshot_frames > manifest.frames_committed) {
    return Status(StatusCode::StoreCorrupt, "manifest snapshot length exceeds its committed length");
  }
  if (manifest.snapshot_segment != 0U && manifest.snapshot_segment > manifest.epoch) {
    return Status(StatusCode::StoreCorrupt, "manifest names a snapshot segment from a future epoch");
  }
  if (manifest.delta_segment != 0U && manifest.delta_segment > manifest.epoch) {
    return Status(StatusCode::StoreCorrupt, "manifest names a delta segment from a future epoch");
  }
  return manifest;
}

}  // namespace resource_envelope
