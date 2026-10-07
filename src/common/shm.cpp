#include "dsv/shm.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dsv {

SharedMemory::~SharedMemory() { close(); }

#if defined(_WIN32)

bool SharedMemory::create(const std::string& name, size_t size) {
  close();
  HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                DWORD(uint64_t(size) >> 32), DWORD(size & 0xffffffffu),
                                name.c_str());
  if (!h) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Another daemon instance owns it.
    CloseHandle(h);
    return false;
  }
  void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!p) {
    CloseHandle(h);
    return false;
  }
  VirtualLock(p, size);
  std::memset(p, 0, size);
  mapping_ = h;
  data_ = p;
  size_ = size;
  name_ = name;
  owner_ = true;
  return true;
}

bool SharedMemory::open(const std::string& name) {
  close();
  HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!h) return false;
  void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!p) {
    CloseHandle(h);
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi;
  VirtualQuery(p, &mbi, sizeof mbi);
  mapping_ = h;
  data_ = p;
  size_ = mbi.RegionSize;
  name_ = name;
  owner_ = false;
  return true;
}

void SharedMemory::close() {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle(mapping_);
  data_ = nullptr;
  mapping_ = nullptr;
  size_ = 0;
}

#else

bool SharedMemory::create(const std::string& name, size_t size) {
  close();
  shm_unlink(name.c_str());  // stale segment from a crashed daemon
  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
  if (fd < 0) return false;
  fchmod(fd, 0666);  // umask must not keep other users' audio apps out
  if (ftruncate(fd, off_t(size)) != 0) {
    ::close(fd);
    shm_unlink(name.c_str());
    return false;
  }
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    shm_unlink(name.c_str());
    return false;
  }
  std::memset(p, 0, size);  // also faults every page in before mlock
  mlock(p, size);
  data_ = p;
  size_ = size;
  name_ = name;
  owner_ = true;
  return true;
}

bool SharedMemory::open(const std::string& name) {
  close();
  int fd = shm_open(name.c_str(), O_RDWR, 0);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    ::close(fd);
    return false;
  }
  void* p = mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return false;
  data_ = p;
  size_ = size_t(st.st_size);
  name_ = name;
  owner_ = false;
  return true;
}

void SharedMemory::close() {
  if (data_) munmap(data_, size_);
  if (owner_ && !name_.empty()) shm_unlink(name_.c_str());
  data_ = nullptr;
  size_ = 0;
  owner_ = false;
}

#endif

}  // namespace dsv
