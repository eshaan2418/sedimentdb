#include "table.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <random>

#include "dbformat.h"

using namespace sediment;

namespace {
std::string Key(int i) {
  char b[32];
  std::snprintf(b, sizeof b, "key%08d", i);
  return MakeInternalKey(b, static_cast<SequenceNumber>(i + 1), kTypeValue);
}
}  // namespace

TEST(Block, BuildSeekIterate) {
  BlockBuilder b;
  for (int i = 0; i < 500; ++i) b.Add(Key(i * 2), "v" + std::to_string(i));
  std::string block(b.Finish());
  auto it = NewBlockIterator(block);
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
  EXPECT_EQ(n, 500);
  it->Seek(Key(101));  // between 100 and 102
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(102));
  EXPECT_EQ(it->value(), "v51");
  it->Seek(Key(0));
  EXPECT_EQ(it->key(), Key(0));
  it->Seek(Key(5000));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST(Block, RejectsGarbage) {
  auto it = NewBlockIterator(std::string_view("\x01\x02", 2));
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_FALSE(it->status().ok());
}

TEST(Bloom, NoFalseNegativesAndLowFalsePositives) {
  std::vector<std::string> keys;
  for (int i = 0; i < 10000; ++i) keys.push_back("present-" + std::to_string(i));
  std::string f = BloomFilter::Build(keys, 10);
  for (const auto& k : keys) ASSERT_TRUE(BloomFilter::MayContain(f, k));
  int fp = 0;
  for (int i = 0; i < 10000; ++i) fp += BloomFilter::MayContain(f, "absent-" + std::to_string(i));
  double rate = fp / 10000.0;
  EXPECT_LT(rate, 0.02) << rate;  // ~1% expected at 10 bits/key
}

class TableTest : public ::testing::Test {
 protected:
  std::string path_ = ::testing::TempDir() + "/table_test.sst";
  void Build(int n) {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(WritableFile::Open(path_, &f).ok());
    TableBuilder b(TableOptions{}, std::move(f));
    for (int i = 0; i < n; ++i) b.Add(Key(i), std::string(100, static_cast<char>('a' + i % 26)));
    ASSERT_TRUE(b.Finish().ok());
  }
};

TEST_F(TableTest, GetAndIterate) {
  Build(20000);
  std::shared_ptr<Table> t;
  ASSERT_TRUE(Table::Open(path_, &t).ok());
  bool found;
  std::string ik, v;
  for (int i = 0; i < 20000; i += 997) {
    ASSERT_TRUE(t->Get(Key(i), &found, &ik, &v).ok());
    ASSERT_TRUE(found) << i;
    EXPECT_EQ(v[0], 'a' + i % 26);
  }
  ASSERT_TRUE(t->Get(MakeInternalKey("nope", 1, kTypeValue), &found, &ik, &v).ok());
  EXPECT_FALSE(found);
  auto it = Table::NewIterator(t);
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
  EXPECT_EQ(n, 20000);
  it->Seek(Key(12345));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Key(12345));
}

TEST_F(TableTest, DetectsCorruption) {
  Build(1000);
  std::string data;
  ASSERT_TRUE(ReadFileToString(path_, &data).ok());
  data[100] ^= 0x01;  // inside the first data block
  ASSERT_TRUE(WriteStringToFileSync(path_, data).ok());
  std::shared_ptr<Table> t;
  ASSERT_TRUE(Table::Open(path_, &t).ok());  // footer, index and filter are intact
  auto it = Table::NewIterator(t);
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
  bool found;
  std::string ik, v;
  EXPECT_TRUE(t->Get(Key(0), &found, &ik, &v).IsCorruption());
}

TEST_F(TableTest, RejectsTruncatedFile) {
  Build(100);
  std::string data;
  ASSERT_TRUE(ReadFileToString(path_, &data).ok());
  ASSERT_TRUE(WriteStringToFileSync(path_, data.substr(0, data.size() - 10)).ok());
  std::shared_ptr<Table> t;
  EXPECT_TRUE(Table::Open(path_, &t).IsCorruption());
}
