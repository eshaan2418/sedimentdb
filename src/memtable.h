#pragma once
// In-memory write buffer: an arena-backed skiplist of encoded entries.
// One writer at a time (the group-commit leader); readers never lock. Next
// pointers are published with release stores and read with acquire loads,
// so a reader sees a fully initialized node or none at all.

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dbformat.h"
#include "iterator.h"

namespace sediment {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  char* Allocate(size_t n);
  char* AllocateAligned(size_t n);
  [[nodiscard]] size_t MemoryUsage() const { return usage_.load(std::memory_order_relaxed); }

 private:
  char* NewBlock(size_t n);
  char* ptr_ = nullptr;
  size_t left_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> usage_{0};
};

class MemTable {
 public:
  MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // Looks up the newest entry for `user_key` with sequence <= `seq`.
  // Returns false if none; otherwise sets *deleted or fills *value.
  bool Get(std::string_view user_key, SequenceNumber seq, std::string* value, bool* deleted) const;

  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;
  [[nodiscard]] size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }
  [[nodiscard]] bool Empty() const;

 private:
  static constexpr int kMaxHeight = 12;
  struct Node;
  friend class MemTableIterator;

  Node* NewNode(const char* entry, int height);
  int RandomHeight();
  // First node whose key >= ikey; fills prev[] if non-null.
  Node* FindGreaterOrEqual(std::string_view ikey, Node** prev) const;

  Arena arena_;
  Node* head_;
  std::atomic<int> max_height_{1};
  std::minstd_rand rng_{0xdecafbad};
};

}  // namespace sediment
