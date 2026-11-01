#pragma once
// Thin POSIX file layer.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sediment/status.h"

namespace sediment {

class WritableFile {
 public:
  static Status Open(const std::string& path, std::unique_ptr<WritableFile>* out, bool truncate = true);
  ~WritableFile();
  Status Append(std::string_view data);
  // Durable flush. On macOS this is F_FULLFSYNC (plain fsync only reaches the
  // drive's cache); elsewhere fdatasync.
  Status Sync();
  Status Close();
  [[nodiscard]] uint64_t Size() const { return size_; }

 private:
  WritableFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  int fd_;
  std::string path_;
  uint64_t size_ = 0;
};

// Read-only memory map of a whole file.
class MappedFile {
 public:
  static Status Open(const std::string& path, std::unique_ptr<MappedFile>* out);
  ~MappedFile();
  [[nodiscard]] std::string_view data() const { return {static_cast<const char*>(base_), size_}; }

 private:
  MappedFile(void* base, size_t size) : base_(base), size_(size) {}
  void* base_;
  size_t size_;
};

Status ReadFileToString(const std::string& path, std::string* out);
Status WriteStringToFileSync(const std::string& path, std::string_view data);
Status RenameFile(const std::string& from, const std::string& to);
Status RemoveFile(const std::string& path);
Status SyncDir(const std::string& dir);
Status CreateDir(const std::string& dir);
Status ListDir(const std::string& dir, std::vector<std::string>* names);
bool FileExists(const std::string& path);

// Exclusive advisory lock held for the lifetime of the returned fd.
Status LockFile(const std::string& path, int* fd);
void UnlockFile(int fd);

}  // namespace sediment
