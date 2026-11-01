#pragma once
// The set of live table files per level, and the MANIFEST that records
// changes to it durably.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dbformat.h"
#include "log.h"
#include "table.h"

namespace sediment {

struct FileMeta {
  uint64_t number = 0;
  uint64_t size = 0;
  std::string smallest;  // internal keys
  std::string largest;
  // The open (mmap'd) table. Versions own their tables, so a reader holding
  // a version can always read its files, even after compaction unlinks them.
  std::shared_ptr<Table> table;
};

// Immutable once published; readers hold it via shared_ptr.
struct Version {
  std::vector<FileMeta> files[kNumLevels];  // L0: newest last; L1+: sorted by key, disjoint
  [[nodiscard]] uint64_t LevelBytes(int level) const;
  [[nodiscard]] int NumFiles(int level) const { return static_cast<int>(files[level].size()); }
};

struct VersionEdit {
  bool has_log_number = false, has_next_file = false, has_last_seq = false;
  uint64_t log_number = 0, next_file = 0;
  SequenceNumber last_seq = 0;
  std::vector<std::pair<int, uint64_t>> deleted;
  std::vector<std::pair<int, FileMeta>> added;

  void SetLogNumber(uint64_t n) { has_log_number = true, log_number = n; }
  void SetNextFile(uint64_t n) { has_next_file = true, next_file = n; }
  void SetLastSequence(SequenceNumber s) { has_last_seq = true, last_seq = s; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);
};

// Applies an edit to a version, producing a new one.
std::shared_ptr<const Version> ApplyEdit(const Version& base, const VersionEdit& e);

std::string TableFileName(const std::string& db, uint64_t n);
std::string LogFileName(const std::string& db, uint64_t n);
std::string ManifestFileName(const std::string& db, uint64_t n);
std::string CurrentFileName(const std::string& db);

// Parses "000123.sst" style names. Returns false for unrelated files.
enum class FileType { kLog, kTable, kManifest, kCurrent, kLock, kTemp };
bool ParseFileName(const std::string& name, uint64_t* number, FileType* type);

}  // namespace sediment
