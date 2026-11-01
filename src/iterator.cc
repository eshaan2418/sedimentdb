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

class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> c) : children_(std::move(c)) {}

  [[nodiscard]] bool Valid() const override { return current_ != nullptr; }
  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    FindSmallest();
  }
  void Seek(std::string_view t) override {
    for (auto& c : children_) c->Seek(t);
    FindSmallest();
  }
  void Next() override {
    current_->Next();
    FindSmallest();
  }
  [[nodiscard]] std::string_view key() const override { return current_->key(); }
  [[nodiscard]] std::string_view value() const override { return current_->value(); }
  [[nodiscard]] Status status() const override {
    for (const auto& c : children_) {
      if (Status s = c->status(); !s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  void FindSmallest() {
    current_ = nullptr;
    for (auto& c : children_) {
      if (c->Valid() && (current_ == nullptr || CompareInternal(c->key(), current_->key()) < 0)) current_ = c.get();
    }
  }
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  if (children.size() == 1) return std::move(children[0]);
  return std::make_unique<MergingIterator>(std::move(children));
}

std::unique_ptr<Iterator> NewEmptyIterator(Status s) { return std::make_unique<EmptyIterator>(std::move(s)); }

}  // namespace sediment
