#include "store_io.hpp"

#include <vector>

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN and NOMINMAX are supplied by the build for every translation unit
// of this repository, so they are not defined here as well.
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace resource_envelope {
namespace internal {
namespace {

// Positional reads and writes are chunked so that a single request never exceeds the
// platform's per-call transfer limit, and so that a partial transfer is resumed
// rather than treated as success.
constexpr std::size_t kIoChunkBytes = 1U << 20U;

}  // namespace

FileHandle::FileHandle(void* handle) noexcept : handle_(handle) {}

FileHandle::~FileHandle() { close(); }

FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

void FileHandle::close() noexcept {
  if (handle_ == nullptr) return;
#if defined(_WIN32)
  ::CloseHandle(static_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)));
#endif
  handle_ = nullptr;
}

bool FileHandle::valid() const noexcept { return handle_ != nullptr; }

#if defined(_WIN32)

Result<FileHandle> FileHandle::open_read(const std::filesystem::path& path) {
  const HANDLE handle =
      ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status(StatusCode::StoreIoError, "file could not be opened for reading");
  }
  return FileHandle(handle);
}

Result<FileHandle> FileHandle::open_write_append(const std::filesystem::path& path) {
  const HANDLE handle =
      ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status(StatusCode::StoreIoError, "file could not be opened for writing");
  }
  return FileHandle(handle);
}

Result<FileHandle> FileHandle::create_truncate(const std::filesystem::path& path) {
  const HANDLE handle =
      ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status(StatusCode::StoreIoError, "file could not be created for writing");
  }
  return FileHandle(handle);
}

std::uint64_t FileHandle::size() const {
  LARGE_INTEGER length{};
  if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &length) == 0) return 0U;
  return static_cast<std::uint64_t>(length.QuadPart);
}

Status FileHandle::write_at(std::uint64_t offset, std::span<const std::uint8_t> data) {
  std::size_t written = 0U;
  while (written < data.size()) {
    const std::size_t remaining = data.size() - written;
    const DWORD chunk = static_cast<DWORD>(remaining > kIoChunkBytes ? kIoChunkBytes : remaining);
    const std::uint64_t position = offset + written;
    DWORD transferred = 0U;
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>((position >> 32U) & 0xFFFFFFFFULL);
    if (::WriteFile(static_cast<HANDLE>(handle_), data.data() + written, chunk, &transferred, &overlapped) == 0) {
      return Status(StatusCode::StoreIoError, "a positional write failed");
    }
    if (transferred == 0U) return Status(StatusCode::StoreIoError, "a positional write transferred no bytes");
    written += transferred;
  }
  return Status{};
}

Status FileHandle::flush() {
  if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return Status(StatusCode::StoreIoError, "flushing file contents to the device failed");
  }
  return Status{};
}

Status FileHandle::truncate(std::uint64_t length) {
  FILE_END_OF_FILE_INFO information{};
  information.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
  if (::SetFileInformationByHandle(static_cast<HANDLE>(handle_), FileEndOfFileInfo, &information,
                                   sizeof(information)) == 0) {
    return Status(StatusCode::StoreIoError, "truncating the file failed");
  }
  return Status{};
}

Status FileHandle::read_at(std::uint64_t offset, std::size_t count, std::vector<std::uint8_t>& out) const {
  out.assign(count, 0U);
  std::size_t read_total = 0U;
  while (read_total < count) {
    const std::size_t remaining = count - read_total;
    const DWORD chunk = static_cast<DWORD>(remaining > kIoChunkBytes ? kIoChunkBytes : remaining);
    const std::uint64_t position = offset + read_total;
    DWORD transferred = 0U;
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>((position >> 32U) & 0xFFFFFFFFULL);
    if (::ReadFile(static_cast<HANDLE>(handle_), out.data() + read_total, chunk, &transferred, &overlapped) == 0) {
      return Status(StatusCode::StoreIoError, "a positional read failed");
    }
    if (transferred == 0U) {
      return Status(StatusCode::TruncatedPayload, "the file ended before the requested range");
    }
    read_total += transferred;
  }
  return Status{};
}

Status flush_directory(const std::filesystem::path& directory) {
  // A directory cannot be opened for flushing on Windows: the replacement is made
  // durable by FlushFileBuffers on the file contents plus MOVEFILE_WRITE_THROUGH on
  // the rename, both of which the publication path performs.
  (void)(directory);
  return Status{};
}

Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (::ReplaceFileW(destination.c_str(), source.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr,
                     nullptr) != 0) {
    return Status{};
  }
  // ReplaceFile requires the destination to exist. A first publication, or a store
  // whose manifest an operator removed, falls back to MoveFileEx, which is itself an
  // atomic replacement within one volume.
  if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return Status{};
  }
  return Status(StatusCode::StoreIoError, "publishing the file could not be completed atomically");
}

#else  // POSIX

Result<FileHandle> FileHandle::open_read(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return Status(StatusCode::StoreIoError, "file could not be opened for reading");
  return FileHandle(reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor)));
}

Result<FileHandle> FileHandle::open_write_append(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (descriptor < 0) return Status(StatusCode::StoreIoError, "file could not be opened for writing");
  return FileHandle(reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor)));
}

Result<FileHandle> FileHandle::create_truncate(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) return Status(StatusCode::StoreIoError, "file could not be created for writing");
  return FileHandle(reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor)));
}

std::uint64_t FileHandle::size() const {
  struct stat information {};
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  if (::fstat(descriptor, &information) != 0) return 0U;
  return static_cast<std::uint64_t>(information.st_size);
}

Status FileHandle::write_at(std::uint64_t offset, std::span<const std::uint8_t> data) {
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  std::size_t written = 0U;
  while (written < data.size()) {
    const ssize_t transferred = ::pwrite(descriptor, data.data() + written, data.size() - written,
                                         static_cast<off_t>(offset + written));
    if (transferred < 0) {
      if (errno == EINTR) continue;
      return Status(StatusCode::StoreIoError, "a positional write failed");
    }
    if (transferred == 0) return Status(StatusCode::StoreIoError, "a positional write transferred no bytes");
    written += static_cast<std::size_t>(transferred);
  }
  return Status{};
}

Status FileHandle::flush() {
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  if (::fsync(descriptor) != 0) {
    return Status(StatusCode::StoreIoError, "flushing file contents to the device failed");
  }
  return Status{};
}

Status FileHandle::truncate(std::uint64_t length) {
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  if (::ftruncate(descriptor, static_cast<off_t>(length)) != 0) {
    return Status(StatusCode::StoreIoError, "truncating the file failed");
  }
  return Status{};
}

Status FileHandle::read_at(std::uint64_t offset, std::size_t count, std::vector<std::uint8_t>& out) const {
  out.assign(count, 0U);
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  std::size_t read_total = 0U;
  while (read_total < count) {
    const ssize_t transferred = ::pread(descriptor, out.data() + read_total, count - read_total,
                                        static_cast<off_t>(offset + read_total));
    if (transferred < 0) {
      if (errno == EINTR) continue;
      return Status(StatusCode::StoreIoError, "a positional read failed");
    }
    if (transferred == 0) {
      return Status(StatusCode::TruncatedPayload, "the file ended before the requested range");
    }
    read_total += static_cast<std::size_t>(transferred);
  }
  return Status{};
}

Status flush_directory(const std::filesystem::path& directory) {
  // On POSIX an atomic rename becomes durable by flushing the containing directory.
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    return Status(StatusCode::StoreIoError, "the store directory could not be opened for flushing");
  }
  const int result = ::fsync(descriptor);
  (void)(::close(descriptor));
  if (result != 0) return Status(StatusCode::StoreIoError, "flushing the store directory failed");
  return Status{};
}

Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (::rename(source.c_str(), destination.c_str()) != 0) {
    return Status(StatusCode::StoreIoError, "publishing the file could not be completed atomically");
  }
  return Status{};
}

#endif  // defined(_WIN32)

Status FileHandle::append(std::span<const std::uint8_t> data) {
  // An append derives its offset from the current length, so the offset used for the
  // write is exactly the offset used for the read-back verification.
  const std::uint64_t offset = size();
  const Status status = write_at(offset, data);
  if (!status.ok()) return status;
  if (size() != offset + data.size()) {
    return Status(StatusCode::StoreIoError, "an append did not extend the file as expected");
  }
  return Status{};
}

Result<Digest> digest_of_file(const std::filesystem::path& path) {
  const Result<FileHandle> handle = FileHandle::open_read(path);
  if (!handle.ok()) return handle.status();
  Sha256 hasher;
  const std::uint64_t length = handle.value().size();
  std::uint64_t offset = 0U;
  std::vector<std::uint8_t> buffer;
  while (offset < length) {
    const std::uint64_t remaining = length - offset;
    const std::size_t chunk =
        static_cast<std::size_t>(remaining > static_cast<std::uint64_t>(kIoChunkBytes) ? kIoChunkBytes : remaining);
    const Status status = handle.value().read_at(offset, chunk, buffer);
    if (!status.ok()) return status;
    hasher.update(std::span<const std::uint8_t>(buffer.data(), buffer.size()));
    offset += chunk;
  }
  return Digest(hasher.finish());
}

}  // namespace internal
}  // namespace resource_envelope
