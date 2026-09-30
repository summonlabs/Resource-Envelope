#ifndef RESOURCE_ENVELOPE_INTERNAL_STORE_IO_HPP
#define RESOURCE_ENVELOPE_INTERNAL_STORE_IO_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "resource_envelope/digest.hpp"
#include "resource_envelope/result.hpp"

namespace resource_envelope {
namespace internal {

// A single open file. Reads and writes are positional, so a torn write is always a
// torn write at a known offset, and a read-back verification compares exactly the
// bytes that were written. Nothing in the store reads back through an operating
// system cache assumption: flush() reaches the device, and verify() re-reads the
// file contents from the handle.
class FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle();

  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;

  [[nodiscard]] static Result<FileHandle> open_read(const std::filesystem::path& path);
  [[nodiscard]] static Result<FileHandle> open_write_append(const std::filesystem::path& path);
  [[nodiscard]] static Result<FileHandle> create_truncate(const std::filesystem::path& path);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t size() const;
  [[nodiscard]] Status write_at(std::uint64_t offset, std::span<const std::uint8_t> data);
  [[nodiscard]] Status append(std::span<const std::uint8_t> data);
  [[nodiscard]] Status flush();
  [[nodiscard]] Status truncate(std::uint64_t size);

  // Reads exactly `count` bytes at `offset`. A short read is reported as a failure
  // rather than returning a shorter buffer.
  [[nodiscard]] Status read_at(std::uint64_t offset, std::size_t count, std::vector<std::uint8_t>& out) const;

 private:
  explicit FileHandle(void* handle) noexcept;
  void close() noexcept;

  void* handle_ = nullptr;
};

// Flushes the directory entry so that a rename is durable. On platforms where this
// is not meaningful the call succeeds and the caller documents the difference.
[[nodiscard]] Status flush_directory(const std::filesystem::path& directory);

// Replaces `destination` with `source` atomically. Returns false when the platform
// cannot replace an existing file in one step, in which case the caller performs the
// documented two-step fallback.
[[nodiscard]] Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination);

// Digest of a file's contents, used to verify a read-back against what was written.
[[nodiscard]] Result<Digest> digest_of_file(const std::filesystem::path& path);

}  // namespace internal
}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_INTERNAL_STORE_IO_HPP
