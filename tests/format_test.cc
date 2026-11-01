#include <gtest/gtest.h>
#include <unistd.h>

#include "coding.h"
#include "dbformat.h"
#include "log.h"
#include "memtable.h"
#include "version.h"
#include "write_batch_internal.h"

using namespace sediment;

TEST(Coding, Varints) {
  std::string s;
  const uint64_t vals[] = {0, 1, 127, 128, 300, 1ull << 32, UINT64_MAX};
  for (uint64_t v : vals) PutVarint64(&s, v);
  std::string_view in(s);
  for (uint64_t v : vals) {
    uint64_t got = 0;
    ASSERT_TRUE(GetVarint64(&in, &got));
    EXPECT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
  std::string_view truncated("\x80\x80", 2);
  uint64_t x;
  EXPECT_FALSE(GetVarint64(&truncated, &x));
}

TEST(Coding, Crc32cKnownValues) {
  // Standard check value for CRC-32C.
  EXPECT_EQ(crc32c::Value("123456789", 9), 0xE3069283u);
  std::string zeros(32, '\0');
  EXPECT_EQ(crc32c::Value(zeros.data(), zeros.size()), 0x8A9136AAu);
  uint32_t c = crc32c::Value("hello world", 11);
  EXPECT_EQ(crc32c::Unmask(crc32c::Mask(c)), c);
  EXPECT_NE(crc32c::Mask(c), c);
  EXPECT_EQ(crc32c::Extend(crc32c::Value("hello ", 6), "world", 5), c);
}

TEST(Format, InternalKeyOrdering) {
  std::string a1 = MakeInternalKey("a", 1, kTypeValue);
  std::string a5 = MakeInternalKey("a", 5, kTypeValue);
  std::string b1 = MakeInternalKey("b", 1, kTypeValue);
  EXPECT_LT(CompareInternal(a5, a1), 0);  // newer first
  EXPECT_LT(CompareInternal(a1, b1), 0);
  EXPECT_EQ(ExtractUserKey(a5), "a");
  EXPECT_EQ(ExtractSeq(a5), 5u);
  EXPECT_EQ(ExtractType(MakeInternalKey("x", 3, kTypeDeletion)), kTypeDeletion);
}

TEST(MemTable, GetSeesNewestVisibleVersion) {
  MemTable m;
  m.Add(1, kTypeValue, "k", "v1");
  m.Add(2, kTypeValue, "k", "v2");
  m.Add(3, kTypeDeletion, "k", "");
  std::string v;
  bool del = false;
  ASSERT_TRUE(m.Get("k", 1, &v, &del));
  EXPECT_FALSE(del);
  EXPECT_EQ(v, "v1");
  ASSERT_TRUE(m.Get("k", 2, &v, &del));
  EXPECT_EQ(v, "v2");
  ASSERT_TRUE(m.Get("k", 10, &v, &del));
  EXPECT_TRUE(del);
  EXPECT_FALSE(m.Get("j", 10, &v, &del));
  EXPECT_FALSE(m.Get("k", 0, &v, &del));
}

TEST(MemTable, IteratorIsSorted) {
  MemTable m;
  for (int i = 999; i >= 0; --i) m.Add(static_cast<SequenceNumber>(1000 - i), kTypeValue, std::to_string(i), "x");
  auto it = m.NewIterator();
  std::string prev;
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++n) {
    if (n > 0) EXPECT_LT(CompareInternal(prev, it->key()), 0);
    prev.assign(it->key());
  }
  EXPECT_EQ(n, 1000);
}

TEST(WriteBatch, RoundTrip) {
  WriteBatch b;
  b.Put("a", "1");
  b.Delete("b");
  b.Put("c", std::string(1000, 'z'));
  WriteBatchInternal::SetSequence(&b, 77);
  WriteBatch c;
  ASSERT_TRUE(WriteBatchInternal::SetContents(&c, WriteBatchInternal::Contents(b)).ok());
  EXPECT_EQ(c.Count(), 3u);
  EXPECT_EQ(WriteBatchInternal::Sequence(c), 77u);
  MemTable m;
  ASSERT_TRUE(WriteBatchInternal::InsertInto(c, &m).ok());
  std::string v;
  bool del;
  ASSERT_TRUE(m.Get("b", 100, &v, &del));
  EXPECT_TRUE(del);
  ASSERT_TRUE(m.Get("c", 100, &v, &del));
  EXPECT_EQ(v.size(), 1000u);
}

TEST(Log, StopsAtTornOrCorruptTail) {
  std::string path = ::testing::TempDir() + "/log_test_" + std::to_string(::getpid()) + ".log";
  {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(WritableFile::Open(path, &f).ok());
    LogWriter w(std::move(f));
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(w.AddRecord("record-" + std::to_string(i)).ok());
  }
  std::string data;
  ASSERT_TRUE(ReadFileToString(path, &data).ok());
  // Torn tail: cut the last record in half.
  LogReader torn(data.substr(0, data.size() - 4));
  std::string_view r;
  int n = 0;
  while (torn.ReadRecord(&r)) ++n;
  EXPECT_EQ(n, 9);
  EXPECT_TRUE(torn.hit_corruption());
  // Bit flip in record 5's payload: everything from record 5 on is dropped.
  std::string flipped = data;
  size_t off = 0;
  for (int i = 0; i < 5; ++i) off += 8 + std::string("record-" + std::to_string(i)).size();
  flipped[off + 9] ^= 0x40;
  LogReader bad(flipped);
  n = 0;
  while (bad.ReadRecord(&r)) ++n;
  EXPECT_EQ(n, 5);
  LogReader clean(data);
  n = 0;
  while (clean.ReadRecord(&r)) ++n;
  EXPECT_EQ(n, 10);
  EXPECT_FALSE(clean.hit_corruption());
}

TEST(Version, EditRoundTripAndApply) {
  VersionEdit e;
  e.SetLogNumber(7);
  e.SetNextFile(42);
  e.SetLastSequence(1000);
  FileMeta f;
  f.number = 9;
  f.size = 1234;
  f.smallest = MakeInternalKey("a", 1, kTypeValue);
  f.largest = MakeInternalKey("m", 5, kTypeValue);
  e.added.emplace_back(1, f);
  e.deleted.emplace_back(0, 3);
  std::string enc;
  e.EncodeTo(&enc);
  VersionEdit d;
  ASSERT_TRUE(d.DecodeFrom(enc).ok());
  EXPECT_EQ(d.log_number, 7u);
  EXPECT_EQ(d.next_file, 42u);
  EXPECT_EQ(d.last_seq, 1000u);
  ASSERT_EQ(d.added.size(), 1u);
  EXPECT_EQ(d.added[0].second.largest, f.largest);
  EXPECT_FALSE(VersionEdit().DecodeFrom("\x63").ok());
  Version base;
  auto v = ApplyEdit(base, d);
  EXPECT_EQ(v->NumFiles(1), 1);
  EXPECT_EQ(v->LevelBytes(1), 1234u);
}

TEST(Version, FileNames) {
  uint64_t n;
  FileType t;
  ASSERT_TRUE(ParseFileName("000123.sst", &n, &t));
  EXPECT_EQ(n, 123u);
  EXPECT_EQ(t, FileType::kTable);
  ASSERT_TRUE(ParseFileName("MANIFEST-000004", &n, &t));
  EXPECT_EQ(t, FileType::kManifest);
  EXPECT_FALSE(ParseFileName("notes.txt", &n, &t));
  EXPECT_FALSE(ParseFileName("12x.log", &n, &t));
}
