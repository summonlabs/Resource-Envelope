#include "resource_envelope/envelope.hpp"

#include <algorithm>

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/text.hpp"

namespace resource_envelope {
namespace {

bool scope_order(const EnvelopeScope& a, const EnvelopeScope& b) noexcept {
  if (a.kind != b.kind) return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
  if (a.identity.id != b.identity.id) return a.identity.id < b.identity.id;
  return a.identity.generation < b.identity.generation;
}

// Constraint comparison. A tighter limit, a larger reservation and a stricter
// alignment are each strictly more restrictive; an absent limit is the least
// restrictive value, which is why "unknown bound" can never silently dominate a
// declared bound.
int compare_optional_tightness(const std::optional<Nanounits>& left, const std::optional<Nanounits>& right) noexcept {
  if (left.has_value() != right.has_value()) return left.has_value() ? -1 : 1;
  if (!left.has_value()) return 0;
  if (*left == *right) return 0;
  return *left < *right ? -1 : 1;
}

int compare_quantum_tightness(Nanounits left, Nanounits right) noexcept {
  // Zero means "no alignment requirement". A non-zero quantum is at least as
  // restrictive as zero; between two non-zero quanta the larger step is stricter.
  if (left == right) return 0;
  if (left == 0) return 1;
  if (right == 0) return -1;
  return left > right ? -1 : 1;
}

// Negative means the left constraint is tighter or equal.
int compare_spec_tightness(const DimensionSpec& left, const DimensionSpec& right) {
  int result = compare_optional_tightness(left.hard_limit, right.hard_limit);
  if (result != 0) return result;
  if (left.reserved != right.reserved) return left.reserved > right.reserved ? -1 : 1;
  result = compare_quantum_tightness(left.quantum, right.quantum);
  if (result != 0) return result;
  if (left.compatibility_class != right.compatibility_class) {
    // Two different classes are not comparable, which makes the merge ambiguous
    // rather than silently choosing one.
    return 2;
  }
  if (left.indexing != right.indexing) return left.indexing == DimensionIndexing::PerPrincipal ? -1 : 1;
  return 0;
}

DimensionSpec tighter_spec(const DimensionSpec& left, const DimensionSpec& right) {
  DimensionSpec merged = left;
  if (compare_optional_tightness(right.hard_limit, left.hard_limit) < 0) merged.hard_limit = right.hard_limit;
  if (right.reserved > merged.reserved) merged.reserved = right.reserved;
  if (compare_quantum_tightness(right.quantum, merged.quantum) < 0) merged.quantum = right.quantum;
  if (merged.compatibility_class.empty()) merged.compatibility_class = right.compatibility_class;
  if (right.indexing == DimensionIndexing::PerPrincipal) merged.indexing = DimensionIndexing::PerPrincipal;
  // Committed usage is an observation about a specific authority; a merged envelope
  // therefore carries no inherited committed figure and resolves it from state or
  // from caller evidence at evaluation time.
  merged.committed.reset();
  return merged;
}

}  // namespace

bool EffectiveWindow::started_at(Timestamp now) const noexcept {
  return !effective_from.has_value() || now >= *effective_from;
}

bool EffectiveWindow::ended_at(Timestamp now) const noexcept {
  return effective_until.has_value() && now >= *effective_until;
}

bool EffectiveWindow::active_at(Timestamp now) const noexcept { return started_at(now) && !ended_at(now); }

const DimensionSpec* Envelope::find(DimensionKind dimension_kind) const noexcept {
  for (const DimensionSpec& spec : dimensions) {
    if (spec.kind == dimension_kind) return &spec;
  }
  return nullptr;
}

DimensionSpec* Envelope::find(DimensionKind dimension_kind) noexcept {
  for (DimensionSpec& spec : dimensions) {
    if (spec.kind == dimension_kind) return &spec;
  }
  return nullptr;
}

const char* to_string(EnvelopeScopeKind kind) noexcept {
  switch (kind) {
    case EnvelopeScopeKind::Tenant: return "tenant";
    case EnvelopeScopeKind::Service: return "service";
    case EnvelopeScopeKind::Facility: return "facility";
  }
  return "unknown-scope";
}

bool envelope_scope_kind_from_string(std::string_view text, EnvelopeScopeKind& out) noexcept {
  if (text == "tenant") {
    out = EnvelopeScopeKind::Tenant;
    return true;
  }
  if (text == "service") {
    out = EnvelopeScopeKind::Service;
    return true;
  }
  if (text == "facility") {
    out = EnvelopeScopeKind::Facility;
    return true;
  }
  return false;
}

const char* to_string(EnvelopeKind kind) noexcept {
  switch (kind) {
    case EnvelopeKind::Declared: return "declared";
    case EnvelopeKind::Composite: return "composite";
  }
  return "unknown-kind";
}

const char* to_string(MergePolicy policy) noexcept {
  switch (policy) {
    case MergePolicy::Strict: return "strict";
    case MergePolicy::TightestWins: return "tightest-wins";
  }
  return "unknown-policy";
}

bool merge_policy_from_string(std::string_view text, MergePolicy& out) noexcept {
  if (text == "strict") {
    out = MergePolicy::Strict;
    return true;
  }
  if (text == "tightest-wins") {
    out = MergePolicy::TightestWins;
    return true;
  }
  return false;
}

const char* to_string(MergeDominance dominance) noexcept {
  switch (dominance) {
    case MergeDominance::NotApplicable: return "not-applicable";
    case MergeDominance::FirstDominates: return "first-dominates";
    case MergeDominance::SecondDominates: return "second-dominates";
    case MergeDominance::Disjoint: return "disjoint";
  }
  return "unknown-dominance";
}

Result<MergeResult> merge_envelopes(const Envelope& left, const Envelope& right, MergePolicy policy) {
  // Composition is only meaningful across different scopes. Two envelopes for the
  // same scope are versions of one authority, not two constraints on one subject,
  // and combining them would invent authority that no declaration granted.
  //
  // The overlay supplies the identity, bindings and declared precedence order of the result, so
  // the two arguments are not interchangeable. Rather than report different results for the two
  // spellings of one composition, the arguments are canonicalised first: they are ordered by
  // envelope identifier, then revision, then record digest, which is a strict total order over
  // distinct envelopes. Both spellings then compute exactly one result.
  const bool left_first = left.id != right.id
                              ? left.id < right.id
                              : (left.revision != right.revision
                                     ? left.revision < right.revision
                                     : record_digest(left).to_string() < record_digest(right).to_string());
  const Envelope& base = left_first ? left : right;
  const Envelope& overlay = left_first ? right : left;
  if (base.scope == overlay.scope) {
    return Status(StatusCode::AmbiguousOverlap,
                  "both envelopes address the same scope; compose revisions instead");
  }
  if (base.id == overlay.id) {
    return Status(StatusCode::AmbiguousOverlap, "an envelope cannot be merged with itself");
  }

  std::vector<DimensionKind> shared;
  for (const DimensionSpec& spec : overlay.dimensions) {
    if (base.find(spec.kind) != nullptr) shared.push_back(spec.kind);
  }
  std::sort(shared.begin(), shared.end(), [](DimensionKind left, DimensionKind right) {
    return static_cast<std::uint8_t>(left) < static_cast<std::uint8_t>(right);
  });

  if (policy == MergePolicy::Strict && !shared.empty()) {
    return Status(StatusCode::AmbiguousOverlap,
                  std::string("strict composition refuses overlapping dimension ") +
                      to_string(shared.front()));
  }

  int towards_overlay = 0;
  int towards_base = 0;
  for (const DimensionKind kind : shared) {
    const DimensionSpec* base_spec = base.find(kind);
    const DimensionSpec* overlay_spec = overlay.find(kind);
    if (base_spec == nullptr || overlay_spec == nullptr) continue;
    const int relative = compare_spec_tightness(*base_spec, *overlay_spec);
    if (relative == 2) {
      return Status(StatusCode::AmbiguousOverlap, std::string("dimension ") + to_string(kind) +
                                                      " declares incomparable compatibility classes");
    }
    if (relative < 0) {
      ++towards_base;
    } else if (relative > 0) {
      ++towards_overlay;
    }
  }
  if (towards_base != 0 && towards_overlay != 0) {
    // Partially overlapping constraints. Selecting either order would change the
    // result, so the merge is refused rather than resolved arbitrarily.
    return Status(StatusCode::AmbiguousOverlap,
                  "overlapping envelopes are mutually non-dominant; the merge would be order dependent");
  }

  Envelope merged;
  merged.kind = EnvelopeKind::Composite;
  merged.scope = overlay.scope;
  merged.site_id = overlay.site_id.empty() ? base.site_id : overlay.site_id;
  merged.identity_digest = overlay.identity_digest.known() ? overlay.identity_digest : base.identity_digest;
  merged.service_class_digest =
      overlay.service_class_digest.known() ? overlay.service_class_digest : base.service_class_digest;
  merged.policy_digest = overlay.policy_digest.known() ? overlay.policy_digest : base.policy_digest;
  merged.merge_policy = policy;
  merged.require_binding_confirmation =
      base.require_binding_confirmation || overlay.require_binding_confirmation;

  // Window intersection: the composite is authoritative only while both sources
  // are. An empty intersection has no authority to grant and is refused.
  merged.window.effective_from = base.window.effective_from;
  if (overlay.window.effective_from.has_value() &&
      (!merged.window.effective_from.has_value() ||
       *overlay.window.effective_from > *merged.window.effective_from)) {
    merged.window.effective_from = overlay.window.effective_from;
  }
  merged.window.effective_until = base.window.effective_until;
  if (overlay.window.effective_until.has_value() &&
      (!merged.window.effective_until.has_value() ||
       *overlay.window.effective_until < *merged.window.effective_until)) {
    merged.window.effective_until = overlay.window.effective_until;
  }
  if (merged.window.effective_from.has_value() && merged.window.effective_until.has_value() &&
      *merged.window.effective_from >= *merged.window.effective_until) {
    return Status(StatusCode::AmbiguousOverlap, "the composed effective windows do not intersect");
  }

  // Precedence: the overlay's declared order first, then base dimensions it omits.
  for (const DimensionKind kind : overlay.precedence) {
    if (std::find(merged.precedence.begin(), merged.precedence.end(), kind) == merged.precedence.end()) {
      merged.precedence.push_back(kind);
    }
  }
  for (const DimensionKind kind : base.precedence) {
    if (std::find(merged.precedence.begin(), merged.precedence.end(), kind) == merged.precedence.end()) {
      merged.precedence.push_back(kind);
    }
  }

  // Constraints: the overlay's own dimensions verbatim, base dimensions that do
  // not overlap, and the tighter of the two for each shared dimension.
  for (const DimensionSpec& spec : overlay.dimensions) merged.dimensions.push_back(spec);
  for (const DimensionSpec& spec : base.dimensions) {
    if (overlay.find(spec.kind) == nullptr) merged.dimensions.push_back(spec);
  }
  for (const DimensionKind kind : shared) {
    DimensionSpec* target = merged.find(kind);
    if (target == nullptr) continue;
    *target = tighter_spec(*base.find(kind), *overlay.find(kind));
  }

  // The identity of a composite is derived from the exact sources and the policy,
  // so identical inputs always produce an identical envelope and digest, and no part
  // of the result depends on argument order or on ambient state.
  ByteWriter fingerprint;
  fingerprint.u8(static_cast<std::uint8_t>(policy));
  const EnvelopeRef base_ref{base.id, base.revision, record_digest(base)};
  const EnvelopeRef overlay_ref{overlay.id, overlay.revision, record_digest(overlay)};
  std::vector<EnvelopeRef> sources{base_ref, overlay_ref};
  std::sort(sources.begin(), sources.end(), [](const EnvelopeRef& left, const EnvelopeRef& right) {
    if (left.envelope_id != right.envelope_id) return left.envelope_id < right.envelope_id;
    return left.revision < right.revision;
  });
  for (const EnvelopeRef& reference : sources) encode_envelope_ref(fingerprint, reference);
  merged.merged_from = sources;
  merged.id = "composite-" + digest_hex_u64(fnv1a64(fingerprint.bytes()));
  merged.revision = 1;
  merged.provenance.authority = "resource-envelope";
  merged.provenance.actor = "merge";
  merged.provenance.reason =
      std::string("composed under ") + to_string(policy) + " policy from " + sources[0].envelope_id +
      " and " + sources[1].envelope_id;
  merged.provenance.declared_at = 0;

  MergeResult result;
  result.envelope = std::move(merged);
  if (shared.empty()) {
    result.dominance = MergeDominance::Disjoint;
  } else if (towards_overlay != 0) {
    result.dominance = MergeDominance::SecondDominates;
  } else if (towards_base != 0) {
    result.dominance = MergeDominance::FirstDominates;
  } else {
    // Overlapping but identical constraints are still order independent.
    result.dominance = MergeDominance::NotApplicable;
  }
  return result;
}

}  // namespace resource_envelope
