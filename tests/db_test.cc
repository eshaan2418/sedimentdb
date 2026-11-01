#include "sediment/db.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include "env.h"

using namespace sediment;

namespace {

std::string Key(int i) {
  char b[32];
  std::snprintf(b, sizeof b, "key%08d", i);
  return b;
}

class DBTest : public ::testing::Test {
 protected:
  std::string path_ =
      ::testing::TempDir() + "/sediment_db_test_" + ::testing::UnitTest::GetInstance()->current_test_info()->name();
  Options opts_;
  std::unique_ptr<DB> db_;

  void SetUp() override {
    DestroyDB(path_);
    opts_.write_buffer_size = 64 << 10;  // small: exercise flushes and compactions
    opts_.max_bytes_level1 = 256 << 10;
    opts_.target_file_size = 64 << 10;
    Reopen();
  }
  void TearDown() override {
    db_.reset();
    DestroyDB(path_);
  }
  void Reopen() {
    db_.reset();
    ASSERT_TRUE(DB::Open(opts_, path_, &db_).ok());
  }
  std::string Get(const std::string& k, const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    std::string v;
    Status s = db_->Get(ro, k, &v);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return "ERROR: " + s.ToString();
    return v;
  }
};

}  // namespace

TEST_F(DBTest, PutGetDeleteOverwrite) {
  WriteOptions w;
  ASSERT_TRUE(db_->Put(w, "a", "1").ok());
  ASSERT_TRUE(db_->Put(w, "b", "2").ok());
  EXPECT_EQ(Get("a"), "1");
  ASSERT_TRUE(db_->Put(w, "a", "3").ok());
  EXPECT_EQ(Get("a"), "3");
  ASSERT_TRUE(db_->Delete(w, "a").ok());
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(Get("zzz"), "NOT_FOUND");
}

TEST_F(DBTest, PersistsAcrossReopenWithAndWithoutFlush) {
  WriteOptions w;
  for (int i = 0; i < 5000; ++i) ASSERT_TRUE(db_->Put(w, Key(i), std::string(100, 'v')).ok());
  ASSERT_TRUE(db_->Delete(w, Key(7)).ok());
  Reopen();  // some data in tables, the tail only in the WAL
  EXPECT_EQ(Get(Key(0)), std::string(100, 'v'));
  EXPECT_EQ(Get(Key(4999)), std::string(100, 'v'));
  EXPECT_EQ(Get(Key(7)), "NOT_FOUND");
  ASSERT_TRUE(db_->Put(w, "after", "reopen").ok());
  Reopen();
  EXPECT_EQ(Get("after"), "reopen");
  EXPECT_EQ(Get(Key(7)), "NOT_FOUND");
}

TEST_F(DBTest, CompactionKeepsNewestAndDropsDeleted) {
  WriteOptions w;
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 3000; ++i) ASSERT_TRUE(db_->Put(w, Key(i), "r" + std::to_string(round)).ok());
  }
  for (int i = 0; i < 3000; i += 2) ASSERT_TRUE(db_->Delete(w, Key(i)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  // CompactAll stops once no level exceeds its trigger.
  EXPECT_LT(db_->NumFilesAtLevel(0), opts_.l0_compaction_trigger) << db_->Stats();
  for (int i = 0; i < 3000; ++i) EXPECT_EQ(Get(Key(i)), i % 2 ? "r3" : "NOT_FOUND") << i;
  Reopen();
  for (int i = 0; i < 3000; i += 101) EXPECT_EQ(Get(Key(i)), i % 2 ? "r3" : "NOT_FOUND") << i;
}

TEST_F(DBTest, IteratorIsSortedAndSkipsDeletes) {
  WriteOptions w;
  for (int i = 999; i >= 0; --i) ASSERT_TRUE(db_->Put(w, Key(i), std::to_string(i)).ok());
  for (int i = 0; i < 1000; i += 3) ASSERT_TRUE(db_->Delete(w, Key(i)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  for (int i = 0; i < 1000; i += 5) ASSERT_TRUE(db_->Put(w, Key(i), "new").ok());  // some in memtable
  auto it = db_->NewIterator({});
  int n = 0;
  std::string prev;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (n++) EXPECT_LT(prev, it->key());
    prev.assign(it->key());
    int i = std::stoi(std::string(it->key().substr(3)));
    EXPECT_TRUE(i % 3 != 0 || i % 5 == 0) << i;
    if (i % 5 == 0) EXPECT_EQ(it->value(), "new");
  }
  EXPECT_TRUE(it->status().ok());
  // 1000 - 334 deleted + 67 re-added among the deleted (multiples of 15)
  EXPECT_EQ(n, 1000 - 334 + 67);
  it->Seek(Key(500));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(500));
}

TEST_F(DBTest, SnapshotIsolationSurvivesCompaction) {
  WriteOptions w;
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(db_->Put(w, Key(i), "old").ok());
  const Snapshot* snap = db_->GetSnapshot();
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(db_->Put(w, Key(i), "new").ok());
  ASSERT_TRUE(db_->Delete(w, Key(5)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ(Get(Key(1)), "new");
  EXPECT_EQ(Get(Key(1), snap), "old");
  EXPECT_EQ(Get(Key(5)), "NOT_FOUND");
  EXPECT_EQ(Get(Key(5), snap), "old");
  ReadOptions ro;
  ro.snapshot = snap;
  auto it = db_->NewIterator(ro);
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++n) EXPECT_EQ(it->value(), "old");
  EXPECT_EQ(n, 2000);
  db_->ReleaseSnapshot(snap);
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ(Get(Key(1)), "new");
}

TEST_F(DBTest, IteratorOutlivesCompaction) {
  WriteOptions w;
  for (int i = 0; i < 3000; ++i) ASSERT_TRUE(db_->Put(w, Key(i), "x").ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  auto it = db_->NewIterator({});
  it->SeekToFirst();
  for (int i = 0; i < 3000; ++i) ASSERT_TRUE(db_->Put(w, Key(i), "y").ok());
  ASSERT_TRUE(db_->CompactAll().ok());  // deletes the files the iterator reads
  int n = 0;
  for (; it->Valid(); it->Next(), ++n) EXPECT_EQ(it->value(), "x");
  EXPECT_EQ(n, 3000);
  EXPECT_TRUE(it->status().ok());
}

TEST_F(DBTest, ConcurrentWritersAndReaders) {
  constexpr int kThreads = 4, kPer = 3000;
  std::atomic<bool> stop{false};
  std::atomic<int> read_errors{0};
  std::thread reader([&] {
    while (!stop) {
      std::string v;
      Status s = db_->Get({}, Key(0), &v);
      if (!s.ok() && !s.IsNotFound()) ++read_errors;
      auto it = db_->NewIterator({});
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
      }
      if (!it->status().ok()) ++read_errors;
    }
  });
  std::vector<std::thread> writers;
  for (int t = 0; t < kThreads; ++t) {
    writers.emplace_back([&, t] {
      WriteOptions w;
      w.sync = (t == 0);  // mix sync and non-sync writers in the same groups
      for (int i = 0; i < kPer; ++i) ASSERT_TRUE(db_->Put(w, Key(t * kPer + i), std::to_string(t)).ok());
    });
  }
  for (auto& th : writers) th.join();
  stop = true;
  reader.join();
  EXPECT_EQ(read_errors.load(), 0);
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPer; i += 211) EXPECT_EQ(Get(Key(t * kPer + i)), std::to_string(t));
  }
}

TEST_F(DBTest, WriteBatchIsAtomicAndOrdered) {
  WriteBatch b;
  b.Put("k", "1");
  b.Delete("k");
  b.Put("k", "2");
  b.Put("other", "x");
  ASSERT_TRUE(db_->Write({}, &b).ok());
  EXPECT_EQ(Get("k"), "2");
  Reopen();
  EXPECT_EQ(Get("k"), "2");
  EXPECT_EQ(Get("other"), "x");
}

TEST_F(DBTest, SecondOpenIsRejectedByLock) {
  std::unique_ptr<DB> other;
  EXPECT_TRUE(DB::Open(opts_, path_, &other).IsIOError());
}

TEST_F(DBTest, RecoversFromTornWalTail) {
  WriteOptions w;
  w.sync = true;
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(db_->Put(w, Key(i), "v").ok());
  db_.reset();
  // Append garbage to the newest WAL, as a crash mid-write would leave.
  std::vector<std::string> names;
  ASSERT_TRUE(ListDir(path_, &names).ok());
  std::string newest;
  for (const auto& n : names) {
    if (n.size() > 4 && n.substr(n.size() - 4) == ".log" && n > newest) newest = n;
  }
  ASSERT_FALSE(newest.empty());
  std::string data;
  ASSERT_TRUE(ReadFileToString(path_ + "/" + newest, &data).ok());
  data += std::string("\x12\x34\x56\x78\xff\xff", 6);
  ASSERT_TRUE(WriteStringToFileSync(path_ + "/" + newest, data).ok());
  Reopen();
  for (int i = 0; i < 100; ++i) EXPECT_EQ(Get(Key(i)), "v");
}

TEST_F(DBTest, ErrorIfExistsAndCreateIfMissing) {
  db_.reset();
  Options o = opts_;
  o.error_if_exists = true;
  std::unique_ptr<DB> d;
  EXPECT_FALSE(DB::Open(o, path_, &d).ok());
  o = opts_;
  o.create_if_missing = false;
  EXPECT_FALSE(DB::Open(o, path_ + "_missing", &d).ok());
  Reopen();
}
