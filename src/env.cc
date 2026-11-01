#include "env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace sediment {

namespace {
Status PosixError(const std::string& ctx) { return Status::IOError(ctx + ": " + std::strerror(errno)); }
}  // namespace

Status WritableFile::Open(const std::string& path, std::unique_ptr<WritableFile>* out, bool truncate) {
  int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (truncate ? O_TRUNC : O_APPEND);
  int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) return PosixError(path);
  out->reset(new WritableFile(fd, path));
  if (!truncate) {
    struct stat st{};
    if (::fstat(fd, &st) == 0) (*out)->size_ = static_cast<uint64_t>(st.st_size);
  }
  return Status::OK();
}

WritableFile::~WritableFile() {
  if (fd_ >= 0) ::close(fd_);
}

Status WritableFile::Append(std::string_view data) {
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_);
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  size_ += data.size();
  return Status::OK();
}

Status WritableFile::Sync() {
#ifdef F_FULLFSYNC
  if (::fcntl(fd_, F_FULLFSYNC) == 0) return Status::OK();
  // Some filesystems do not support F_FULLFSYNC; fall back to fsync.
#endif
#if defined(__linux__)
  if (::fdatasync(fd_) != 0) return PosixError(path_);
#else
  if (::fsync(fd_) != 0) return PosixError(path_);
#endif
  return Status::OK();
}

Status WritableFile::Close() {
  if (fd_ >= 0 && ::close(fd_) != 0) {
    fd_ = -1;
    return PosixError(path_);
  }
  fd_ = -1;
  return Status::OK();
}

Status MappedFile::Open(const std::string& path, std::unique_ptr<MappedFile>* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return PosixError(path);
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return PosixError(path);
  }
  auto size = static_cast<size_t>(st.st_size);
  void* base = nullptr;
  if (size > 0) {
    base = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
      ::close(fd);
      return PosixError(path);
    }
  }
  ::close(fd);  // the mapping stays valid, even after the file is unlinked
  out->reset(new MappedFile(base, size));
  return Status::OK();
}

MappedFile::~MappedFile() {
  if (base_ != nullptr) ::munmap(base_, size_);
}

Status ReadFileToString(const std::string& path, std::string* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return PosixError(path);
  out->clear();
  char buf[65536];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return PosixError(path);
    }
    if (n == 0) break;
    out->append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return Status::OK();
}

Status WriteStringToFileSync(const std::string& path, std::string_view data) {
  std::unique_ptr<WritableFile> f;
  Status s = WritableFile::Open(path, &f);
  if (s.ok()) s = f->Append(data);
  if (s.ok()) s = f->Sync();
  if (s.ok()) s = f->Close();
  if (!s.ok()) ::unlink(path.c_str());
  return s;
}

Status RenameFile(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) return PosixError(from);
  return Status::OK();
}

Status RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0) return PosixError(path);
  return Status::OK();
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return PosixError(dir);
  int r = ::fsync(fd);
  ::close(fd);
  if (r != 0) return PosixError(dir);
  return Status::OK();
}

Status CreateDir(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return PosixError(dir);
  return Status::OK();
}

Status ListDir(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return PosixError(dir);
  while (struct dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n != "." && n != "..") names->push_back(n);
  }
  ::closedir(d);
  return Status::OK();
}

bool FileExists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

Status LockFile(const std::string& path, int* fd) {
  int f = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (f < 0) return PosixError(path);
  if (::flock(f, LOCK_EX | LOCK_NB) != 0) {
    ::close(f);
    return Status::IOError("database is locked by another process: " + path);
  }
  *fd = f;
  return Status::OK();
}

void UnlockFile(int fd) {
  ::flock(fd, LOCK_UN);
  ::close(fd);
}

}  // namespace sediment
