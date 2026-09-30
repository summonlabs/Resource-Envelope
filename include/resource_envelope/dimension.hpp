#ifndef RESOURCE_ENVELOPE_DIMENSION_HPP
#define RESOURCE_ENVELOPE_DIMENSION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "resource_envelope/export.hpp"
#include "resource_envelope/quantity.hpp"
#include "resource_envelope/result.hpp"
#include "resource_envelope/status.hpp"
#include "resource_envelope/time.hpp"

namespace resource_envelope {

// ---------------------------------------------------------------------------
// Dimension kinds
// ---------------------------------------------------------------------------
// Dimensions are modelled independently and are never converted into one another.
// The numeric values are part of the canonical encoding, the persisted format and
// the CLI vocabulary, so they are fixed and new kinds are appended.
enum class DimensionKind : std::uint8_t {
  SpaceRackUnits = 1,
  SpaceRackSlots = 2,
  PowerDrawWatts = 3,
  PowerFeedCircuits = 4,
  CoolingLoadWatts = 5,
  CoolingEnergyBudget = 6,
  RackExposureClass = 7,
  RedundancyLevel = 8,
  CountRackPositions = 9,
};

// True only for a defined kind. Wire values that are not listed are refused rather
// than being treated as a default.
RESOURCE_ENVELOPE_API bool is_valid(DimensionKind kind) noexcept;
RESOURCE_ENVELOPE_API const char* to_string(DimensionKind kind) noexcept;
RESOURCE_ENVELOPE_API bool dimension_kind_from_string(std::string_view text, DimensionKind& out) noexcept;

// How the arithmetic of a dimension behaves.
enum class Cardinality : std::uint8_t {
  // Quantitative: limit minus reserved minus committed yields a residual that a
  // request can consume. Space, power and cooling load are Consumable.
  Consumable = 0,
  // At most one principal may hold the dimension, and a holder consumes the whole
  // capacity. Electrical feed circuits and rack exposure classes are Exclusive.
  Exclusive = 1,
  // A threshold that must be met or exceeded but is not consumed and does not
  // yield capacity. Redundancy level and the cooling energy budget are
  // NonConsumable: they constrain a declaration instead of granting headroom.
  NonConsumable = 2,
};

// Physical unit of a dimension. A request may only carry dimensions whose unit
// matches; the runtime never converts between units.
enum class DimensionUnit : std::uint8_t {
  RackUnit = 0,
  Slot = 1,
  Watt = 2,
  Circuit = 3,
  WattHour = 4,
  ClassToken = 5,
  Level = 6,
  Position = 7,
};

RESOURCE_ENVELOPE_API const char* to_string(DimensionUnit unit) noexcept;
RESOURCE_ENVELOPE_API DimensionUnit unit_of(DimensionKind kind) noexcept;
RESOURCE_ENVELOPE_API Cardinality cardinality_of(DimensionKind kind) noexcept;
RESOURCE_ENVELOPE_API bool requires_compatibility_class(DimensionKind kind) noexcept;
RESOURCE_ENVELOPE_API bool is_quantized_unit(DimensionUnit unit) noexcept;
// Number of whole base units that one increment of this unit represents. Watt-hour
// budgets are declared in whole watt-hours; every other unit is already scaled by
// the base quantity representation.
RESOURCE_ENVELOPE_API Nanounits base_increment(DimensionUnit unit) noexcept;

enum class DimensionIndexing : std::uint8_t {
  // The limit applies to the aggregate of every principal in scope.
  Aggregate = 0,
  // The limit applies to each principal separately; the aggregate is reported but
  // never used as the admission test.
  PerPrincipal = 1,
};

RESOURCE_ENVELOPE_API const char* to_string(DimensionIndexing indexing) noexcept;

// Compatibility classes are opaque tokens owned by the facility authority (for
// example an electrical feed class or a rack exposure class). Resource Envelope
// stores and compares them; it never invents, ranks, or normalises them.
inline constexpr std::size_t kCompatibilityClassMaxLength = 32;

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------
// Observation is never authority. A measurement is recorded and reported, but the
// admission arithmetic only ever consumes the committed quantity. Committed usage
// in turn is either present or explicitly unknown; it is never inferred from an
// observation.
enum class MeasureStatus : std::uint8_t {
  Unknown = 0,
  Measured = 1,
};

struct RESOURCE_ENVELOPE_API Measurement {
  MeasureStatus status = MeasureStatus::Unknown;
  Nanounits value = 0;
  Timestamp observed_at = 0;
  std::string source;  // canonical token identifying the measuring authority

  [[nodiscard]] bool known() const noexcept { return status == MeasureStatus::Measured; }

  [[nodiscard]] friend bool operator==(const Measurement& a, const Measurement& b) noexcept {
    return a.status == b.status && (!a.known() || (a.value == b.value && a.observed_at == b.observed_at &&
                                                          a.source == b.source));
  }
};

struct RESOURCE_ENVELOPE_API CommittedUsage {
  MeasureStatus status = MeasureStatus::Unknown;
  Nanounits value = 0;
  Timestamp observed_at = 0;
  std::string source;

  [[nodiscard]] bool known() const noexcept { return status == MeasureStatus::Measured; }

  [[nodiscard]] friend bool operator==(const CommittedUsage& a, const CommittedUsage& b) noexcept {
    return a.status == b.status && (!a.known() || (a.value == b.value && a.observed_at == b.observed_at &&
                                                          a.source == b.source));
  }
};

// An exclusive-class declaration. For Exclusive dimensions, "committed" means the
// class currently held; holders are counted in whole units.
struct RESOURCE_ENVELOPE_API ExclusiveDeclaration {
  MeasureStatus status = MeasureStatus::Unknown;
  std::string compatibility_class;
  UnitCount holders = 0;
  std::string holder_identity;
  Timestamp observed_at = 0;
  std::string source;

  [[nodiscard]] bool known() const noexcept { return status == MeasureStatus::Measured; }
};

// A threshold declaration for a NonConsumable dimension.
struct RESOURCE_ENVELOPE_API ThresholdDeclaration {
  MeasureStatus status = MeasureStatus::Unknown;
  Nanounits required = 0;
  Timestamp observed_at = 0;
  std::string source;

  [[nodiscard]] bool known() const noexcept { return status == MeasureStatus::Measured; }
};

// One evidence item for one dimension. The active member is selected by the
// cardinality of the addressed dimension, and a mismatch between the two is
// refused at the API boundary rather than being interpreted.
struct RESOURCE_ENVELOPE_API Evidence {
  DimensionKind kind = DimensionKind::CountRackPositions;
  std::optional<Measurement> observation;
  std::optional<CommittedUsage> committed;
  std::optional<ExclusiveDeclaration> exclusive;
  std::optional<ThresholdDeclaration> threshold;
};

// ---------------------------------------------------------------------------
// Dimension specification
// ---------------------------------------------------------------------------
struct RESOURCE_ENVELOPE_API DimensionSpec {
  DimensionKind kind = DimensionKind::CountRackPositions;

  // The absolute physical bound recorded for this dimension. `none` means the
  // bound is genuinely unknown to Resource Envelope; it is never read as "zero" and
  // never read as "unlimited". An evaluation that needs an unknown bound to answer
  // returns Indeterminate, not Granted.
  std::optional<Nanounits> hard_limit;

  // Capacity set aside for principals outside this envelope. Reserved capacity is
  // subtracted from the residual before committed usage is considered, so a
  // reservation can never be consumed by this envelope.
  Nanounits reserved = 0;

  // Alignment increment. A request whose quantity is not an exact multiple is
  // refused with the aligned quantity reported; the runtime never rounds silently.
  Nanounits quantum = 0;

  // Externally owned context key (policy digest or service-class digest). It is
  // stored, compared and digested, never interpreted.
  std::string compatibility_class;

  DimensionIndexing indexing = DimensionIndexing::Aggregate;

  // The committed quantity recorded in durable state, if any. When absent, the
  // caller's evidence decides; when the caller supplies none either, committed
  // usage is Unknown rather than zero.
  std::optional<CommittedUsage> committed;

  [[nodiscard]] bool has_known_limit() const noexcept { return hard_limit.has_value(); }
};

// Requested change for one dimension. A request never mutates an envelope; it is
// evaluated against the envelope's declared constraints.
struct RESOURCE_ENVELOPE_API DimensionRequest {
  DimensionKind kind = DimensionKind::CountRackPositions;
  Nanounits quantity = 0;
  std::uint32_t principals = 1;
  std::string compatibility_class;
  std::string compatibility_classes;  // comma-separated multi-class alternative
  std::string principal;
  // Declared redundancy level for RedundancyLevel requests.
  std::uint32_t level = 0;
  // Spare is operational only when this says so; a declared-but-unmeasured spare is
  // Unknown and can never satisfy a redundancy requirement.
  std::optional<bool> operational_spare;
};

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_DIMENSION_HPP
