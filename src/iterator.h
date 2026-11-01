#pragma once
// Forward iterators over internal keys (internal) and user keys (public).

#include <memory>
#include <string_view>
#include <vector>

#include "sediment/status.h"

namespace sediment {

class Iterator {
 public:
  virtual ~Iterator() = default;
  [[nodiscard]] virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Positions at the first entry with key >= target.
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  [[nodiscard]] virtual std::string_view key() const = 0;
  [[nodiscard]] virtual std::string_view value() const = 0;
  [[nodiscard]] virtual Status status() const { return Status::OK(); }
};

// Merges sorted children by internal key (a binary heap would pay off only
// for many children; we have memtables + L0 files + one per level).
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

std::unique_ptr<Iterator> NewEmptyIterator(Status s = Status::OK());

}  // namespace sediment
