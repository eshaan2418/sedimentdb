// DB implementation.
//
// Concurrency model:
//   * `mu_` protects all mutable state except memtable contents.
//   * Writers queue up; the writer at the front (the leader) merges queued
//     batches into one group, appends it to the WAL with a single write (and
//     a single fsync if any writer in the group asked for sync), applies it
//     to the memtable, then wakes the group (group commit).
//   * Only the leader touches the WAL and inserts into the memtable, and it
//     does so with mu_ released; readers access the skiplist lock-free. Any
//     other code that must swap the WAL goes through the writer queue.
//   * One background thread flushes the immutable memtable to L0 and runs
//     compactions. Versions are immutable and own their (mmap'd) tables, so
//     readers never block on it and never lose a file they are reading.

#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "dbformat.h"
#include "env.h"
#include "iterator.h"
#include "log.h"
#include "memtable.h"
#include "sediment/db.h"
#include "table.h"
#include "version.h"
#include "write_batch_internal.h"

namespace sediment {

namespace {

// Concatenates the disjoint, sorted files of one level.
class LevelIterator final : public Iterator {
 public:
  explicit LevelIterator(std::vector<FileMeta> files) : files_(std::move(files)) {}

  [[nodiscard]] bool Valid() const override { return cur_ && cur_->Valid(); }
  [[nodiscard]] std::string_view key() const override { return cur_->key(); }
  [[nodiscard]] std::string_view value() const override { return cur_->value(); }
  [[nodiscard]] Status status() const override { return cur_ ? cur_->status() : Status::OK(); }
  void SeekToFirst() override {
    Load(0);
    if (cur_) cur_->SeekToFirst();
    Skip();
  }
  void Seek(std::string_view t) override {
    auto it = std::lower_bound(files_.begin(), files_.end(), t,
                               [](const FileMeta& f, std::string_view k) { return CompareInternal(f.largest, k) < 0; });
    Load(static_cast<size_t>(it - files_.begin()));
    if (cur_) cur_->Seek(t);
    Skip();
  }
  void Next() override {
    cur_->Next();
    Skip();
  }

 private:
  void Load(size_t i) {
    idx_ = i;
    cur_ = i < files_.size() ? Table::NewIterator(files_[i].table) : nullptr;
  }
  void Skip() {
    while (cur_ && !cur_->Valid() && cur_->status().ok() && idx_ + 1 < files_.size()) {
      Load(idx_ + 1);
      cur_->SeekToFirst();
    }
  }
  std::vector<FileMeta> files_;
  size_t idx_ = 0;
  std::unique_ptr<Iterator> cur_;
};

// Turns an internal-key stream into user-visible entries as of `seq`.
class DBIter final : public DBIterator {
 public:
  DBIter(std::unique_ptr<Iterator> it, SequenceNumber seq, std::vector<std::shared_ptr<const void>> pins)
      : it_(std::move(it)), seq_(seq), pins_(std::move(pins)) {}

  [[nodiscard]] bool Valid() const override { return valid_; }
  [[nodiscard]] std::string_view key() const override { return key_; }
  [[nodiscard]] std::string_view value() const override { return value_; }
  [[nodiscard]] Status status() const override { return it_->status(); }

  void SeekToFirst() override {
    it_->SeekToFirst();
    FindNextVisible(false, {});
  }
  void Seek(std::string_view k) override {
    it_->Seek(LookupKey(k, seq_));
    FindNextVisible(false, {});
  }
  void Next() override { FindNextVisible(true, key_); }

 private:
  // Moves to the newest visible, non-deleted version of the next user key,
  // first passing over remaining entries for `skip` if `skipping`.
  void FindNextVisible(bool skipping, std::string skip) {
    valid_ = false;
    while (it_->Valid()) {
      std::string_view ik = it_->key();
      std::string_view uk = ExtractUserKey(ik);
      if (ExtractSeq(ik) > seq_ || (skipping && uk == skip)) {
        it_->Next();
        continue;
      }
      if (ExtractType(ik) == kTypeDeletion) {
        skip.assign(uk);
        skipping = true;
        it_->Next();
        continue;
      }
      key_.assign(uk);
      value_.assign(it_->value());
      valid_ = true;
      return;
    }
  }

  std::unique_ptr<Iterator> it_;
  SequenceNumber seq_;
  std::vector<std::shared_ptr<const void>> pins_;  // memtables and version
  std::string key_, value_;
  bool valid_ = false;
};

}  // namespace

class DBImpl final : public DB {
 public:
  DBImpl(const Options& o, std::string path) : opts_(o), path_(std::move(path)) {}
  ~DBImpl() override;

  Status Recover();

  Status Put(const WriteOptions& o, std::string_view k, std::string_view v) override {
    WriteBatch b;
    b.Put(k, v);
    return Write(o, &b);
  }
  Status Delete(const WriteOptions& o, std::string_view k) override {
    WriteBatch b;
    b.Delete(k);
    return Write(o, &b);
  }
  Status Write(const WriteOptions& o, WriteBatch* batch) override;
  Status Get(const ReadOptions& o, std::string_view key, std::string* value) override;
  std::unique_ptr<DBIterator> NewIterator(const ReadOptions& o) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* s) override;
  Status CompactAll() override;
  std::string Stats() override;
  int NumFilesAtLevel(int level) override {
    std::lock_guard<std::mutex> l(mu_);
    return current_->NumFiles(level);
  }

 private:
  struct Writer {
    WriteBatch* batch;
    bool sync;
    bool done = false;
    Status status;
    std::condition_variable cv;
  };

  Status MakeRoomForWrite(std::unique_lock<std::mutex>& l);
  WriteBatch* BuildGroup(Writer** last);
  Status NewLog();
  Status LogAndApplyLocked(VersionEdit* e, std::unique_lock<std::mutex>& l);
  Status WriteManifestSnapshot();
  void BackgroundLoop();
  bool NeedsCompactionLocked() const;
  Status FlushImmLocked(std::unique_lock<std::mutex>& l);
  Status CompactLocked(std::unique_lock<std::mutex>& l);
  Status WriteTable(Iterator* it, uint64_t number, FileMeta* meta);
  Status OpenTableFor(FileMeta* f);
  uint64_t NewFileNumber();
  void DeleteObsoleteFilesLocked();
  uint64_t MaxBytesForLevel(int level) const;

  const Options opts_;
  const std::string path_;
  int lock_fd_ = -1;

  std::mutex mu_;
  std::condition_variable bg_cv_;    // background progress
  std::condition_variable work_cv_;  // wakes the background thread
  std::deque<Writer*> writers_;
  WriteBatch group_batch_;
  bool force_switch_ = false;

  std::shared_ptr<MemTable> mem_, imm_;
  std::unique_ptr<LogWriter> log_;
  uint64_t log_number_ = 0;            // WAL receiving new writes
  uint64_t persisted_log_number_ = 0;  // oldest WAL still needed (from MANIFEST)
  std::unique_ptr<LogWriter> manifest_;
  uint64_t manifest_number_ = 0;
  uint64_t next_file_ = 2;
  SequenceNumber last_seq_ = 0;
  std::shared_ptr<const Version> current_ = std::make_shared<Version>();
  std::multiset<SequenceNumber> snapshots_;
  std::set<uint64_t> pending_outputs_;
  std::string compact_pointer_[kNumLevels];
  Status bg_error_;
  bool shutting_down_ = false;
  bool bg_busy_ = false;
  std::thread bg_;
};

// ---------------------------------------------------------------------------
// Open / recovery

Status DB::Open(const Options& opts, const std::string& path, std::unique_ptr<DB>* db) {
  auto impl = std::make_unique<DBImpl>(opts, path);
  Status s = impl->Recover();
  if (s.ok()) *db = std::move(impl);
  return s;
}

uint64_t DBImpl::NewFileNumber() { return next_file_++; }

Status DBImpl::OpenTableFor(FileMeta* f) { return Table::Open(TableFileName(path_, f->number), &f->table); }

Status DBImpl::Recover() {
  if (Status s = CreateDir(path_); !s.ok()) return s;
  if (Status s = LockFile(path_ + "/LOCK", &lock_fd_); !s.ok()) return s;

  bool exists = FileExists(CurrentFileName(path_));
  if (exists && opts_.error_if_exists) return Status::InvalidArgument(path_ + " exists");
  if (!exists && !opts_.create_if_missing) return Status::InvalidArgument(path_ + " does not exist");

  // 1. Replay the MANIFEST into a version, then open its tables.
  if (exists) {
    std::string current;
    if (Status s = ReadFileToString(CurrentFileName(path_), &current); !s.ok()) return s;
    if (current.empty() || current.back() != '\n') return Status::Corruption("CURRENT is malformed");
    current.pop_back();
    std::string contents;
    if (Status s = ReadFileToString(path_ + "/" + current, &contents); !s.ok()) return s;
    LogReader rd(std::move(contents));
    std::string_view rec;
    std::shared_ptr<const Version> v = std::make_shared<Version>();
    while (rd.ReadRecord(&rec)) {
      VersionEdit e;
      if (Status s = e.DecodeFrom(rec); !s.ok()) return s;
      v = ApplyEdit(*v, e);
      if (e.has_log_number) persisted_log_number_ = e.log_number;
      if (e.has_next_file) next_file_ = std::max(next_file_, e.next_file);
      if (e.has_last_seq) last_seq_ = std::max(last_seq_, e.last_seq);
    }
    auto opened = std::make_shared<Version>(*v);
    for (auto& fs : opened->files) {
      for (auto& f : fs) {
        if (Status s = OpenTableFor(&f); !s.ok()) return s;
      }
    }
    current_ = opened;
  }

  // 2. Replay WAL files not yet covered by tables.
  std::vector<std::string> names;
  if (Status s = ListDir(path_, &names); !s.ok()) return s;
  std::vector<uint64_t> logs;
  for (const auto& n : names) {
    uint64_t num;
    FileType t;
    if (!ParseFileName(n, &num, &t)) continue;
    if (t == FileType::kLog && num >= persisted_log_number_) logs.push_back(num);
    next_file_ = std::max(next_file_, num + 1);
  }
  std::sort(logs.begin(), logs.end());
  mem_ = std::make_shared<MemTable>();
  VersionEdit edit;
  for (uint64_t num : logs) {
    std::string contents;
    if (Status s = ReadFileToString(LogFileName(path_, num), &contents); !s.ok()) return s;
    LogReader rd(std::move(contents));
    std::string_view rec;
    // Stops at a torn or corrupt tail, which was never acknowledged.
    while (rd.ReadRecord(&rec)) {
      WriteBatch b;
      if (Status s = WriteBatchInternal::SetContents(&b, rec); !s.ok()) return s;
      if (Status s = WriteBatchInternal::InsertInto(b, mem_.get()); !s.ok()) return s;
      if (b.Count() > 0) last_seq_ = std::max(last_seq_, WriteBatchInternal::Sequence(b) + b.Count() - 1);
    }
  }
  if (!mem_->Empty()) {
    FileMeta meta;
    auto it = mem_->NewIterator();
    if (Status s = WriteTable(it.get(), NewFileNumber(), &meta); !s.ok()) return s;
    edit.added.emplace_back(0, meta);
    mem_ = std::make_shared<MemTable>();
  }

  // 3. Start a fresh WAL and a MANIFEST describing the recovered state.
  if (Status s = NewLog(); !s.ok()) return s;
  current_ = ApplyEdit(*current_, edit);
  persisted_log_number_ = log_number_;
  if (Status s = WriteManifestSnapshot(); !s.ok()) return s;

  std::lock_guard<std::mutex> l(mu_);
  DeleteObsoleteFilesLocked();
  bg_ = std::thread([this] { BackgroundLoop(); });
  return Status::OK();
}

Status DBImpl::NewLog() {
  uint64_t num = NewFileNumber();
  std::unique_ptr<WritableFile> f;
  if (Status s = WritableFile::Open(LogFileName(path_, num), &f); !s.ok()) return s;
  if (log_) log_->Close();
  log_ = std::make_unique<LogWriter>(std::move(f));
  log_number_ = num;
  return Status::OK();
}

// Writes a new MANIFEST with the full current state, then points CURRENT at
// it atomically (write temp, fsync, rename, fsync directory).
Status DBImpl::WriteManifestSnapshot() {
  uint64_t num = NewFileNumber();
  std::unique_ptr<WritableFile> f;
  if (Status s = WritableFile::Open(ManifestFileName(path_, num), &f); !s.ok()) return s;
  auto w = std::make_unique<LogWriter>(std::move(f));
  VersionEdit e;
  e.SetLogNumber(persisted_log_number_);
  e.SetNextFile(next_file_);
  e.SetLastSequence(last_seq_);
  for (int l = 0; l < kNumLevels; ++l) {
    for (const auto& fm : current_->files[l]) e.added.emplace_back(l, fm);
  }
  std::string rec;
  e.EncodeTo(&rec);
  Status s = w->AddRecord(rec);
  if (s.ok()) s = w->Sync();
  if (!s.ok()) return s;
  char name[64];
  std::snprintf(name, sizeof name, "MANIFEST-%06llu\n", static_cast<unsigned long long>(num));
  std::string tmp = path_ + "/CURRENT.tmp";
  if (s = WriteStringToFileSync(tmp, name); !s.ok()) return s;
  if (s = RenameFile(tmp, CurrentFileName(path_)); !s.ok()) return s;
  if (s = SyncDir(path_); !s.ok()) return s;
  manifest_ = std::move(w);
  manifest_number_ = num;
  return Status::OK();
}

// Appends an edit to the MANIFEST (fsynced) and installs the new version.
// Only the background thread calls this after Open, so the manifest writer
// is never shared and mu_ can be released during the fsync.
Status DBImpl::LogAndApplyLocked(VersionEdit* e, std::unique_lock<std::mutex>& l) {
  e->SetNextFile(next_file_);
  e->SetLastSequence(last_seq_);
  // Compactions must not advance the log number: an unflushed immutable
  // memtable may still live in an older WAL.
  if (!e->has_log_number) e->SetLogNumber(persisted_log_number_);
  std::string rec;
  e->EncodeTo(&rec);
  l.unlock();
  Status s = manifest_->AddRecord(rec);
  if (s.ok()) s = manifest_->Sync();
  l.lock();
  if (!s.ok()) return s;
  current_ = ApplyEdit(*current_, *e);
  persisted_log_number_ = e->log_number;
  return Status::OK();
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> l(mu_);
    shutting_down_ = true;
    work_cv_.notify_all();
  }
  if (bg_.joinable()) bg_.join();
  if (log_) log_->Close();
  if (manifest_) manifest_->Close();
  if (lock_fd_ >= 0) UnlockFile(lock_fd_);
}

// ---------------------------------------------------------------------------
// Writes

Status DBImpl::Write(const WriteOptions& o, WriteBatch* batch) {
  Writer w{batch, o.sync, false, Status(), {}};
  std::unique_lock<std::mutex> l(mu_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) w.cv.wait(l);
  if (w.done) return w.status;

  Status s = MakeRoomForWrite(l);
  Writer* last = &w;
  if (s.ok() && batch->Count() > 0) {
    WriteBatch* group = BuildGroup(&last);
    WriteBatchInternal::SetSequence(group, last_seq_ + 1);
    // Publish the new sequence only after the memtable insert, so readers
    // and snapshots never observe a half-applied group.
    const SequenceNumber new_last = last_seq_ + group->Count();
    bool sync = false;
    for (Writer* x : writers_) {
      sync |= x->sync;
      if (x == last) break;
    }
    std::shared_ptr<MemTable> mem = mem_;
    l.unlock();
    s = log_->AddRecord(WriteBatchInternal::Contents(*group));
    if (s.ok() && sync) s = log_->Sync();
    if (s.ok()) s = WriteBatchInternal::InsertInto(*group, mem.get());
    l.lock();
    if (s.ok()) last_seq_ = new_last;
    if (group == &group_batch_) group_batch_.Clear();
    if (!s.ok() && bg_error_.ok()) bg_error_ = s;  // the WAL may now be inconsistent
  }
  for (;;) {
    Writer* x = writers_.front();
    writers_.pop_front();
    if (x != &w) {
      x->status = s;
      x->done = true;
      x->cv.notify_one();
    }
    if (x == last) break;
  }
  if (!writers_.empty()) writers_.front()->cv.notify_one();
  return s;
}

// Folds queued followers into the leader's batch (up to ~1 MB). A sync
// write never joins a group led by a non-sync writer.
WriteBatch* DBImpl::BuildGroup(Writer** last) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  size_t size = first->batch->ByteSize();
  size_t max_size = size <= (128 << 10) ? size + (128 << 10) : 1 << 20;
  *last = first;
  for (size_t i = 1; i < writers_.size(); ++i) {
    Writer* w = writers_[i];
    if (w->sync && !first->sync) break;
    if (w->batch->Count() == 0) break;  // a control write (forced memtable switch)
    size += w->batch->ByteSize();
    if (size > max_size) break;
    if (result == first->batch) {
      group_batch_.Clear();
      WriteBatchInternal::Append(&group_batch_, *first->batch);
      result = &group_batch_;
    }
    WriteBatchInternal::Append(result, *w->batch);
    *last = w;
  }
  return result;
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& l) {
  bool delayed = false;
  for (;;) {
    if (!bg_error_.ok()) return bg_error_;
    int l0 = current_->NumFiles(0);
    if (!delayed && l0 >= opts_.l0_slowdown_trigger) {
      // Gentle back-pressure before L0 reaches the hard stop.
      l.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      l.lock();
      delayed = true;
    } else if (!force_switch_ && mem_->ApproximateMemoryUsage() < opts_.write_buffer_size) {
      return Status::OK();
    } else if (imm_ != nullptr || l0 >= opts_.l0_stop_trigger) {
      bg_cv_.wait(l);  // let the flush / compaction catch up
    } else if (force_switch_ && mem_->Empty()) {
      force_switch_ = false;
      return Status::OK();
    } else {
      if (Status s = NewLog(); !s.ok()) return s;
      imm_ = std::move(mem_);
      mem_ = std::make_shared<MemTable>();
      force_switch_ = false;
      work_cv_.notify_one();
    }
  }
}

// ---------------------------------------------------------------------------
// Reads

Status DBImpl::Get(const ReadOptions& o, std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> v;
  SequenceNumber seq;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    v = current_;
    seq = o.snapshot ? o.snapshot->sequence() : last_seq_;
  }
  bool deleted = false;
  if (mem->Get(key, seq, value, &deleted) || (imm && imm->Get(key, seq, value, &deleted))) {
    return deleted ? Status::NotFound() : Status::OK();
  }
  std::string lookup = LookupKey(key, seq);
  std::string ik;
  // Returns true when the file decided the answer (value or tombstone).
  auto probe = [&](const FileMeta& f, Status* s) {
    bool found = false;
    *s = f.table->Get(lookup, &found, &ik, value);
    if (!s->ok()) return true;
    if (!found) return false;
    *s = ExtractType(ik) == kTypeDeletion ? Status::NotFound() : Status::OK();
    return true;
  };
  Status s;
  // L0 files may overlap each other: check newest first.
  const auto& l0 = v->files[0];
  for (auto it = l0.rbegin(); it != l0.rend(); ++it) {
    if (CompareUser(key, ExtractUserKey(it->smallest)) < 0 || CompareUser(key, ExtractUserKey(it->largest)) > 0)
      continue;
    if (probe(*it, &s)) return s;
  }
  for (int level = 1; level < kNumLevels; ++level) {
    const auto& fs = v->files[level];
    auto it = std::lower_bound(fs.begin(), fs.end(), lookup, [](const FileMeta& f, const std::string& k) {
      return CompareInternal(f.largest, k) < 0;
    });
    if (it == fs.end() || CompareUser(key, ExtractUserKey(it->smallest)) < 0) continue;
    if (probe(*it, &s)) return s;
  }
  return Status::NotFound();
}

std::unique_ptr<DBIterator> DBImpl::NewIterator(const ReadOptions& o) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> v;
  SequenceNumber seq;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    v = current_;
    seq = o.snapshot ? o.snapshot->sequence() : last_seq_;
  }
  std::vector<std::unique_ptr<Iterator>> children;
  std::vector<std::shared_ptr<const void>> pins{mem, v};
  children.push_back(mem->NewIterator());
  if (imm) {
    children.push_back(imm->NewIterator());
    pins.push_back(imm);
  }
  for (const auto& f : v->files[0]) children.push_back(Table::NewIterator(f.table));
  for (int l = 1; l < kNumLevels; ++l) {
    if (!v->files[l].empty()) children.push_back(std::make_unique<LevelIterator>(v->files[l]));
  }
  return std::make_unique<DBIter>(NewMergingIterator(std::move(children)), seq, std::move(pins));
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> l(mu_);
  snapshots_.insert(last_seq_);
  return new Snapshot(last_seq_);
}

void DBImpl::ReleaseSnapshot(const Snapshot* s) {
  {
    std::lock_guard<std::mutex> l(mu_);
    snapshots_.erase(snapshots_.find(s->sequence()));
  }
  delete s;
}

// ---------------------------------------------------------------------------
// Background work

uint64_t DBImpl::MaxBytesForLevel(int level) const {
  uint64_t b = opts_.max_bytes_level1;
  for (int l = 1; l < level; ++l) b *= 10;
  return b;
}

bool DBImpl::NeedsCompactionLocked() const {
  if (current_->NumFiles(0) >= opts_.l0_compaction_trigger) return true;
  for (int l = 1; l < kNumLevels - 1; ++l) {
    if (current_->LevelBytes(l) > MaxBytesForLevel(l)) return true;
  }
  return false;
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> l(mu_);
  for (;;) {
    while (!shutting_down_ && bg_error_.ok() && imm_ == nullptr && !NeedsCompactionLocked()) work_cv_.wait(l);
    if (shutting_down_ || !bg_error_.ok()) break;
    bg_busy_ = true;
    Status s = imm_ != nullptr ? FlushImmLocked(l) : CompactLocked(l);
    bg_busy_ = false;
    if (!s.ok()) bg_error_ = s;
    DeleteObsoleteFilesLocked();
    bg_cv_.notify_all();
  }
  bg_cv_.notify_all();
}

Status DBImpl::WriteTable(Iterator* it, uint64_t number, FileMeta* meta) {
  std::unique_ptr<WritableFile> f;
  if (Status s = WritableFile::Open(TableFileName(path_, number), &f); !s.ok()) return s;
  TableBuilder b(TableOptions{opts_.block_size, opts_.bloom_bits_per_key}, std::move(f));
  meta->number = number;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (b.NumEntries() == 0) meta->smallest.assign(it->key());
    meta->largest.assign(it->key());
    b.Add(it->key(), it->value());
  }
  if (Status s = it->status(); !s.ok()) return s;
  if (Status s = b.Finish(); !s.ok()) return s;
  meta->size = b.FileSize();
  return OpenTableFor(meta);
}

Status DBImpl::FlushImmLocked(std::unique_lock<std::mutex>& l) {
  std::shared_ptr<MemTable> imm = imm_;
  uint64_t num = NewFileNumber();
  pending_outputs_.insert(num);
  uint64_t covered_up_to = log_number_;  // WAL created when imm_ was frozen
  l.unlock();
  FileMeta meta;
  auto it = imm->NewIterator();
  Status s = WriteTable(it.get(), num, &meta);
  l.lock();
  pending_outputs_.erase(num);
  if (!s.ok()) return s;
  VersionEdit e;
  e.added.emplace_back(0, meta);
  e.SetLogNumber(covered_up_to);  // older WALs are now fully in tables
  s = LogAndApplyLocked(&e, l);
  if (s.ok()) imm_.reset();
  return s;
}

Status DBImpl::CompactLocked(std::unique_lock<std::mutex>& l) {
  std::shared_ptr<const Version> v = current_;
  int level = -1;
  std::vector<FileMeta> inputs[2];
  if (v->NumFiles(0) >= opts_.l0_compaction_trigger) {
    level = 0;
    inputs[0] = v->files[0];
  } else {
    double best = 1.0;
    for (int lv = 1; lv < kNumLevels - 1; ++lv) {
      double score = static_cast<double>(v->LevelBytes(lv)) / static_cast<double>(MaxBytesForLevel(lv));
      if (score > best) best = score, level = lv;
    }
    if (level < 0) return Status::OK();
    // Round-robin through the level's key space.
    const auto& fs = v->files[level];
    const FileMeta* pick = &fs[0];
    for (const auto& f : fs) {
      if (compact_pointer_[level].empty() || CompareInternal(f.largest, compact_pointer_[level]) > 0) {
        pick = &f;
        break;
      }
    }
    inputs[0].push_back(*pick);
  }
  std::string lo(ExtractUserKey(inputs[0][0].smallest)), hi(ExtractUserKey(inputs[0][0].largest));
  for (const auto& f : inputs[0]) {
    if (CompareUser(ExtractUserKey(f.smallest), lo) < 0) lo.assign(ExtractUserKey(f.smallest));
    if (CompareUser(ExtractUserKey(f.largest), hi) > 0) hi.assign(ExtractUserKey(f.largest));
  }
  for (const auto& f : v->files[level + 1]) {
    if (CompareUser(ExtractUserKey(f.largest), lo) >= 0 && CompareUser(ExtractUserKey(f.smallest), hi) <= 0)
      inputs[1].push_back(f);
  }
  compact_pointer_[level] = inputs[0].back().largest;
  const int out_level = level + 1;
  const SequenceNumber smallest_snapshot = snapshots_.empty() ? last_seq_ : *snapshots_.begin();

  // True if no level below the output may hold an older version of uk.
  auto is_base_level = [&](std::string_view uk) {
    for (int lv = out_level + 1; lv < kNumLevels; ++lv) {
      for (const auto& f : v->files[lv]) {
        if (CompareUser(uk, ExtractUserKey(f.smallest)) >= 0 && CompareUser(uk, ExtractUserKey(f.largest)) <= 0)
          return false;
      }
    }
    return true;
  };
  l.unlock();

  std::vector<std::unique_ptr<Iterator>> children;
  for (const auto& in : inputs) {
    for (const auto& f : in) children.push_back(Table::NewIterator(f.table));
  }
  auto it = NewMergingIterator(std::move(children));

  Status s;
  std::vector<FileMeta> outputs;
  std::vector<uint64_t> reserved;
  std::unique_ptr<TableBuilder> builder;
  FileMeta cur;
  auto finish = [&]() -> Status {
    if (!builder) return Status::OK();
    Status fs = builder->Finish();
    cur.size = builder->FileSize();
    builder.reset();
    if (fs.ok()) fs = OpenTableFor(&cur);
    if (fs.ok()) outputs.push_back(cur);
    return fs;
  };

  std::string last_user;
  bool has_last = false;
  SequenceNumber last_seq_for_key = kMaxSequence;
  for (it->SeekToFirst(); it->Valid() && s.ok(); it->Next()) {
    std::string_view ik = it->key();
    std::string_view uk = ExtractUserKey(ik);
    SequenceNumber seq = ExtractSeq(ik);
    if (!has_last || uk != last_user) {
      // A new user key: the only place an output file may end, so files in
      // L1+ keep disjoint user-key ranges.
      if (builder && builder->FileSize() >= opts_.target_file_size) s = finish();
      last_user.assign(uk);
      has_last = true;
      last_seq_for_key = kMaxSequence;
    }
    bool drop = false;
    if (last_seq_for_key <= smallest_snapshot) {
      drop = true;  // shadowed by a newer version every snapshot can see
    } else if (ExtractType(ik) == kTypeDeletion && seq <= smallest_snapshot && is_base_level(uk)) {
      drop = true;  // tombstone with nothing older left to hide
    }
    last_seq_for_key = seq;
    if (drop || !s.ok()) continue;
    if (!builder) {
      uint64_t num;
      {
        std::lock_guard<std::mutex> g(mu_);
        num = NewFileNumber();
        pending_outputs_.insert(num);
      }
      reserved.push_back(num);
      std::unique_ptr<WritableFile> f;
      if (s = WritableFile::Open(TableFileName(path_, num), &f); !s.ok()) break;
      builder = std::make_unique<TableBuilder>(TableOptions{opts_.block_size, opts_.bloom_bits_per_key}, std::move(f));
      cur = FileMeta{};
      cur.number = num;
      cur.smallest.assign(ik);
    }
    builder->Add(ik, it->value());
    cur.largest.assign(ik);
  }
  if (s.ok()) s = it->status();
  if (s.ok()) s = finish();

  l.lock();
  for (uint64_t n : reserved) pending_outputs_.erase(n);
  if (!s.ok()) return s;
  VersionEdit e;
  for (const auto& f : inputs[0]) e.deleted.emplace_back(level, f.number);
  for (const auto& f : inputs[1]) e.deleted.emplace_back(out_level, f.number);
  for (const auto& f : outputs) e.added.emplace_back(out_level, f);
  return LogAndApplyLocked(&e, l);
}

void DBImpl::DeleteObsoleteFilesLocked() {
  std::set<uint64_t> live(pending_outputs_.begin(), pending_outputs_.end());
  for (const auto& fs : current_->files) {
    for (const auto& f : fs) live.insert(f.number);
  }
  std::vector<std::string> names;
  if (!ListDir(path_, &names).ok()) return;
  for (const auto& n : names) {
    uint64_t num;
    FileType t;
    if (!ParseFileName(n, &num, &t)) continue;
    bool keep = true;
    switch (t) {
      case FileType::kLog: keep = num >= persisted_log_number_ || num == log_number_; break;
      case FileType::kTable: keep = live.count(num) > 0; break;
      case FileType::kManifest: keep = num >= manifest_number_; break;
      default: break;
    }
    // Readers keep their mappings (and therefore the data) after unlink.
    if (!keep) RemoveFile(path_ + "/" + n);
  }
}

Status DBImpl::CompactAll() {
  {
    std::lock_guard<std::mutex> l(mu_);
    force_switch_ = true;
  }
  // Swapping the WAL is the write leader's job, so go through the queue.
  WriteBatch empty;
  if (Status s = Write(WriteOptions(), &empty); !s.ok()) return s;
  std::unique_lock<std::mutex> l(mu_);
  while (bg_error_.ok() && (imm_ != nullptr || bg_busy_ || NeedsCompactionLocked())) {
    work_cv_.notify_one();
    bg_cv_.wait(l);
  }
  return bg_error_;
}

std::string DBImpl::Stats() {
  std::lock_guard<std::mutex> l(mu_);
  std::ostringstream o;
  for (int lv = 0; lv < kNumLevels; ++lv) {
    if (current_->NumFiles(lv) == 0) continue;
    o << "L" << lv << ": " << current_->NumFiles(lv) << " files, " << current_->LevelBytes(lv) / 1024 << " KiB\n";
  }
  return o.str();
}

Status DestroyDB(const std::string& path) {
  std::vector<std::string> names;
  if (!ListDir(path, &names).ok()) return Status::OK();
  for (const auto& n : names) {
    uint64_t num;
    FileType t;
    if (ParseFileName(n, &num, &t) || n == "CURRENT.tmp") RemoveFile(path + "/" + n);
  }
  ::rmdir(path.c_str());
  return Status::OK();
}

}  // namespace sediment
