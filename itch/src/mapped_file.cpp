#include "itch/mapped_file.hpp"

#include <cstddef>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ob::itch {

MappedFile::~MappedFile() noexcept {
  close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept {
  adopt(other);
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    close();
    adopt(other);
  }
  return *this;
}

void MappedFile::adopt(MappedFile& other) noexcept {
  data_ = other.data_;
  size_ = other.size_;
  error_ = std::move(other.error_);
  mapping_handle_ = other.mapping_handle_;
  file_handle_ = other.file_handle_;
  file_descriptor_ = other.file_descriptor_;

  other.data_ = nullptr;
  other.size_ = 0;
  other.mapping_handle_ = nullptr;
  other.file_handle_ = nullptr;
  other.file_descriptor_ = -1;
}

#ifdef _WIN32

bool MappedFile::open(const std::string& path) {
  close();

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) Win32 returns
  // opaque HANDLEs, which are stored here as void* so this header does not have to
  // include windows.h. The round trip through void* is what that costs.
  HANDLE file = CreateFileA(path.c_str(),
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error_ = "CreateFileA failed for " + path;
    return false;
  }

  LARGE_INTEGER file_size{};
  if (GetFileSizeEx(file, &file_size) == 0) {
    CloseHandle(file);
    error_ = "GetFileSizeEx failed for " + path;
    return false;
  }

  // A zero length file maps to nothing on Windows, and an empty mapping is an
  // error rather than an empty view, so it is handled before the mapping call.
  if (file_size.QuadPart == 0) {
    CloseHandle(file);
    size_ = 0;
    data_ = nullptr;
    return true;
  }

  HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    CloseHandle(file);
    error_ = "CreateFileMappingA failed for " + path;
    return false;
  }

  const LPVOID view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (view == nullptr) {
    CloseHandle(mapping);
    CloseHandle(file);
    error_ = "MapViewOfFile failed for " + path;
    return false;
  }

  data_ = static_cast<const std::byte*>(view);
  size_ = static_cast<std::size_t>(file_size.QuadPart);
  mapping_handle_ = mapping;
  file_handle_ = file;
  return true;
}

void MappedFile::close() noexcept {
  if (data_ != nullptr) {
    UnmapViewOfFile(data_);
  }
  if (mapping_handle_ != nullptr) {
    CloseHandle(mapping_handle_);
  }
  if (file_handle_ != nullptr) {
    CloseHandle(file_handle_);
  }

  data_ = nullptr;
  size_ = 0;
  mapping_handle_ = nullptr;
  file_handle_ = nullptr;
}

#else

bool MappedFile::open(const std::string& path) {
  close();

  // POSIX open is variadic, because the third mode argument is only meaningful
  // with O_CREAT. There is no non-variadic spelling of it to prefer.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    error_ = "open failed for " + path;
    return false;
  }

  // Written as `= {}` rather than `status{}` because clang-format 18 and 22
  // disagree about how to format brace initialisation of an elaborated type
  // specifier, and CI runs 18 while this project's development host runs 22.
  struct stat file_status = {};
  if (::fstat(descriptor, &file_status) != 0) {
    ::close(descriptor);
    error_ = "fstat failed for " + path;
    return false;
  }

  if (file_status.st_size == 0) {
    ::close(descriptor);
    size_ = 0;
    data_ = nullptr;
    return true;
  }

  const auto length = static_cast<std::size_t>(file_status.st_size);

  // MAP_PRIVATE rather than MAP_SHARED because the mapping is read only and
  // nothing here writes back. MADV_SEQUENTIAL tells the kernel the access pattern,
  // which is worth stating: replay walks the file front to back exactly once, so
  // aggressive readahead helps and retaining pages behind the cursor does not.
  void* const view = ::mmap(nullptr, length, PROT_READ, MAP_PRIVATE, descriptor, 0);
  if (view == MAP_FAILED) {
    ::close(descriptor);
    error_ = "mmap failed for " + path;
    return false;
  }

  ::madvise(view, length, MADV_SEQUENTIAL);

  data_ = static_cast<const std::byte*>(view);
  size_ = length;
  file_descriptor_ = descriptor;
  return true;
}

void MappedFile::close() noexcept {
  if (data_ != nullptr && size_ != 0) {
    // munmap takes a non-const pointer, and the mapping was created by this
    // object, so casting the const away is dropping a qualifier this class added
    // rather than one the platform imposed.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
  }
  if (file_descriptor_ >= 0) {
    ::close(file_descriptor_);
  }

  data_ = nullptr;
  size_ = 0;
  file_descriptor_ = -1;
}

#endif

}  // namespace ob::itch
