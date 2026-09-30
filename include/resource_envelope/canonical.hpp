#ifndef RESOURCE_ENVELOPE_CANONICAL_HPP
#define RESOURCE_ENVELOPE_CANONICAL_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "resource_envelope/bytes.hpp"
#include "resource_envelope/decision.hpp"
#include "resource_envelope/digest.hpp"
#include "resource_envelope/envelope.hpp"
#include "resource_envelope/export.hpp"
#include "resource_envelope/result.hpp"

namespace resource_envelope {

// Canonical encoding format tag for the persisted representation. It is written
// into every durable header; a reader refuses any other value instead of
// attempting a best-effort decode.
inline constexpr std::uint16_t kCanonicalFormatVersion = 1;

// Absolute size ceilings. Every decode path is bounded by these before allocating,
// so a hostile length prefix cannot cause an unbounded allocation.
inline constexpr std::size_t kMaxCanonicalDimspecBytes = 1024;
inline constexpr std::size_t kMaxCanonicalEnvelopeBytes = 64 * 1024;
inline constexpr std::size_t kMaxCanonicalEvidenceBytes = 1024;
inline constexpr std::size_t kMaxCanonicalRequestBytes = 256 * 1024;
inline constexpr std::size_t kMaxCanonicalDecisionBytes = 256 * 1024;
inline constexpr std::size_t kMaxCanonicalRecordBytes = 1024 * 1024;

// ---------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------
RESOURCE_ENVELOPE_API void encode_dimension_spec(ByteWriter& writer, const DimensionSpec& spec);
RESOURCE_ENVELOPE_API void encode_envelope(ByteWriter& writer, const Envelope& envelope);
RESOURCE_ENVELOPE_API void encode_dimension_request(ByteWriter& writer, const DimensionRequest& request);
RESOURCE_ENVELOPE_API void encode_evidence(ByteWriter& writer, const Evidence& evidence);
RESOURCE_ENVELOPE_API void encode_evaluation_request_body(ByteWriter& writer,
                                                         const EvaluationRequest& request);
RESOURCE_ENVELOPE_API void encode_evaluation_result(ByteWriter& writer, const EvaluationResult& result);
RESOURCE_ENVELOPE_API void encode_identity_ref(ByteWriter& writer, const IdentityRef& identity);
RESOURCE_ENVELOPE_API void encode_envelope_ref(ByteWriter& writer, const EnvelopeRef& reference);
RESOURCE_ENVELOPE_API void encode_provenance(ByteWriter& writer, const Provenance& provenance);
RESOURCE_ENVELOPE_API void encode_digest(ByteWriter& writer, const Digest& digest);
// Strict digest decoder shared by every canonical form that carries a digest.
RESOURCE_ENVELOPE_API bool decode_digest(ByteReader& reader, Digest& out);
// Shared dimension-result decoder used by the evaluation result and the durable
// decision record, so the two forms can never drift apart.
RESOURCE_ENVELOPE_API bool decode_dimension_result(ByteReader& reader, DimensionResult& item);
RESOURCE_ENVELOPE_API void encode_dimension_result(ByteWriter& writer, const DimensionResult& item);

// Convenience wrappers that return the complete canonical form.
RESOURCE_ENVELOPE_API Bytes canonical_dimension_spec(const DimensionSpec& spec);
RESOURCE_ENVELOPE_API Bytes canonical_envelope(const Envelope& envelope);
RESOURCE_ENVELOPE_API Bytes canonical_dimension_request(const DimensionRequest& request);
RESOURCE_ENVELOPE_API Bytes canonical_evidence(const Evidence& evidence);
RESOURCE_ENVELOPE_API Bytes canonical_evaluation_request_body(const EvaluationRequest& request);
RESOURCE_ENVELOPE_API Bytes canonical_evaluation_result(const EvaluationResult& result);

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------
// Every decoder fails on an unknown format tag, a truncated payload, an invalid
// enum, a reserved field that is not zero, a bound violation, or trailing bytes.
RESOURCE_ENVELOPE_API Result<DimensionSpec> decode_dimension_spec(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Result<Envelope> decode_envelope(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Result<DimensionRequest> decode_dimension_request(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Result<Evidence> decode_evidence(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Result<EvidenceSet> decode_evidence_set(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Result<EvaluationRequest> decode_evaluation_request_body(std::span<const std::uint8_t> data);
RESOURCE_ENVELOPE_API Result<EvaluationResult> decode_evaluation_result(std::span<const std::uint8_t> data);

// ---------------------------------------------------------------------------
// Digest projection
// ---------------------------------------------------------------------------
// Exact digest inputs, exposed so that the invariance tests can prove that a
// re-encoding of the same logical value produces the same bytes and that a changed
// authority input changes the digest.
RESOURCE_ENVELOPE_API Bytes content_digest_input(const Envelope& envelope);
RESOURCE_ENVELOPE_API Bytes record_digest_input(const Envelope& envelope);
RESOURCE_ENVELOPE_API Digest request_digest(const EvaluationRequest& request) noexcept;
RESOURCE_ENVELOPE_API Digest evidence_digest(const EvidenceSet& evidence) noexcept;

// ---------------------------------------------------------------------------
// Domain-separated digest helpers
// ---------------------------------------------------------------------------
// Domain tags are stored as ASCII and hashed ahead of the body, so digests for
// different purposes can never be confused even over identical bytes.
inline constexpr const char* kDomainEnvelopeContent = "resource-envelope/envelope-content/v1";
inline constexpr const char* kDomainEnvelopeRecord = "resource-envelope/envelope-record/v1";
inline constexpr const char* kDomainRequest = "resource-envelope/evaluation-request/v1";
inline constexpr const char* kDomainEvidence = "resource-envelope/evidence-set/v1";
inline constexpr const char* kDomainDecision = "resource-envelope/decision-record/v1";
inline constexpr const char* kDomainState = "resource-envelope/store-state/v1";
inline constexpr const char* kDomainFrame = "resource-envelope/journal-frame/v1";
inline constexpr const char* kDomainIdempotency = "resource-envelope/idempotency/v1";
inline constexpr const char* kDomainGrant = "resource-envelope/grant/v1";

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_CANONICAL_HPP
