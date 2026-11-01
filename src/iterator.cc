#include "iterator.h"

#include "dbformat.h"

namespace sediment {

namespace {

class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(Status s) : s_(std::move(s)) {}
  [[nodiscard]] bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view) override {}
  void Next() override {}
  [[nodiscard]] std::string_view key() const override { return {}; }
  [[nodiscard]] std::string_view value() const override { return {}; }
  [[nodiscard]] Status status() const override { return s_; }

 private:
  Status s_;
};

// k-way merge with a binary min-heap: each Next() costs O(log k) key
// comparisons instead of comparing every child.
class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> c) : children_(std::move(c)) {}

  [[nodiscard]] bool Valid() const override { return !heap_.empty(); }
  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    Rebuild();
  }
  void Seek(std::string_view t) override {
    for (auto& c : children_) c->Seek(t);
    Rebuild();
  }
  void Next() override {
    Iterator* top = heap_.front();
    top->Next();
    if (!top->Valid()) {
      heap_.front() = heap_.back();
      heap_.pop_back();
    }
    if (!heap_.empty()) SiftDown(0);
  }
  [[nodiscard]] std::string_view key() const override { return heap_.front()->key(); }
  [[nodiscard]] std::string_view value() const override { return heap_.front()->value(); }
  [[nodiscard]] Status status() const override {
    for (const auto& c : children_) {
      if (Status s = c->status(); !s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  static bool Less(Iterator* a, Iterator* b) { return CompareInternal(a->key(), b->key()) < 0; }

  void Rebuild() {
    heap_.clear();
    for (auto& c : children_) {
      if (c->Valid()) heap_.push_back(c.get());
    }
    for (size_t i = heap_.size() / 2; i-- > 0;) SiftDown(i);
  }

  void SiftDown(size_t i) {
    const size_t n = heap_.size();
    for (;;) {
      size_t l = 2 * i + 1, best = i;
      if (l < n && Less(heap_[l], heap_[best])) best = l;
      if (l + 1 < n && Less(heap_[l + 1], heap_[best])) best = l + 1;
      if (best == i) return;
      std::swap(heap_[i], heap_[best]);
      i = best;
    }
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<Iterator*> heap_;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  if (children.size() == 1) return std::move(children[0]);
  return std::make_unique<MergingIterator>(std::move(children));
}

std::unique_ptr<Iterator> NewEmptyIterator(Status s) { return std::make_unique<EmptyIterator>(std::move(s)); }

}  // namespace sediment
