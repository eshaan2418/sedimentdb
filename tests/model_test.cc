// Differential test: random operations against the DB and a std::map model,
// with snapshots, reopens and forced compactions interleaved. Every read is
// checked, including full scans at random snapshots.

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <random>

#include "sediment/db.h"

using namespace sediment;

namespace {
using Model = std::map<std::string, std::string>;

void ExpectScanMatches(DB* db, const Snapshot* snap, const Model& m) {
  ReadOptions ro;
  ro.snapshot = snap;
  auto it = db->NewIterator(ro);
  auto mi = m.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++mi) {
    ASSERT_NE(mi, m.end()) << "extra key " << it->key();
    ASSERT_EQ(it->key(), mi->first);
    ASSERT_EQ(it->value(), mi->second);
  }
  ASSERT_TRUE(it->status().ok()) << it->status().ToString();
  ASSERT_EQ(mi, m.end()) << "missing key " << mi->first;
}
}  // namespace

class ModelTest : public ::testing::TestWithParam<int> {};

TEST_P(ModelTest, RandomOperationsMatchStdMap) {
  const int seed = GetParam();
  std::mt19937 rng(seed);
  std::string path = ::testing::TempDir() + "/sediment_model_" + std::to_string(seed);
  DestroyDB(path);
  Options o;
  o.write_buffer_size = 32 << 10;
  o.max_bytes_level1 = 128 << 10;
  o.target_file_size = 32 << 10;
  o.l0_compaction_trigger = 2;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(o, path, &db).ok());

  Model model;
  std::vector<std::pair<const Snapshot*, Model>> snaps;
  auto key = [&] { return "k" + std::to_string(rng() % 800); };
  for (int step = 0; step < 20000; ++step) {
    int r = static_cast<int>(rng() % 1000);
    if (r < 550) {
      std::string k = key(), v = "v" + std::to_string(step) + std::string(rng() % 200, 'x');
      ASSERT_TRUE(db->Put({}, k, v).ok());
      model[k] = v;
    } else if (r < 700) {
      std::string k = key();
      ASSERT_TRUE(db->Delete({}, k).ok());
      model.erase(k);
    } else if (r < 760) {
      WriteBatch b;
      for (int i = 0; i < 5; ++i) {
        std::string k = key();
        if (rng() % 3) {
          b.Put(k, "batch" + std::to_string(step));
          model[k] = "batch" + std::to_string(step);
        } else {
          b.Delete(k);
          model.erase(k);
        }
      }
      ASSERT_TRUE(db->Write({}, &b).ok());
    } else if (r < 960) {
      std::string k = key(), v;
      Status s = db->Get({}, k, &v);
      auto it = model.find(k);
      if (it == model.end()) {
        ASSERT_TRUE(s.IsNotFound()) << "step " << step << " key " << k << " got " << v;
      } else {
        ASSERT_TRUE(s.ok()) << "step " << step << " key " << k << " " << s.ToString();
        ASSERT_EQ(v, it->second) << "step " << step;
      }
    } else if (r < 975) {
      if (snaps.size() < 4) snaps.emplace_back(db->GetSnapshot(), model);
    } else if (r < 985 && !snaps.empty()) {
      size_t i = rng() % snaps.size();
      ExpectScanMatches(db.get(), snaps[i].first, snaps[i].second);
      // Point reads at the snapshot too.
      for (int j = 0; j < 20; ++j) {
        std::string k = key(), v;
        ReadOptions ro;
        ro.snapshot = snaps[i].first;
        Status s = db->Get(ro, k, &v);
        auto it = snaps[i].second.find(k);
        ASSERT_EQ(s.ok(), it != snaps[i].second.end()) << k;
        if (s.ok()) ASSERT_EQ(v, it->second);
      }
      db->ReleaseSnapshot(snaps[i].first);
      snaps.erase(snaps.begin() + static_cast<long>(i));
    } else if (r < 992) {
      ASSERT_TRUE(db->CompactAll().ok());
    } else if (r < 996) {
      ExpectScanMatches(db.get(), nullptr, model);
    } else {
      // Reopen (snapshots do not survive a restart).
      for (auto& [s, m] : snaps) db->ReleaseSnapshot(s);
      snaps.clear();
      db.reset();
      ASSERT_TRUE(DB::Open(o, path, &db).ok());
    }
  }
  for (auto& [s, m] : snaps) db->ReleaseSnapshot(s);
  ExpectScanMatches(db.get(), nullptr, model);
  db.reset();
  DestroyDB(path);
}

INSTANTIATE_TEST_SUITE_P(Seeds, ModelTest, ::testing::Values(1, 2, 3, 4));
