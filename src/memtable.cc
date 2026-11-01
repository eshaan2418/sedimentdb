#include "memtable.h"

#include <cassert>
#include <cstring>

namespace sediment {

// ---------------------------------------------------------------------------
// Arena

namespace {
constexpr size_t kBlockSize = 4096;
}

char* Arena::NewBlock(size_t n) {
  blocks_.emplace_back(new char[n]);
  usage_.fetch_add(n + sizeof(char*), std::memory_order_relaxed);
  return blocks_.back().get();
}

char* Arena::Allocate(size_t n) {
  if (n <= left_) {
    char* r = ptr_;
    ptr_ += n;
    left_ -= n;
    return r;
  }
  if (n > kBlockSize / 4) return NewBlock(n);  // large: own block, keep current
  ptr_ = NewBlock(kBlockSize);
  left_ = kBlockSize - n;
  char* r = ptr_;
  ptr_ += n;
  return r;
}

char* Arena::AllocateAligned(size_t n) {
  constexpr size_t kAlign = alignof(std::max_align_t) > 8 ? alignof(std::max_align_t) : 8;
  size_t mod = reinterpret_cast<uintptr_t>(ptr_) & (kAlign - 1);
  size_t slop = mod == 0 ? 0 : kAlign - mod;
  if (n + slop <= left_) {
    char* r = ptr_ + slop;
    ptr_ += n + slop;
    left_ -= n + slop;
    return r;
  }
  // Not enough room for the padding plus n: start a fresh block (blocks from
  // operator new[] are suitably aligned). Falling back to Allocate(n) here
  // could return the current, unaligned pointer.
  if (n > kBlockSize / 4) return NewBlock(n);
  ptr_ = NewBlock(kBlockSize);
  left_ = kBlockSize - n;
  char* r = ptr_;
  ptr_ += n;
  return r;
}

// ---------------------------------------------------------------------------
// Skiplist

struct MemTable::Node {
  const char* entry;  // varint32 ikey_len | ikey | varint32 val_len | value
  std::atomic<Node*> next[1];

  Node* Next(int level) const { return next[level].load(std::memory_order_acquire); }
  void SetNext(int level, Node* n) { next[level].store(n, std::memory_order_release); }
};

namespace {
std::string_view EntryKey(const char* e) {
  std::string_view in(e, 5 + 1);  // a varint32 is at most 5 bytes
  uint32_t n = 0;
  GetVarint32(&in, &n);
  return {in.data(), n};
}
std::string_view EntryValue(const char* e) {
  std::string_view k = EntryKey(e);
  std::string_view in(k.data() + k.size(), 5);
  uint32_t n = 0;
  GetVarint32(&in, &n);
  return {in.data(), n};
}
}  // namespace

MemTable::MemTable() : head_(NewNode(nullptr, kMaxHeight)) {
  for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
}

MemTable::Node* MemTable::NewNode(const char* entry, int height) {
  char* mem = arena_.AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  Node* n = new (mem) Node;
  n->entry = entry;
  for (int i = 1; i < height; ++i) new (&n->next[i]) std::atomic<Node*>(nullptr);
  return n;
}

int MemTable::RandomHeight() {
  int h = 1;
  while (h < kMaxHeight && rng_() % 4 == 0) ++h;
  return h;
}

MemTable::Node* MemTable::FindGreaterOrEqual(std::string_view ikey, Node** prev) const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  for (;;) {
    Node* next = x->Next(level);
    if (next != nullptr && CompareInternal(EntryKey(next->entry), ikey) < 0) {
      x = next;
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;
    }
  }
}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value) {
  std::string buf;
  PutVarint32(&buf, static_cast<uint32_t>(key.size() + 8));
  AppendInternalKey(&buf, key, seq, type);
  PutVarint32(&buf, static_cast<uint32_t>(value.size()));
  buf.append(value);
  char* entry = arena_.Allocate(buf.size());
  std::memcpy(entry, buf.data(), buf.size());

  Node* prev[kMaxHeight];
  FindGreaterOrEqual(EntryKey(entry), prev);
  int h = RandomHeight();
  int cur = max_height_.load(std::memory_order_relaxed);
  if (h > cur) {
    for (int i = cur; i < h; ++i) prev[i] = head_;
    // Readers that see the new height before the node simply find null
    // pointers at the new levels from head_, which is fine.
    max_height_.store(h, std::memory_order_relaxed);
  }
  Node* n = NewNode(entry, h);
  for (int i = 0; i < h; ++i) {
    n->next[i].store(prev[i]->Next(i), std::memory_order_relaxed);
    prev[i]->SetNext(i, n);
  }
}

bool MemTable::Get(std::string_view user_key, SequenceNumber seq, std::string* value, bool* deleted) const {
  std::string lk = LookupKey(user_key, seq);
  Node* n = FindGreaterOrEqual(lk, nullptr);
  if (n == nullptr) return false;
  std::string_view ik = EntryKey(n->entry);
  if (ExtractUserKey(ik) != user_key) return false;
  if (ExtractType(ik) == kTypeDeletion) {
    *deleted = true;
  } else {
    *deleted = false;
    value->assign(EntryValue(n->entry));
  }
  return true;
}

bool MemTable::Empty() const { return head_->Next(0) == nullptr; }

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const MemTable* m) : m_(m) {}
  [[nodiscard]] bool Valid() const override { return n_ != nullptr; }
  void SeekToFirst() override { n_ = m_->head_->Next(0); }
  void Seek(std::string_view target) override { n_ = m_->FindGreaterOrEqual(target, nullptr); }
  void Next() override { n_ = n_->Next(0); }
  [[nodiscard]] std::string_view key() const override { return EntryKey(n_->entry); }
  [[nodiscard]] std::string_view value() const override { return EntryValue(n_->entry); }

 private:
  const MemTable* m_;
  MemTable::Node* n_ = nullptr;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const { return std::make_unique<MemTableIterator>(this); }

}  // namespace sediment
