#ifndef RESOURCE_ENVELOPE_RESULT_HPP
#define RESOURCE_ENVELOPE_RESULT_HPP

#include <string>
#include <utility>
#include <variant>

#include "resource_envelope/status.hpp"

namespace resource_envelope {

// A value-or-status return type. The library does not throw for expected failure
// modes: every refusal carries a StatusCode and a machine-readable detail string.
template <typename T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Status status) : storage_(std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] const Status& status() const& { return std::get<1>(storage_); }

  template <typename U>
  [[nodiscard]] T value_or(U&& fallback) const {
    return ok() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::variant<T, Status> storage_;
};

// Convenience status-only result. The name deliberately avoids `Outcome`, which the
// evaluation model uses for the determination enum, so the two can never be confused
// at a call site.
template <typename T = void>
using StatusResult = Result<T>;

}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_RESULT_HPP
