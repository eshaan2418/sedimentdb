#pragma once

#include "dbformat.h"
#include "memtable.h"
#include "sediment/write_batch.h"

namespace sediment {

class WriteBatchInternal {
 public:
  static SequenceNumber Sequence(const WriteBatch& b);
  static void SetSequence(WriteBatch* b, SequenceNumber s);
  static std::string_view Contents(const WriteBatch& b);
  static Status SetContents(WriteBatch* b, std::string_view c);
  static void Append(WriteBatch* dst, const WriteBatch& src);
  static Status InsertInto(const WriteBatch& b, MemTable* m);
};

}  // namespace sediment
