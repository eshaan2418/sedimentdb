#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sediment/status.h"

namespace sediment {

// An atomic group of updates. Encoding (also the WAL record payload):
//   fixed64 sequence | fixed32 count | (type byte | lenprefixed key [| lenprefixed value])*
class WriteBatch {
 public:
  WriteBatch();
  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();
  [[nodiscard]] uint32_t Count() const;
  [[nodiscard]] size_t ByteSize() const { return rep_.size(); }

  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };
  [[nodiscard]] Status Iterate(Handler* h) const;

 private:
  friend class WriteBatchInternal;
  std::string rep_;
};

}  // namespace sediment
