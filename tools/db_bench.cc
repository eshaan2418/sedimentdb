// db_bench: the same workloads against sedimentdb, LevelDB and SQLite.
//
//   db_bench --engine=sediment --benchmarks=fillseq,fillrandom,readrandom --num=1000000
//
// Keys are 16-byte decimal strings, values random (incompressible) bytes.
// LevelDB runs with compression off and a 10 bits/key bloom filter to match
// sedimentdb. SQLite uses a WITHOUT ROWID table in WAL mode; each operation
// is its own transaction, like a Put on the other engines; synchronous=OFF
// for non-sync workloads and FULL + fullfsync for fillsync.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "sediment/db.h"

#ifdef HAVE_LEVELDB
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>
#endif
#ifdef HAVE_SQLITE
#include <sqlite3.h>
#endif

namespace {

struct Engine {
  virtual ~Engine() = default;
  virtual bool Open(const std::string& path) = 0;
  virtual bool Put(const std::string& k, const std::string& v, bool sync) = 0;
  virtual bool Get(const std::string& k, std::string* v) = 0;
  virtual uint64_t ScanAll() = 0;
  virtual void Destroy(const std::string& path) = 0;
};

struct Sediment : Engine {
  std::unique_ptr<sediment::DB> db;
  bool Open(const std::string& path) override {
    db.reset();
    return sediment::DB::Open(sediment::Options(), path, &db).ok();
  }
  bool Put(const std::string& k, const std::string& v, bool sync) override {
    sediment::WriteOptions w;
    w.sync = sync;
    return db->Put(w, k, v).ok();
  }
  bool Get(const std::string& k, std::string* v) override { return db->Get({}, k, v).ok(); }
  uint64_t ScanAll() override {
    uint64_t n = 0;
    auto it = db->NewIterator({});
    for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
    return n;
  }
  void Destroy(const std::string& path) override {
    db.reset();
    sediment::DestroyDB(path);
  }
};

#ifdef HAVE_LEVELDB
struct LevelDB : Engine {
  std::unique_ptr<leveldb::DB> db;
  std::unique_ptr<const leveldb::FilterPolicy> filter{leveldb::NewBloomFilterPolicy(10)};
  bool Open(const std::string& path) override {
    db.reset();
    leveldb::Options o;
    o.create_if_missing = true;
    o.compression = leveldb::kNoCompression;
    o.filter_policy = filter.get();
    leveldb::DB* raw = nullptr;
    bool ok = leveldb::DB::Open(o, path, &raw).ok();
    db.reset(raw);
    return ok;
  }
  bool Put(const std::string& k, const std::string& v, bool sync) override {
    leveldb::WriteOptions w;
    w.sync = sync;
    return db->Put(w, k, v).ok();
  }
  bool Get(const std::string& k, std::string* v) override { return db->Get({}, k, v).ok(); }
  uint64_t ScanAll() override {
    uint64_t n = 0;
    std::unique_ptr<leveldb::Iterator> it(db->NewIterator({}));
    for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
    return n;
  }
  void Destroy(const std::string& path) override {
    db.reset();
    leveldb::DestroyDB(path, leveldb::Options());
  }
};
#endif

#ifdef HAVE_SQLITE
struct SQLite : Engine {
  sqlite3* db = nullptr;
  sqlite3_stmt *put = nullptr, *get = nullptr;
  bool synchronous = false;
  std::string file;
  ~SQLite() override { Close(); }
  void Close() {
    if (put) sqlite3_finalize(put), put = nullptr;
    if (get) sqlite3_finalize(get), get = nullptr;
    if (db) sqlite3_close(db), db = nullptr;
  }
  bool Exec(const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
  bool Open(const std::string& path) override {
    Close();
    std::string mk = "mkdir -p '" + path + "'";
    if (std::system(mk.c_str()) != 0) return false;
    file = path + "/kv.sqlite";
    if (sqlite3_open(file.c_str(), &db) != SQLITE_OK) return false;
    return Exec("PRAGMA journal_mode=WAL") && Exec("PRAGMA synchronous=OFF") &&
           Exec("CREATE TABLE IF NOT EXISTS kv(k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID") &&
           sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv(k, v) VALUES(?, ?)", -1, &put, nullptr) == SQLITE_OK &&
           sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?", -1, &get, nullptr) == SQLITE_OK;
  }
  bool Put(const std::string& k, const std::string& v, bool sync) override {
    if (sync != synchronous) {
      Exec(sync ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=OFF");
      Exec(sync ? "PRAGMA fullfsync=ON" : "PRAGMA fullfsync=OFF");
      synchronous = sync;
    }
    sqlite3_bind_blob(put, 1, k.data(), static_cast<int>(k.size()), SQLITE_STATIC);
    sqlite3_bind_blob(put, 2, v.data(), static_cast<int>(v.size()), SQLITE_STATIC);
    bool ok = sqlite3_step(put) == SQLITE_DONE;
    sqlite3_reset(put);
    return ok;
  }
  bool Get(const std::string& k, std::string* v) override {
    sqlite3_bind_blob(get, 1, k.data(), static_cast<int>(k.size()), SQLITE_STATIC);
    bool ok = sqlite3_step(get) == SQLITE_ROW;
    if (ok)
      v->assign(static_cast<const char*>(sqlite3_column_blob(get, 0)),
                static_cast<size_t>(sqlite3_column_bytes(get, 0)));
    sqlite3_reset(get);
    return ok;
  }
  uint64_t ScanAll() override {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "SELECT k, v FROM kv ORDER BY k", -1, &s, nullptr);
    uint64_t n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
      // Touch the payload like the iterators do.
      if (sqlite3_column_bytes(s, 1) < 0) break;
      ++n;
    }
    sqlite3_finalize(s);
    return n;
  }
  void Destroy(const std::string& path) override {
    Close();
    std::string rm = "rm -rf '" + path + "'";
    if (std::system(rm.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  }
};
#endif

std::string KeyOf(uint64_t i) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llu", static_cast<unsigned long long>(i));
  return {b, 16};
}

struct Result {
  std::string name;
  uint64_t ops;
  double seconds;
  uint64_t bytes;
};

}  // namespace

int main(int argc, char** argv) {
  std::string engine = "sediment", benchmarks = "fillseq,fillrandom,overwrite,readrandom,readmissing,readseq,fillsync";
  std::string path = "/tmp/db_bench";
  uint64_t num = 1000000;
  size_t value_size = 100;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* p) { return a.substr(std::strlen(p)); };
    if (a.rfind("--engine=", 0) == 0)
      engine = val("--engine=");
    else if (a.rfind("--benchmarks=", 0) == 0)
      benchmarks = val("--benchmarks=");
    else if (a.rfind("--num=", 0) == 0)
      num = std::stoull(val("--num="));
    else if (a.rfind("--value_size=", 0) == 0)
      value_size = std::stoul(val("--value_size="));
    else if (a.rfind("--db=", 0) == 0)
      path = val("--db=");
    else {
      std::fprintf(stderr, "unknown flag %s\n", a.c_str());
      return 2;
    }
  }
  std::unique_ptr<Engine> e;
  if (engine == "sediment") e = std::make_unique<Sediment>();
#ifdef HAVE_LEVELDB
  else if (engine == "leveldb")
    e = std::make_unique<LevelDB>();
#endif
#ifdef HAVE_SQLITE
  else if (engine == "sqlite")
    e = std::make_unique<SQLite>();
#endif
  if (!e) {
    std::fprintf(stderr, "engine %s not available\n", engine.c_str());
    return 2;
  }

  std::mt19937_64 rng(301);
  std::string value(value_size, 'x');
  auto refill_value = [&] {
    for (size_t i = 0; i < value.size(); i += 8) {
      uint64_t r = rng();
      std::memcpy(&value[i], &r, std::min<size_t>(8, value.size() - i));
    }
  };
  std::vector<uint64_t> perm(num);
  for (uint64_t i = 0; i < num; ++i) perm[i] = i;
  std::shuffle(perm.begin(), perm.end(), rng);

  std::vector<Result> results;
  std::stringstream list(benchmarks);
  std::string b;
  bool opened = false;
  auto fresh = [&] {
    e->Destroy(path);
    opened = e->Open(path);
    if (!opened) std::fprintf(stderr, "open failed\n");
  };
  while (std::getline(list, b, ',')) {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t ops = 0, bytes = 0, found = 0;
    if (b == "fillseq" || b == "fillrandom") {
      fresh();
      t0 = std::chrono::steady_clock::now();
      for (uint64_t i = 0; i < num; ++i) {
        refill_value();
        if (!e->Put(KeyOf(b == "fillseq" ? i : perm[i]), value, false)) return 1;
      }
      ops = num;
      bytes = num * (16 + value_size);
    } else if (b == "overwrite") {
      for (uint64_t i = 0; i < num; ++i) {
        refill_value();
        if (!e->Put(KeyOf(rng() % num), value, false)) return 1;
      }
      ops = num;
      bytes = num * (16 + value_size);
    } else if (b == "readrandom" || b == "readmissing") {
      // Reopen first so reads come from tables, not the write buffer.
      opened = e->Open(path);
      t0 = std::chrono::steady_clock::now();
      std::string v;
      for (uint64_t i = 0; i < num; ++i) {
        std::string k = KeyOf(rng() % num);
        if (b == "readmissing") k.back() = 'x';  // same prefix, never present
        found += e->Get(k, &v);
      }
      ops = num;
    } else if (b == "readseq") {
      ops = e->ScanAll();
    } else if (b == "fillsync") {
      fresh();
      t0 = std::chrono::steady_clock::now();
      uint64_t n = std::max<uint64_t>(1, num / 1000);
      for (uint64_t i = 0; i < n; ++i) {
        refill_value();
        if (!e->Put(KeyOf(perm[i]), value, true)) return 1;
      }
      ops = n;
      bytes = n * (16 + value_size);
    } else {
      std::fprintf(stderr, "unknown benchmark %s\n", b.c_str());
      return 2;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-10s %-12s %10llu ops %9.3f s %10.3f us/op %10.0f ops/s", engine.c_str(), b.c_str(),
                static_cast<unsigned long long>(ops), secs, secs * 1e6 / static_cast<double>(ops),
                static_cast<double>(ops) / secs);
    if (bytes) std::printf(" %7.1f MB/s", static_cast<double>(bytes) / secs / 1e6);
    if (b == "readrandom" || b == "readmissing") std::printf(" (%llu found)", static_cast<unsigned long long>(found));
    std::printf("\n");
    std::fflush(stdout);
  }
  e->Destroy(path);
  return 0;
}
