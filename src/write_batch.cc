#include "coding.h"
#include "dbformat.h"
#include "write_batch_internal.h"

namespace sediment {

namespace {
constexpr size_t kHeader = 12;
}

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() { rep_.assign(kHeader, '\0'); }

uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data() + 8); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  EncodeFixed32(rep_.data() + 8, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  EncodeFixed32(rep_.data() + 8, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixed(&rep_, key);
}

Status WriteBatch::Iterate(Handler* h) const {
  if (rep_.size() < kHeader) return Status::Corruption("write batch too small");
  std::string_view in(rep_);
  in.remove_prefix(kHeader);
  uint32_t n = 0;
  while (!in.empty()) {
    char tag = in[0];
    in.remove_prefix(1);
    std::string_view k, v;
    if (tag == kTypeValue) {
      if (!GetLengthPrefixed(&in, &k) || !GetLengthPrefixed(&in, &v)) return Status::Corruption("bad put");
      h->Put(k, v);
    } else if (tag == kTypeDeletion) {
      if (!GetLengthPrefixed(&in, &k)) return Status::Corruption("bad delete");
      h->Delete(k);
    } else {
      return Status::Corruption("unknown write batch tag");
    }
    ++n;
  }
  if (n != Count()) return Status::Corruption("write batch count mismatch");
  return Status::OK();
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch& b) { return DecodeFixed64(b.rep_.data()); }
void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber s) { EncodeFixed64(b->rep_.data(), s); }
std::string_view WriteBatchInternal::Contents(const WriteBatch& b) { return b.rep_; }
Status WriteBatchInternal::SetContents(WriteBatch* b, std::string_view c) {
  if (c.size() < kHeader) return Status::Corruption("write batch too small");
  b->rep_.assign(c);
  return Status::OK();
}
void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch& src) {
  EncodeFixed32(dst->rep_.data() + 8, dst->Count() + src.Count());
  dst->rep_.append(src.rep_.data() + kHeader, src.rep_.size() - kHeader);
}

namespace {
class Inserter final : public WriteBatch::Handler {
 public:
  Inserter(MemTable* m, SequenceNumber s) : m_(m), seq_(s) {}
  void Put(std::string_view k, std::string_view v) override { m_->Add(seq_++, kTypeValue, k, v); }
  void Delete(std::string_view k) override { m_->Add(seq_++, kTypeDeletion, k, {}); }

 private:
  MemTable* m_;
  SequenceNumber seq_;
};
}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch& b, MemTable* m) {
  Inserter ins(m, Sequence(b));
  return b.Iterate(&ins);
}

}  // namespace sediment
