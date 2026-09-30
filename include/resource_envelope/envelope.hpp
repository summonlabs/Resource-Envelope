#ifndef RESOURCE_ENVELOPE_ENVELOPE_HPP
#define RESOURCE_ENVELOPE_ENVELOPE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "resource_envelope/digest.hpp"
#include "resource_envelope/dimension.hpp"
#include "resource_envelope/export.hpp"
#include "resource_envelope/quantity.hpp"
#include "resource_envelope/result.hpp"
#include "resource_envelope/status.hpp"
#include "resource_envelope/time.hpp"

namespace resource_envelope {

inline constexpr std::uint64_t kRevisionMax = 0xFFFFFFFFFFFFFFFFULL;
inline constexpr std::uint64_t kStoreEpochMax = 0xFFFFFFFFFFFFFFFFULL;
inline constexpr std::uint64_t kSequenceMax = 0xFFFFFFFFFFFFFFFFULL;
inline constexpr std::size_t kMaxDimensionsPerEnvelope = 32;
inline constexpr std::size_t kMaxEvidencePerRequest = 64;
inline constexpr std::uint32_t kMaxRequestPrincipals = 65536U;

// ---------------------------------------------------------------------------
// Scope and identity binding
// ---------------------------------------------------------------------------
enum class EnvelopeScopeKind : std::uint8_t {
  Tenant = 0,
  Service = 1,
  // A facility-level envelope with no tenant or service principal. It never
  // replaces a tenant/service envelope; it composes with one through the declared
  // merge policy.
  Facility = 2,
};

RESOURCE_ENVELOPE_API const char* to_string(EnvelopeScopeKind kind) noexcept;
RESOURCE_ENVELOPE_API bool envelope_scope_kind_from_string(std::string_view text,
                                                          EnvelopeScopeKind& out) noexcept;

// A reference to an identity owned by another authority. Resource Envelope never
// creates, mutates, or infers a tenant or service identity; it binds to one.
struct RESOURCE_ENVELOPE_API IdentityRef {
  std::string id;
  std::uint64_t generation = 0;

  [[nodiscard]] friend bool operator==(const IdentityRef& a, const IdentityRef& b) noexcept {
    return a.id == b.id && a.generation == b.generation;
  }
  [[nodiscard]] friend bool operator!=(const IdentityRef& a, const IdentityRef& b) noexcept {
    return !(a == b);
  }
};

// The comparison key of an envelope. A request states the scope it claims; the
// scope is matched exactly on kind, identity and identity generation.
struct RESOURCE_ENVELOPE_API EnvelopeScope {
  EnvelopeScopeKind kind = EnvelopeScopeKind::Tenant;
  IdentityRef identity;

  [[nodiscard]] friend bool operator==(const EnvelopeScope& a, const EnvelopeScope& b) noexcept {
    return a.kind == b.kind && a.identity == b.identity;
  }
  [[nodiscard]] friend bool operator!=(const EnvelopeScope& a, const EnvelopeScope& b) noexcept {
    return !(a == b);
  }
};

// What the caller supplies about an externally owned identity so that the envelope
// binding can be checked. `digest` is the digest of the identity record as
// published by the owning authority; an unknown digest is explicitly represented.
struct RESOURCE_ENVELOPE_API IdentitySnapshot {
  IdentityRef identity;
  Digest digest;
};

// Resolution state of an externally owned referenced context.
enum class ResolutionState : std::uint8_t {
  // The caller supplied the referenced context.
  Provided = 0,
  // The owning authority was consulted and answered that the referenced revision
  // no longer exists or is not the current one.
  NotCurrent = 1,
  // The owning authority could not be consulted. This is never treated as
  // agreement, and when the referenced binding is required it is refused.
  Unavailable = 2,
};

struct RESOURCE_ENVELOPE_API ServiceClassResolution {
  ResolutionState state = ResolutionState::Unavailable;
  Digest digest;
};

struct RESOURCE_ENVELOPE_API PolicyResolution {
  ResolutionState state = ResolutionState::Unavailable;
  Digest digest;
};

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------
enum class EnvelopeKind : std::uint8_t {
  // Declared by an operator from the facility's physical records.
  Declared = 0,
  // Derived by merging overlapping envelopes under a declared merge policy.
  Composite = 1,
};

RESOURCE_ENVELOPE_API const char* to_string(EnvelopeKind kind) noexcept;

// An effective window. An absent start means "already effective"; an absent end
// means "no declared expiry". An envelope is authoritative only while the
// evaluation timestamp lies inside [effective_from, effective_until).
struct RESOURCE_ENVELOPE_API EffectiveWindow {
  std::optional<Timestamp> effective_from;
  std::optional<Timestamp> effective_until;

  [[nodiscard]] bool started_at(Timestamp now) const noexcept;
  [[nodiscard]] bool ended_at(Timestamp now) const noexcept;
  [[nodiscard]] bool active_at(Timestamp now) const noexcept;
};

// How to combine a facility-scope envelope with a tenant/service envelope.
enum class MergePolicy : std::uint8_t {
  // Refuse any overlap that is not provably order-independent. This is the default
  // because it is the only policy that cannot invent authority.
  Strict = 0,
  // Permit a composite only when one envelope dominates the other in *every*
  // dimension, which makes the merge provably order-independent. A partial
  // overlap (one envelope tighter here, the other tighter there) is refused as
  // ambiguous rather than resolved by an arbitrary choice.
  TightestWins = 1,
};

RESOURCE_ENVELOPE_API const char* to_string(MergePolicy policy) noexcept;
RESOURCE_ENVELOPE_API bool merge_policy_from_string(std::string_view text, MergePolicy& out) noexcept;

// Bounded reference to the revision of another envelope that this one supersedes or
// was merged from. The digest makes the lineage exact: a lineage entry whose digest
// no longer matches the referenced revision is detectable rather than silently
// accepted.
struct RESOURCE_ENVELOPE_API EnvelopeRef {
  std::string envelope_id;
  std::uint64_t revision = 0;
  Digest digest;
};

struct RESOURCE_ENVELOPE_API Provenance {
  std::string authority;
  std::string actor;
  std::string reason;
  Timestamp declared_at = 0;
};

struct RESOURCE_ENVELOPE_API Envelope {
  std::string id;
  std::uint64_t revision = 0;
  std::optional<std::string> supersedes;

  EnvelopeKind kind = EnvelopeKind::Declared;
  EnvelopeScope scope;
  std::string site_id;

  // Digest of the tenant/service identity record this envelope is bound to, as
  // published by the owning identity authority.
  Digest identity_digest;
  // Digest of the externally owned service class definition, when one applies.
  Digest service_class_digest;
  // Digest of the policy revision that authorised this declaration.
  Digest policy_digest;

  EffectiveWindow window;
  std::vector<EnvelopeRef> merged_from;

  // User-supplied ordering key. It is made total by falling back to kind order and
  // then to the request's own dimension order, so evaluation never depends on
  // container iteration order.
  std::vector<DimensionKind> precedence;
  MergePolicy merge_policy = MergePolicy::Strict;

  // Canonical dimension order. Merged evaluation is reportable only because this
  // order is total and stable.
  std::vector<DimensionSpec> dimensions;

  // When true, evaluation refuses unless the caller supplies the identity snapshot
  // and every referenced context digest declared above. A declared binding is
  // never allowed to decay into an unchecked one.
  bool require_binding_confirmation = true;

  Provenance provenance;

  [[nodiscard]] const DimensionSpec* find(DimensionKind kind) const noexcept;
  [[nodiscard]] DimensionSpec* find(DimensionKind kind) noexcept;
};

// ---------------------------------------------------------------------------
// Canonical digests
// ---------------------------------------------------------------------------
// content_digest covers only the authority-bearing content of the envelope:
// identity, scope, bindings, window, merge policy, precedence and every dimension
// limit. It deliberately excludes provenance text, the supersedes pointer and the
// lineage list, so that re-declaring identical constraints after a provenance
// correction yields the same content digest.
RESOURCE_ENVELOPE_API Digest content_digest(const Envelope& envelope) noexcept;
// record_digest covers content_digest plus provenance, supersedes and lineage, and
// is the digest bound into decision records.
RESOURCE_ENVELOPE_API Digest record_digest(const Envelope& envelope) noexcept;

// A merge outcome. Dominance is reported so that the caller can see why a merge was
// permitted rather than having to infer it.
enum class MergeDominance : std::uint8_t {
  NotApplicable = 0,
  FirstDominates = 1,
  SecondDominates = 2,
  Disjoint = 3,
};

RESOURCE_ENVELOPE_API const char* to_string(MergeDominance dominance) noexcept;

struct RESOURCE_ENVELOPE_API MergeResult {
  Envelope envelope;
  MergeDominance dominance = MergeDominance::NotApplicable;
};

// Composes `base` (typically facility scope) with `overlay` (tenant or service
// scope). The merge is refused with AmbiguousOverlap whenever the result would
// depend on argument order.
RESOURCE_ENVELOPE_API Result<MergeResult> merge_envelopes(const Envelope& base, const Envelope& overlay,
                                                         MergePolicy policy);

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_ENVELOPE_HPP
