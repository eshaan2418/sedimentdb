#include "version.h"

#include <algorithm>
#include <cstdio>

#include "coding.h"

namespace sediment {

uint64_t Version::LevelBytes(int level) const {
  uint64_t n = 0;
  for (const auto& f : files[level]) n += f.size;
  return n;
}

namespace {
enum Tag : uint32_t { kLogNumber = 1, kNextFile = 2, kLastSeq = 3, kDeleted = 4, kAdded = 5 };
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_log_number) PutVarint32(dst, kLogNumber), PutVarint64(dst, log_number);
  if (has_next_file) PutVarint32(dst, kNextFile), PutVarint64(dst, next_file);
  if (has_last_seq) PutVarint32(dst, kLastSeq), PutVarint64(dst, last_seq);
  for (const auto& [level, num] : deleted) {
    PutVarint32(dst, kDeleted);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, num);
  }
  for (const auto& [level, f] : added) {
    PutVarint32(dst, kAdded);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.size);
    PutLengthPrefixed(dst, f.smallest);
    PutLengthPrefixed(dst, f.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view in) {
  while (!in.empty()) {
    uint32_t tag, level;
    if (!GetVarint32(&in, &tag)) return Status::Corruption("manifest: bad tag");
    switch (tag) {
      case kLogNumber:
        if (!GetVarint64(&in, &log_number)) return Status::Corruption("manifest: log number");
        has_log_number = true;
        break;
      case kNextFile:
        if (!GetVarint64(&in, &next_file)) return Status::Corruption("manifest: next file");
        has_next_file = true;
        break;
      case kLastSeq:
        if (!GetVarint64(&in, &last_seq)) return Status::Corruption("manifest: last seq");
        has_last_seq = true;
        break;
      case kDeleted: {
        uint64_t n;
        if (!GetVarint32(&in, &level) || !GetVarint64(&in, &n) || level >= kNumLevels)
          return Status::Corruption("manifest: deleted file");
        deleted.emplace_back(static_cast<int>(level), n);
        break;
      }
      case kAdded: {
        FileMeta f;
        std::string_view s, l;
        if (!GetVarint32(&in, &level) || !GetVarint64(&in, &f.number) || !GetVarint64(&in, &f.size) ||
            !GetLengthPrefixed(&in, &s) || !GetLengthPrefixed(&in, &l) || level >= kNumLevels)
          return Status::Corruption("manifest: added file");
        f.smallest.assign(s);
        f.largest.assign(l);
        added.emplace_back(static_cast<int>(level), std::move(f));
        break;
      }
      default: return Status::Corruption("manifest: unknown tag");
    }
  }
  return Status::OK();
}

std::shared_ptr<const Version> ApplyEdit(const Version& base, const VersionEdit& e) {
  auto v = std::make_shared<Version>(base);
  for (const auto& [level, num] : e.deleted) {
    auto& fs = v->files[level];
    fs.erase(std::remove_if(fs.begin(), fs.end(), [n = num](const FileMeta& f) { return f.number == n; }), fs.end());
  }
  for (const auto& [level, f] : e.added) v->files[level].push_back(f);
  // L0 is ordered by file number (newest last); deeper levels by key.
  std::sort(v->files[0].begin(), v->files[0].end(),
            [](const FileMeta& a, const FileMeta& b) { return a.number < b.number; });
  for (int l = 1; l < kNumLevels; ++l) {
    std::sort(v->files[l].begin(), v->files[l].end(),
              [](const FileMeta& a, const FileMeta& b) { return CompareInternal(a.smallest, b.smallest) < 0; });
  }
  return v;
}

namespace {
std::string Numbered(const std::string& db, uint64_t n, const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "/%06llu%s", static_cast<unsigned long long>(n), suffix);
  return db + buf;
}
}  // namespace

std::string TableFileName(const std::string& db, uint64_t n) { return Numbered(db, n, ".sst"); }
std::string LogFileName(const std::string& db, uint64_t n) { return Numbered(db, n, ".log"); }
std::string ManifestFileName(const std::string& db, uint64_t n) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "/MANIFEST-%06llu", static_cast<unsigned long long>(n));
  return db + buf;
}
std::string CurrentFileName(const std::string& db) { return db + "/CURRENT"; }

bool ParseFileName(const std::string& name, uint64_t* number, FileType* type) {
  if (name == "CURRENT") return *type = FileType::kCurrent, *number = 0, true;
  if (name == "LOCK") return *type = FileType::kLock, *number = 0, true;
  auto parse_num = [](std::string_view s, uint64_t* n) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
      if (c < '0' || c > '9') return false;
      v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    *n = v;
    return true;
  };
  std::string_view s(name);
  if (s.starts_with("MANIFEST-")) {
    *type = FileType::kManifest;
    return parse_num(s.substr(9), number);
  }
  auto dot = s.rfind('.');
  if (dot == std::string_view::npos) return false;
  std::string_view ext = s.substr(dot + 1);
  if (ext == "sst")
    *type = FileType::kTable;
  else if (ext == "log")
    *type = FileType::kLog;
  else if (ext == "tmp")
    *type = FileType::kTemp;
  else
    return false;
  return parse_num(s.substr(0, dot), number);
}

}  // namespace sediment
