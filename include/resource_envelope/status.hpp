#ifndef RESOURCE_ENVELOPE_STATUS_HPP
#define RESOURCE_ENVELOPE_STATUS_HPP

#include <cstdint>
#include <string>
#include <utility>

#include "resource_envelope/export.hpp"

namespace resource_envelope {

// Stable, machine-readable failure classification. These numeric values are part
// of the observable contract: they appear in CLI output and inside persisted
// decision records, so they are never renumbered. New codes are appended.
enum class StatusCode : std::uint32_t {
  Ok = 0,

  // Structural and input validation.
  InvalidArgument = 1,
  InvalidIdentifier = 2,
  InvalidText = 3,
  InvalidEnumValue = 4,
  InvalidDigest = 5,
  OutOfRange = 6,
  DuplicateDimension = 7,
  DuplicateIdentity = 8,
  TruncatedPayload = 9,
  TrailingBytes = 10,
  UnsupportedVersion = 11,
  ReservedFieldNotZero = 12,
  OpaqueLimit = 13,
  InsufficientReserved = 14,
  CapacityNotUnlimited = 15,
  MissingObservation = 16,
  UnexpectedObservation = 17,
  EmptyEnvelope = 18,

  // Envelope lifecycle and authority.
  EnvelopeNotFound = 20,
  EnvelopeNotCurrent = 21,
  StaleRevision = 22,
  StaleAuthorityFenced = 23,
  ExpiredAuthority = 24,

  // Identity and context binding.
  IdentityBindingMismatch = 30,
  ServiceClassBindingMismatch = 31,
  PolicyBindingMismatch = 32,
  RequiredBindingUnavailable = 33,
  ForeignScope = 34,

  // Evaluation outcomes.
  RejectedHardLimit = 40,
  RejectedInsufficientResidual = 41,
  RejectedExclusiveConflict = 42,
  RejectedRedundancy = 43,
  RejectedExposureClass = 44,
  RejectedQuantization = 45,
  RejectedAbsoluteBound = 46,
  IndeterminateUnknownLimit = 47,
  IndeterminateUnknownCommitted = 48,
  AmbiguousOverlap = 49,
  RejectedReservation = 50,

  // Idempotency.
  IdempotencyConflict = 60,
  ReplayedRequest = 61,

  // Durable store.
  StoreNotFound = 80,
  StoreExists = 81,
  StoreCorrupt = 82,
  StoreUnverified = 83,
  StoreRolledBack = 84,
  StoreLocked = 85,
  StoreIoError = 86,
  StoreWriterBusy = 87,
  StoreIntegrityMismatch = 88,
  StoreLimitExceeded = 89,
  UnsupportedOperation = 90,
};

RESOURCE_ENVELOPE_API const char* to_string(StatusCode code) noexcept;
RESOURCE_ENVELOPE_API bool is_ok(StatusCode code) noexcept;

// A status carries the primary code plus detail that is safe to print and to
// persist: it never contains a filesystem-absolute path, a host name, a process
// id, or any other host-specific value.
class RESOURCE_ENVELOPE_API Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string detail);
  explicit Status(StatusCode code);

  // A default-constructed Status is the success value; there is deliberately no
  // static ok() factory, because a static and a non-static member of the same name
  // would make every call site ambiguous.
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  // Renders as "Code" or "Code: detail". Named describe() rather than to_string()
  // so that the member and the free to_string(StatusCode) overload can never be
  // selected ambiguously at a call site.
  [[nodiscard]] std::string describe() const;

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string detail_;
};

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_STATUS_HPP
