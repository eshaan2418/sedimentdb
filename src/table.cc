#include "table.h"

#include <algorithm>
#include <cmath>

#include "coding.h"
#include "dbformat.h"

namespace sediment {

namespace {
constexpr uint64_t kMagic = 0x5345444d454e5431ull;  // "SEDMENT1"
constexpr size_t kFooterSize = 40;

void PutHandle(std::string* dst, const BlockHandle& h) {
  PutVarint64(dst, h.offset);
  PutVarint64(dst, h.size);
}
bool GetHandle(std::string_view* in, BlockHandle* h) {
  return GetVarint64(in, &h->offset) && GetVarint64(in, &h->size);
}

uint64_t Hash64(std::string_view s) {
  // FNV-1a followed by a murmur3 finalizer: cheap and well mixed.
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}
}  // namespace

// ---------------------------------------------------------------------------
// Blocks

BlockBuilder::BlockBuilder(int restart_interval) : interval_(restart_interval) { restarts_.push_back(0); }

void BlockBuilder::Reset() {
  buf_.clear();
  restarts_.assign(1, 0);
  counter_ = 0;
  last_key_.clear();
  finished_ = false;
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  size_t shared = 0;
  if (counter_ < interval_) {
    size_t lim = std::min(last_key_.size(), key.size());
    while (shared < lim && last_key_[shared] == key[shared]) ++shared;
  } else {
    restarts_.push_back(static_cast<uint32_t>(buf_.size()));
    counter_ = 0;
  }
  PutVarint32(&buf_, static_cast<uint32_t>(shared));
  PutVarint32(&buf_, static_cast<uint32_t>(key.size() - shared));
  PutVarint32(&buf_, static_cast<uint32_t>(value.size()));
  buf_.append(key.substr(shared));
  buf_.append(value);
  last_key_.assign(key);
  ++counter_;
}

std::string_view BlockBuilder::Finish() {
  for (uint32_t r : restarts_) PutFixed32(&buf_, r);
  PutFixed32(&buf_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buf_;
}

namespace {

class BlockIterator final : public Iterator {
 public:
  explicit BlockIterator(std::string_view b) : data_(b) {
    if (b.size() < 4) {
      status_ = Status::Corruption("block too small");
      return;
    }
    num_restarts_ = DecodeFixed32(b.data() + b.size() - 4);
    if (num_restarts_ == 0 || (b.size() - 4) / 4 < num_restarts_) {
      status_ = Status::Corruption("bad restart array");
      num_restarts_ = 0;
      return;
    }
    restarts_off_ = b.size() - 4 - 4 * num_restarts_;
  }

  [[nodiscard]] bool Valid() const override { return valid_; }
  [[nodiscard]] std::string_view key() const override { return key_; }
  [[nodiscard]] std::string_view value() const override { return value_; }
  [[nodiscard]] Status status() const override { return status_; }

  void SeekToFirst() override {
    if (!status_.ok()) return;
    SeekToRestart(0);
    ParseNext();
  }

  void Next() override { ParseNext(); }

  void Seek(std::string_view target) override {
    if (!status_.ok()) return;
    // Binary search for the last restart point whose key < target.
    uint32_t lo = 0, hi = num_restarts_ - 1;
    while (lo < hi) {
      uint32_t mid = (lo + hi + 1) / 2;
      std::string_view k;
      if (!KeyAtRestart(mid, &k)) return;
      if (CompareInternal(k, target) < 0) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    SeekToRestart(lo);
    for (;;) {
      if (!ParseNext()) return;
      if (CompareInternal(key_, target) >= 0) return;
    }
  }

 private:
  uint32_t RestartPoint(uint32_t i) const { return DecodeFixed32(data_.data() + restarts_off_ + 4 * i); }

  void SeekToRestart(uint32_t i) {
    key_.clear();
    next_ = RestartPoint(i);
    valid_ = false;
  }

  bool KeyAtRestart(uint32_t i, std::string_view* k) {
    std::string_view in = data_.substr(RestartPoint(i), restarts_off_ - RestartPoint(i));
    uint32_t shared, non_shared, vlen;
    if (!GetVarint32(&in, &shared) || !GetVarint32(&in, &non_shared) || !GetVarint32(&in, &vlen) || shared != 0 ||
        in.size() < non_shared) {
      Corrupt();
      return false;
    }
    *k = in.substr(0, non_shared);
    return true;
  }

  bool ParseNext() {
    if (next_ >= restarts_off_) {
      valid_ = false;
      return false;
    }
    std::string_view in = data_.substr(next_, restarts_off_ - next_);
    uint32_t shared, non_shared, vlen;
    if (!GetVarint32(&in, &shared) || !GetVarint32(&in, &non_shared) || !GetVarint32(&in, &vlen) ||
        shared > key_.size() || in.size() < static_cast<size_t>(non_shared) + vlen) {
      Corrupt();
      return false;
    }
    key_.resize(shared);
    key_.append(in.substr(0, non_shared));
    value_ = in.substr(non_shared, vlen);
    next_ = static_cast<size_t>(value_.data() + vlen - data_.data());
    valid_ = true;
    return true;
  }

  void Corrupt() {
    valid_ = false;
    status_ = Status::Corruption("bad block entry");
  }

  std::string_view data_;
  uint32_t num_restarts_ = 0;
  size_t restarts_off_ = 0;
  size_t next_ = 0;
  std::string key_;
  std::string_view value_;
  bool valid_ = false;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewBlockIterator(std::string_view block) { return std::make_unique<BlockIterator>(block); }

// ---------------------------------------------------------------------------
// Bloom filter: k probes via double hashing; last byte stores k.

std::string BloomFilter::Build(const std::vector<std::string>& keys, int bits_per_key) {
  auto k = static_cast<int>(std::lround(bits_per_key * 0.69));
  k = std::clamp(k, 1, 30);
  size_t bits = std::max<size_t>(64, keys.size() * static_cast<size_t>(bits_per_key));
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  std::string f(bytes, '\0');
  for (const auto& key : keys) {
    uint64_t h = Hash64(key);
    uint64_t delta = (h >> 33) | (h << 31);
    for (int j = 0; j < k; ++j) {
      uint64_t pos = h % bits;
      f[pos / 8] = static_cast<char>(f[pos / 8] | (1 << (pos % 8)));
      h += delta;
    }
  }
  f.push_back(static_cast<char>(k));
  return f;
}

bool BloomFilter::MayContain(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) return true;
  int k = static_cast<unsigned char>(filter.back());
  if (k < 1 || k > 30) return true;  // unknown encoding: never false-negative
  size_t bits = (filter.size() - 1) * 8;
  uint64_t h = Hash64(key);
  uint64_t delta = (h >> 33) | (h << 31);
  for (int j = 0; j < k; ++j) {
    uint64_t pos = h % bits;
    if ((static_cast<unsigned char>(filter[pos / 8]) & (1 << (pos % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

// ---------------------------------------------------------------------------
// TableBuilder

TableBuilder::TableBuilder(const TableOptions& opts, std::unique_ptr<WritableFile> file)
    : opts_(opts), file_(std::move(file)) {}

BlockHandle TableBuilder::WriteRaw(std::string_view contents) {
  BlockHandle h{offset_, contents.size()};
  std::string trailer;
  PutFixed32(&trailer, crc32c::Mask(crc32c::Value(contents.data(), contents.size())));
  if (status_.ok()) status_ = file_->Append(contents);
  if (status_.ok()) status_ = file_->Append(trailer);
  offset_ += contents.size() + trailer.size();
  return h;
}

void TableBuilder::Add(std::string_view ikey, std::string_view value) {
  pending_.Add(ikey, value);
  last_key_.assign(ikey);
  std::string_view uk = ExtractUserKey(ikey);
  if (filter_keys_.empty() || filter_keys_.back() != uk) filter_keys_.emplace_back(uk);
  ++entries_;
  if (pending_.SizeEstimate() >= opts_.block_size) FlushBlock();
}

void TableBuilder::FlushBlock() {
  if (pending_.empty()) return;
  BlockHandle h = WriteRaw(pending_.Finish());
  std::string hv;
  PutHandle(&hv, h);
  index_.Add(last_key_, hv);
  pending_.Reset();
}

Status TableBuilder::Finish() {
  FlushBlock();
  BlockHandle fh = WriteRaw(BloomFilter::Build(filter_keys_, opts_.bloom_bits_per_key));
  BlockHandle ih = WriteRaw(index_.Finish());
  std::string footer;
  PutFixed64(&footer, fh.offset);
  PutFixed64(&footer, fh.size);
  PutFixed64(&footer, ih.offset);
  PutFixed64(&footer, ih.size);
  PutFixed64(&footer, kMagic);
  if (status_.ok()) status_ = file_->Append(footer);
  offset_ += footer.size();
  if (status_.ok()) status_ = file_->Sync();
  if (status_.ok()) status_ = file_->Close();
  return status_;
}

// ---------------------------------------------------------------------------
// Table

Status Table::ReadBlock(const BlockHandle& h, std::string_view* out) const {
  std::string_view d = file_->data();
  if (h.offset > d.size() || d.size() - h.offset < h.size + 4)
    return Status::Corruption("block handle past end of file");
  std::string_view b = d.substr(h.offset, h.size);
  uint32_t want = crc32c::Unmask(DecodeFixed32(d.data() + h.offset + h.size));
  if (crc32c::Value(b.data(), b.size()) != want) return Status::Corruption("block checksum mismatch");
  *out = b;
  return Status::OK();
}

Status Table::Open(const std::string& path, std::shared_ptr<Table>* out) {
  auto t = std::shared_ptr<Table>(new Table);
  Status s = MappedFile::Open(path, &t->file_);
  if (!s.ok()) return s;
  std::string_view d = t->file_->data();
  if (d.size() < kFooterSize) return Status::Corruption("file too short for a table: " + path);
  const char* f = d.data() + d.size() - kFooterSize;
  if (DecodeFixed64(f + 32) != kMagic) return Status::Corruption("bad table magic: " + path);
  BlockHandle fh{DecodeFixed64(f), DecodeFixed64(f + 8)};
  BlockHandle ih{DecodeFixed64(f + 16), DecodeFixed64(f + 24)};
  if (s = t->ReadBlock(fh, &t->filter_); !s.ok()) return s;
  if (s = t->ReadBlock(ih, &t->index_); !s.ok()) return s;
  *out = std::move(t);
  return Status::OK();
}

Status Table::Get(std::string_view lookup, bool* found, std::string* ikey, std::string* value) const {
  *found = false;
  if (!BloomFilter::MayContain(filter_, ExtractUserKey(lookup))) return Status::OK();
  auto idx = NewBlockIterator(index_);
  idx->Seek(lookup);
  if (!idx->Valid()) return idx->status();
  std::string_view hv = idx->value();
  BlockHandle h;
  if (!GetHandle(&hv, &h)) return Status::Corruption("bad index entry");
  std::string_view block;
  if (Status s = ReadBlock(h, &block); !s.ok()) return s;
  auto it = NewBlockIterator(block);
  it->Seek(lookup);
  if (!it->Valid()) return it->status();
  if (ExtractUserKey(it->key()) == ExtractUserKey(lookup)) {
    *found = true;
    ikey->assign(it->key());
    value->assign(it->value());
  }
  return Status::OK();
}

namespace {

// Two-level iterator: index block -> data blocks.
class TableIterator final : public Iterator {
 public:
  explicit TableIterator(std::shared_ptr<const Table> t)
      : t_(std::move(t)), index_(NewBlockIterator(t_->index_block())) {}

  [[nodiscard]] bool Valid() const override { return data_ && data_->Valid(); }
  [[nodiscard]] std::string_view key() const override { return data_->key(); }
  [[nodiscard]] std::string_view value() const override { return data_->value(); }
  [[nodiscard]] Status status() const override {
    if (!status_.ok()) return status_;
    if (Status s = index_->status(); !s.ok()) return s;
    return data_ ? data_->status() : Status::OK();
  }

  void SeekToFirst() override {
    index_->SeekToFirst();
    LoadBlock();
    if (data_) data_->SeekToFirst();
    SkipEmpty();
  }
  void Seek(std::string_view target) override {
    index_->Seek(target);
    LoadBlock();
    if (data_) data_->Seek(target);
    SkipEmpty();
  }
  void Next() override {
    data_->Next();
    SkipEmpty();
  }

 private:
  void LoadBlock() {
    data_.reset();
    if (!index_->Valid()) return;
    std::string_view hv = index_->value();
    BlockHandle h;
    if (!GetHandle(&hv, &h)) {
      status_ = Status::Corruption("bad index entry");
      return;
    }
    std::string_view block;
    if (Status s = t_->ReadBlock(h, &block); !s.ok()) {
      status_ = s;
      return;
    }
    data_ = NewBlockIterator(block);
  }
  void SkipEmpty() {
    while (data_ && !data_->Valid() && data_->status().ok()) {
      index_->Next();
      LoadBlock();
      if (data_) data_->SeekToFirst();
    }
  }

  std::shared_ptr<const Table> t_;
  std::unique_ptr<Iterator> index_;
  std::unique_ptr<Iterator> data_;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> Table::NewIterator(std::shared_ptr<const Table> t) {
  return std::make_unique<TableIterator>(std::move(t));
}

}  // namespace sediment
