#include "file_lock.hpp"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace resource_envelope {
namespace internal {
namespace {

#if defined(_WIN32)

// The lock covers the whole file. A shared lock admits any number of readers while
// excluding a writer; an exclusive lock excludes everything else.
bool try_lock_handle(void* handle, LockMode mode) noexcept {
  OVERLAPPED overlapped{};
  const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (mode == LockMode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0UL);
  const DWORD low = MAXDWORD;
  const DWORD high = MAXDWORD;
  return ::LockFileEx(static_cast<HANDLE>(handle), flags, 0UL, low, high, &overlapped) != 0;
}

void unlock_handle(void* handle) noexcept {
  OVERLAPPED overlapped{};
  const DWORD low = MAXDWORD;
  const DWORD high = MAXDWORD;
  (void)(::UnlockFileEx(static_cast<HANDLE>(handle), 0UL, low, high, &overlapped));
}

#else

bool try_lock_handle(void* handle, LockMode mode) noexcept {
  const int operation = (mode == LockMode::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
  return ::flock(static_cast<int>(reinterpret_cast<std::intptr_t>(handle)), operation) == 0;
}

void unlock_handle(void* handle) noexcept {
  (void)(::flock(static_cast<int>(reinterpret_cast<std::intptr_t>(handle)), LOCK_UN));
}

#endif

}  // namespace

FileLock::FileLock(void* handle) noexcept : handle_(handle) {}

FileLock::~FileLock() { reset(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    reset();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

void FileLock::reset() noexcept {
  if (handle_ == nullptr) return;
  unlock_handle(handle_);
#if defined(_WIN32)
  ::CloseHandle(static_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)));
#endif
  handle_ = nullptr;
}

bool FileLock::held() const noexcept { return handle_ != nullptr; }

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, LockMode mode,
                                  std::uint64_t timeout_ms, std::uint64_t retry_interval_ms) {
  // The lock file is opened for read/write and created when absent. The file itself
  // carries no state, so nothing depends on its contents.
#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status(StatusCode::StoreIoError, "the store lock file could not be opened");
  }
  void* raw = handle;
#else
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return Status(StatusCode::StoreIoError, "the store lock file could not be opened");
  }
  void* raw = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor));
#endif

  const std::uint64_t deadline = timeout_ms;
  std::uint64_t waited = 0;
  for (;;) {
    if (try_lock_handle(raw, mode)) return FileLock(raw);
    if (waited >= deadline) {
      FileLock failed(raw);
      failed.reset();
      return Status(StatusCode::StoreWriterBusy, "the store lock is held by another process");
    }
    const std::uint64_t slice = retry_interval_ms == 0 ? 1U : retry_interval_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    waited += slice;
  }
}

}  // namespace internal
}  // namespace resource_envelope
