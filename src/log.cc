#include "log.h"

#include "coding.h"

namespace sediment {

Status LogWriter::AddRecord(std::string_view payload) {
  std::string rec;
  rec.reserve(payload.size() + 8);
  PutFixed32(&rec, crc32c::Mask(crc32c::Value(payload.data(), payload.size())));
  PutFixed32(&rec, static_cast<uint32_t>(payload.size()));
  rec.append(payload);
  return f_->Append(rec);  // one write(2) per record (one per commit group)
}

bool LogReader::ReadRecord(std::string_view* record) {
  if (pos_ == data_.size()) return false;
  if (data_.size() - pos_ < 8) {
    corrupt_ = true;
    return false;
  }
  uint32_t crc = crc32c::Unmask(DecodeFixed32(data_.data() + pos_));
  uint32_t len = DecodeFixed32(data_.data() + pos_ + 4);
  if (data_.size() - pos_ - 8 < len) {
    corrupt_ = true;
    return false;
  }
  const char* p = data_.data() + pos_ + 8;
  if (crc32c::Value(p, len) != crc) {
    corrupt_ = true;
    return false;
  }
  *record = {p, len};
  pos_ += 8 + len;
  return true;
}

}  // namespace sediment
