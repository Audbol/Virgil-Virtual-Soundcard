// Cross-process shared memory segment (POSIX shm_open / Win32 file mapping).
#pragma once

#include <cstddef>
#include <string>

namespace dsv {

class SharedMemory {
 public:
  SharedMemory() = default;
  ~SharedMemory();
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

  // Daemon side: create (replacing any stale segment) and zero-fill.
  bool create(const std::string& name, size_t size);
  // Driver side: map an existing segment in full.
  bool open(const std::string& name);
  void close();

  void* data() const { return data_; }
  size_t size() const { return size_; }
  bool is_open() const { return data_ != nullptr; }
  const std::string& name() const { return name_; }

 private:
  std::string name_;
  void* data_ = nullptr;
  size_t size_ = 0;
  bool owner_ = false;
#if defined(_WIN32)
  void* mapping_ = nullptr;
#endif
};

}  // namespace dsv
