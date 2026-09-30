#ifndef RESOURCE_ENVELOPE_INTERNAL_FILE_LOCK_HPP
#define RESOURCE_ENVELOPE_INTERNAL_FILE_LOCK_HPP

#include <cstdint>
#include <filesystem>

#include "resource_envelope/result.hpp"

namespace resource_envelope {
namespace internal {

// A real operating-system advisory lock over a byte range of one file. It is held
// for the lifetime of the object and released by the kernel when the process exits,
// so an abrupt death cannot leave the store permanently locked. The same primitive
// serves shared readers and the single writer.
enum class LockMode : std::uint8_t {
  Shared = 0,
  Exclusive = 1,
};

class FileLock {
 public:
  FileLock() = default;
  ~FileLock();

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  // Acquires the lock without blocking for longer than the supplied budget. Zero
  // means a single non-blocking attempt. This is a lock-acquisition policy, not a
  // watchdog over the work the lock protects.
  [[nodiscard]] static Result<FileLock> acquire(const std::filesystem::path& path, LockMode mode,
                                                std::uint64_t timeout_ms,
                                                std::uint64_t retry_interval_ms);

  [[nodiscard]] bool held() const noexcept;

  // Releases the lock if one is held. The destructor calls it, and it is public so that
  // a holder can release deterministically at a documented point instead of relying on
  // scope exit.
  void reset() noexcept;

 private:
  explicit FileLock(void* handle) noexcept;

  void* handle_ = nullptr;
};

}  // namespace internal
}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_INTERNAL_FILE_LOCK_HPP
