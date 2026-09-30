#include "resource_envelope/status.hpp"

namespace resource_envelope {

const char* to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "Ok";
    case StatusCode::InvalidArgument: return "InvalidArgument";
    case StatusCode::InvalidIdentifier: return "InvalidIdentifier";
    case StatusCode::InvalidText: return "InvalidText";
    case StatusCode::InvalidEnumValue: return "InvalidEnumValue";
    case StatusCode::InvalidDigest: return "InvalidDigest";
    case StatusCode::OutOfRange: return "OutOfRange";
    case StatusCode::DuplicateDimension: return "DuplicateDimension";
    case StatusCode::DuplicateIdentity: return "DuplicateIdentity";
    case StatusCode::TruncatedPayload: return "TruncatedPayload";
    case StatusCode::TrailingBytes: return "TrailingBytes";
    case StatusCode::UnsupportedVersion: return "UnsupportedVersion";
    case StatusCode::ReservedFieldNotZero: return "ReservedFieldNotZero";
    case StatusCode::OpaqueLimit: return "OpaqueLimit";
    case StatusCode::InsufficientReserved: return "InsufficientReserved";
    case StatusCode::CapacityNotUnlimited: return "CapacityNotUnlimited";
    case StatusCode::MissingObservation: return "MissingObservation";
    case StatusCode::UnexpectedObservation: return "UnexpectedObservation";
    case StatusCode::EmptyEnvelope: return "EmptyEnvelope";
    case StatusCode::EnvelopeNotFound: return "EnvelopeNotFound";
    case StatusCode::EnvelopeNotCurrent: return "EnvelopeNotCurrent";
    case StatusCode::StaleRevision: return "StaleRevision";
    case StatusCode::StaleAuthorityFenced: return "StaleAuthorityFenced";
    case StatusCode::ExpiredAuthority: return "ExpiredAuthority";
    case StatusCode::IdentityBindingMismatch: return "IdentityBindingMismatch";
    case StatusCode::ServiceClassBindingMismatch: return "ServiceClassBindingMismatch";
    case StatusCode::PolicyBindingMismatch: return "PolicyBindingMismatch";
    case StatusCode::RequiredBindingUnavailable: return "RequiredBindingUnavailable";
    case StatusCode::ForeignScope: return "ForeignScope";
    case StatusCode::RejectedHardLimit: return "RejectedHardLimit";
    case StatusCode::RejectedInsufficientResidual: return "RejectedInsufficientResidual";
    case StatusCode::RejectedExclusiveConflict: return "RejectedExclusiveConflict";
    case StatusCode::RejectedRedundancy: return "RejectedRedundancy";
    case StatusCode::RejectedExposureClass: return "RejectedExposureClass";
    case StatusCode::RejectedQuantization: return "RejectedQuantization";
    case StatusCode::RejectedAbsoluteBound: return "RejectedAbsoluteBound";
    case StatusCode::IndeterminateUnknownLimit: return "IndeterminateUnknownLimit";
    case StatusCode::IndeterminateUnknownCommitted: return "IndeterminateUnknownCommitted";
    case StatusCode::AmbiguousOverlap: return "AmbiguousOverlap";
    case StatusCode::RejectedReservation: return "RejectedReservation";
    case StatusCode::IdempotencyConflict: return "IdempotencyConflict";
    case StatusCode::ReplayedRequest: return "ReplayedRequest";
    case StatusCode::StoreNotFound: return "StoreNotFound";
    case StatusCode::StoreExists: return "StoreExists";
    case StatusCode::StoreCorrupt: return "StoreCorrupt";
    case StatusCode::StoreUnverified: return "StoreUnverified";
    case StatusCode::StoreRolledBack: return "StoreRolledBack";
    case StatusCode::StoreLocked: return "StoreLocked";
    case StatusCode::StoreIoError: return "StoreIoError";
    case StatusCode::StoreWriterBusy: return "StoreWriterBusy";
    case StatusCode::StoreIntegrityMismatch: return "StoreIntegrityMismatch";
    case StatusCode::StoreLimitExceeded: return "StoreLimitExceeded";
    case StatusCode::UnsupportedOperation: return "UnsupportedOperation";
  }
  return "UnknownStatusCode";
}

bool is_ok(StatusCode code) noexcept { return code == StatusCode::Ok; }

Status::Status(StatusCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

Status::Status(StatusCode code) : code_(code) {}

std::string Status::describe() const {
  std::string out(to_string(code_));
  if (!detail_.empty()) {
    out += ": ";
    out += detail_;
  }
  return out;
}

}  // namespace resource_envelope
