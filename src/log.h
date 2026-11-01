#pragma once
// Append-only record log used for the WAL and the MANIFEST.
// Record: [masked crc32c(payload) u32][length u32][payload].
// A reader stops at the first truncated or corrupt record: anything after
// it was never acknowledged as durable.

#include <memory>
#include <string>

#include "env.h"

namespace sediment {

class LogWriter {
 public:
  explicit LogWriter(std::unique_ptr<WritableFile> f) : f_(std::move(f)) {}
  Status AddRecord(std::string_view payload);
  Status Sync() { return f_->Sync(); }
  Status Close() { return f_->Close(); }

 private:
  std::unique_ptr<WritableFile> f_;
};

class LogReader {
 public:
  explicit LogReader(std::string contents) : data_(std::move(contents)) {}
  // Returns false at the end of valid data.
  bool ReadRecord(std::string_view* record);
  // True if reading stopped because of a torn or corrupt record rather than
  // a clean end of file.
  [[nodiscard]] bool hit_corruption() const { return corrupt_; }
  [[nodiscard]] size_t valid_bytes() const { return pos_; }

 private:
  std::string data_;
  size_t pos_ = 0;
  bool corrupt_ = false;
};

}  // namespace sediment
