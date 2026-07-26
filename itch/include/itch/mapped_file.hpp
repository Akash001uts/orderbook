#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ob::itch {

// A read only memory mapping of a file.
//
// Why map rather than read. A full trading day of TotalView-ITCH is several
// gigabytes. Reading it into a buffer costs that much resident memory plus a copy
// of every byte, and the copy is pure waste because the parser is zero copy: it
// hands out spans into whatever memory the bytes already live in. Mapping lets the
// kernel page in what the parser actually touches and drop it again under
// pressure, so replay memory stays roughly constant regardless of file size.
//
// Platform note. mmap is the POSIX call and CreateFileMapping plus MapViewOfFile
// is the Windows equivalent. Both are implemented, because the development host for
// this project is Windows while the benchmark methodology in BENCHMARKS.md targets
// Linux, and a parser that only compiles on one of them would be untestable on the
// other. The abstraction is thin on purpose: open, size, span, close.
class MappedFile {
 public:
  MappedFile() noexcept = default;

  ~MappedFile() noexcept;

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  // False on failure, with error() describing why. No exception is thrown: a
  // missing file is an ordinary outcome for a tool that takes a path as an
  // argument, not an exceptional one.
  [[nodiscard]] bool open(const std::string& path);

  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] std::string_view error() const noexcept { return error_; }

 private:
  // Takes an lvalue reference rather than an rvalue one because it steals the
  // fields individually rather than moving the object as a whole, which is what a
  // platform handle owner has to do.
  void adopt(MappedFile& other) noexcept;

  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
  std::string error_;

  // Platform handles, kept as void pointers and an int so that this header pulls
  // in neither <windows.h> nor <sys/mman.h>. Including windows.h from a header
  // would drag several thousand macros into every translation unit that touches
  // ITCH, and one of them is named min.
  void* mapping_handle_ = nullptr;
  void* file_handle_ = nullptr;
  int file_descriptor_ = -1;
};

}  // namespace ob::itch
