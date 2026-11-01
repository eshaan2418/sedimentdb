#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "sediment/status.h"
#include "sediment/write_batch.h"

namespace sediment {

struct Options {
  bool create_if_missing = true;
  bool error_if_exists = false;
  size_t write_buffer_size = 4 << 20;    // memtable size before it is flushed
  size_t block_size = 4096;              // uncompressed data block size
  int bloom_bits_per_key = 10;           // ~1% false positives
  int l0_compaction_trigger = 4;         // L0 files that start a compaction
  int l0_slowdown_trigger = 8;           // ...that delay each write by 1 ms
  int l0_stop_trigger = 12;              // ...that stall writes
  uint64_t max_bytes_level1 = 10 << 20;  // L(n+1) target = 10 * L(n)
  uint64_t target_file_size = 2 << 20;   // compaction output file size
};

struct WriteOptions {
  // Wait for the WAL record to reach stable storage before returning.
  bool sync = false;
};

class Snapshot {
 public:
  [[nodiscard]] uint64_t sequence() const { return seq_; }

 private:
  friend class DBImpl;
  explicit Snapshot(uint64_t s) : seq_(s) {}
  uint64_t seq_;
};

struct ReadOptions {
  const Snapshot* snapshot = nullptr;  // nullptr = latest
};

// Forward iterator over user keys in ascending order.
class DBIterator {
 public:
  virtual ~DBIterator() = default;
  [[nodiscard]] virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view key) = 0;
  virtual void Next() = 0;
  [[nodiscard]] virtual std::string_view key() const = 0;
  [[nodiscard]] virtual std::string_view value() const = 0;
  [[nodiscard]] virtual Status status() const = 0;
};

class DB {
 public:
  static Status Open(const Options& opts, const std::string& path, std::unique_ptr<DB>* db);
  virtual ~DB() = default;

  virtual Status Put(const WriteOptions& o, std::string_view key, std::string_view value) = 0;
  virtual Status Delete(const WriteOptions& o, std::string_view key) = 0;
  virtual Status Write(const WriteOptions& o, WriteBatch* batch) = 0;
  // Returns NotFound if the key does not exist (or was deleted).
  virtual Status Get(const ReadOptions& o, std::string_view key, std::string* value) = 0;
  virtual std::unique_ptr<DBIterator> NewIterator(const ReadOptions& o) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* s) = 0;

  // Flushes the memtable and compacts until no level needs compaction.
  virtual Status CompactAll() = 0;
  // Human-readable per-level file counts and sizes.
  virtual std::string Stats() = 0;
  [[nodiscard]] virtual int NumFilesAtLevel(int level) = 0;
};

// Deletes all files of a database (which must not be open).
Status DestroyDB(const std::string& path);

}  // namespace sediment
