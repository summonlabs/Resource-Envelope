#include "resource_envelope/dimension.hpp"

#include "resource_envelope/text.hpp"

namespace resource_envelope {

bool is_valid(DimensionKind kind) noexcept {
  switch (kind) {
    case DimensionKind::SpaceRackUnits:
    case DimensionKind::SpaceRackSlots:
    case DimensionKind::PowerDrawWatts:
    case DimensionKind::PowerFeedCircuits:
    case DimensionKind::CoolingLoadWatts:
    case DimensionKind::CoolingEnergyBudget:
    case DimensionKind::RackExposureClass:
    case DimensionKind::RedundancyLevel:
    case DimensionKind::CountRackPositions: return true;
  }
  return false;
}

const char* to_string(DimensionKind kind) noexcept {
  switch (kind) {
    case DimensionKind::SpaceRackUnits: return "space-rack-units";
    case DimensionKind::SpaceRackSlots: return "space-rack-slots";
    case DimensionKind::PowerDrawWatts: return "power-draw-watts";
    case DimensionKind::PowerFeedCircuits: return "power-feed-circuits";
    case DimensionKind::CoolingLoadWatts: return "cooling-load-watts";
    case DimensionKind::CoolingEnergyBudget: return "cooling-energy-budget";
    case DimensionKind::RackExposureClass: return "rack-exposure-class";
    case DimensionKind::RedundancyLevel: return "redundancy-level";
    case DimensionKind::CountRackPositions: return "count-rack-positions";
  }
  return "unknown-dimension";
}

bool dimension_kind_from_string(std::string_view text, DimensionKind& out) noexcept {
  static constexpr DimensionKind kKinds[] = {
      DimensionKind::SpaceRackUnits,      DimensionKind::SpaceRackSlots,
      DimensionKind::PowerDrawWatts,      DimensionKind::PowerFeedCircuits,
      DimensionKind::CoolingLoadWatts,    DimensionKind::CoolingEnergyBudget,
      DimensionKind::RackExposureClass,   DimensionKind::RedundancyLevel,
      DimensionKind::CountRackPositions,
  };
  for (const DimensionKind kind : kKinds) {
    if (text == to_string(kind)) {
      out = kind;
      return true;
    }
  }
  return false;
}

const char* to_string(DimensionUnit unit) noexcept {
  switch (unit) {
    case DimensionUnit::RackUnit: return "rack-unit";
    case DimensionUnit::Slot: return "slot";
    case DimensionUnit::Watt: return "watt";
    case DimensionUnit::Circuit: return "circuit";
    case DimensionUnit::WattHour: return "watt-hour";
    case DimensionUnit::ClassToken: return "class-token";
    case DimensionUnit::Level: return "level";
    case DimensionUnit::Position: return "position";
  }
  return "unknown-unit";
}

DimensionUnit unit_of(DimensionKind kind) noexcept {
  switch (kind) {
    case DimensionKind::SpaceRackUnits: return DimensionUnit::RackUnit;
    case DimensionKind::SpaceRackSlots: return DimensionUnit::Slot;
    case DimensionKind::PowerDrawWatts: return DimensionUnit::Watt;
    case DimensionKind::PowerFeedCircuits: return DimensionUnit::Circuit;
    case DimensionKind::CoolingLoadWatts: return DimensionUnit::Watt;
    case DimensionKind::CoolingEnergyBudget: return DimensionUnit::WattHour;
    case DimensionKind::RackExposureClass: return DimensionUnit::ClassToken;
    case DimensionKind::RedundancyLevel: return DimensionUnit::Level;
    case DimensionKind::CountRackPositions: return DimensionUnit::Position;
  }
  return DimensionUnit::Position;
}

Cardinality cardinality_of(DimensionKind kind) noexcept {
  switch (kind) {
    case DimensionKind::SpaceRackUnits:
    case DimensionKind::SpaceRackSlots:
    case DimensionKind::PowerDrawWatts:
    case DimensionKind::CoolingLoadWatts:
    case DimensionKind::CountRackPositions: return Cardinality::Consumable;
    case DimensionKind::PowerFeedCircuits:
    case DimensionKind::RackExposureClass: return Cardinality::Exclusive;
    case DimensionKind::CoolingEnergyBudget:
    case DimensionKind::RedundancyLevel: return Cardinality::NonConsumable;
  }
  return Cardinality::Consumable;
}

bool requires_compatibility_class(DimensionKind kind) noexcept {
  return cardinality_of(kind) == Cardinality::Exclusive;
}

bool is_quantized_unit(DimensionUnit unit) noexcept {
  // Watt-hour budgets are declared in whole watt-hours; every other unit is already
  // expressed in the scaled quantity representation.
  return unit == DimensionUnit::WattHour;
}

Nanounits base_increment(DimensionUnit unit) noexcept {
  return is_quantized_unit(unit) ? kNanounitsPerUnit : 1U;
}

const char* to_string(DimensionIndexing indexing) noexcept {
  switch (indexing) {
    case DimensionIndexing::Aggregate: return "aggregate";
    case DimensionIndexing::PerPrincipal: return "per-principal";
  }
  return "unknown-indexing";
}

}  // namespace resource_envelope
