#pragma once
// Sorted string tables.
//
//   [data block]*  [filter block]  [index block]  [footer]
//
// Data and index blocks hold prefix-compressed entries with a restart point
// every 16 keys:
//   entry   := varint shared | varint non_shared | varint value_len | key delta | value
//   trailer := fixed32 restart_offset * n | fixed32 n
// Every block is followed by fixed32 masked crc32c of its bytes.
// The filter block is one bloom filter over all user keys in the table.
// Index entries map the last internal key of a data block to its handle.
// Footer (40 bytes): fixed64 filter_off, filter_size, index_off, index_size, magic.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "env.h"
#include "iterator.h"

namespace sediment {

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // excluding the 4-byte checksum
};

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval = 16);
  void Add(std::string_view key, std::string_view value);
  std::string_view Finish();
  void Reset();
  [[nodiscard]] size_t SizeEstimate() const { return buf_.size() + restarts_.size() * 4 + 4; }
  [[nodiscard]] bool empty() const { return counter_ == 0 && restarts_.size() == 1 && buf_.empty(); }

 private:
  int interval_;
  std::string buf_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  std::string last_key_;
  bool finished_ = false;
};

// Iterates a block held in caller-owned memory. Keys are internal keys.
std::unique_ptr<Iterator> NewBlockIterator(std::string_view block);

class BloomFilter {
 public:
  static std::string Build(const std::vector<std::string>& keys, int bits_per_key);
  static bool MayContain(std::string_view filter, std::string_view key);
};

struct TableOptions {
  size_t block_size = 4096;
  int bloom_bits_per_key = 10;
};

class TableBuilder {
 public:
  TableBuilder(const TableOptions& opts, std::unique_ptr<WritableFile> file);
  // Keys must be added in strictly increasing internal-key order.
  void Add(std::string_view ikey, std::string_view value);
  Status Finish();  // writes, syncs and closes the file
  [[nodiscard]] uint64_t FileSize() const { return offset_ + pending_.SizeEstimate(); }
  [[nodiscard]] uint64_t NumEntries() const { return entries_; }

 private:
  void FlushBlock();
  BlockHandle WriteRaw(std::string_view contents);

  TableOptions opts_;
  std::unique_ptr<WritableFile> file_;
  BlockBuilder pending_;
  BlockBuilder index_{1};
  std::string last_key_;
  std::vector<std::string> filter_keys_;
  uint64_t offset_ = 0;
  uint64_t entries_ = 0;
  Status status_;
};

class Table {
 public:
  static Status Open(const std::string& path, std::shared_ptr<Table>* out);

  // Seeks to the first entry >= lookup key. If it has the same user key,
  // calls handle(found ikey, value). The bloom filter is checked first.
  Status Get(std::string_view lookup_ikey, bool* found, std::string* ikey, std::string* value) const;

  // The iterator keeps the table alive (and the file mapped) via shared_ptr.
  static std::unique_ptr<Iterator> NewIterator(std::shared_ptr<const Table> t);

  [[nodiscard]] Status ReadBlock(const BlockHandle& h, std::string_view* out) const;
  [[nodiscard]] std::string_view index_block() const { return index_; }

 private:
  std::unique_ptr<MappedFile> file_;
  std::string_view index_;
  std::string_view filter_;
};

}  // namespace sediment
