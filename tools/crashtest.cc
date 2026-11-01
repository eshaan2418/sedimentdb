// Crash-recovery test: a child process writes with sync=true and reports
// every acknowledged key over a pipe; the parent SIGKILLs it at a random
// moment, reopens the database, and checks that
//   1. every acknowledged write is present with the right value, and
//   2. the database opens cleanly and a full scan is sorted and readable.
// Repeated for --rounds rounds on the same database, so recovery runs on
// top of previous recoveries, compactions and torn WAL tails.

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#include "sediment/db.h"

using namespace sediment;

namespace {

std::string Key(uint64_t i) {
  char b[32];
  std::snprintf(b, sizeof b, "key%012llu", static_cast<unsigned long long>(i));
  return b;
}
std::string Value(uint64_t i) { return "value-" + std::to_string(i * 2654435761u) + std::string(i % 300, 'p'); }

Options TestOptions() {
  Options o;
  o.write_buffer_size = 128 << 10;  // frequent flushes and compactions
  o.max_bytes_level1 = 512 << 10;
  o.target_file_size = 128 << 10;
  return o;
}

[[noreturn]] void Child(const std::string& dir, uint64_t start, int fd) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(TestOptions(), dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "child: open failed: %s\n", s.ToString().c_str());
    _exit(2);
  }
  std::mt19937_64 rng(start);
  // A filler thread churns non-sync writes with large values so that
  // flushes and compactions are in flight when the kill lands.
  std::thread([&db, start] {
    std::mt19937_64 r(start * 7 + 1);
    std::string big(2000, 'f');
    for (uint64_t j = 0;; ++j) {
      big[j % big.size()] = static_cast<char>('a' + r() % 26);
      if (!db->Put({}, "fill" + std::to_string(r() % 20000), big).ok()) _exit(5);
    }
  }).detach();
  WriteOptions sync;
  sync.sync = true;
  for (uint64_t i = start;; ++i) {
    if (rng() % 8 == 0) {
      // Multi-key batch: either all of it is durable or none is reported.
      WriteBatch b;
      b.Put(Key(i), Value(i));
      b.Put(Key(i) + "-twin", Value(i));
      s = db->Write(sync, &b);
    } else {
      s = db->Put(sync, Key(i), Value(i));
    }
    if (!s.ok()) _exit(3);
    char line[32];
    int n = std::snprintf(line, sizeof line, "%llu\n", static_cast<unsigned long long>(i));
    if (write(fd, line, static_cast<size_t>(n)) != n) _exit(4);
  }
}

}  // namespace

int main(int argc, char** argv) {
  int rounds = 10;
  std::string dir = "/tmp/sediment-crashtest";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc)
      rounds = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--dir") && i + 1 < argc)
      dir = argv[++i];
  }
  DestroyDB(dir);
  std::mt19937 rng(12345);
  uint64_t next = 0, total_acked = 0;
  for (int round = 1; round <= rounds; ++round) {
    int p[2];
    if (pipe(p) != 0) return 1;
    pid_t pid = fork();
    if (pid == 0) {
      close(p[0]);
      Child(dir, next, p[1]);
    }
    close(p[1]);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50 + rng() % 400);
    // Read acknowledgements until the deadline, then kill without warning.
    uint64_t max_acked = 0;
    bool any = false;
    std::string buf;
    char tmp[4096];
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    while (std::chrono::steady_clock::now() < deadline) {
      ssize_t n = read(p[0], tmp, sizeof tmp);
      if (n > 0) {
        buf.append(tmp, static_cast<size_t>(n));
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    }
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    // Drain anything acknowledged before the kill landed.
    for (;;) {
      ssize_t n = read(p[0], tmp, sizeof tmp);
      if (n <= 0) break;
      buf.append(tmp, static_cast<size_t>(n));
    }
    close(p[0]);
    size_t pos = 0, nl;
    uint64_t count = 0;
    while ((nl = buf.find('\n', pos)) != std::string::npos) {
      max_acked = std::strtoull(buf.c_str() + pos, nullptr, 10);
      any = true;
      ++count;
      pos = nl + 1;
    }
    if (WIFEXITED(status)) {
      std::fprintf(stderr, "round %d: child exited early with %d\n", round, WEXITSTATUS(status));
      return 1;
    }

    std::unique_ptr<DB> db;
    Status s = DB::Open(TestOptions(), dir, &db);
    if (!s.ok()) {
      std::fprintf(stderr, "round %d: reopen failed: %s\n", round, s.ToString().c_str());
      return 1;
    }
    uint64_t missing = 0;
    if (any) {
      for (uint64_t i = next; i <= max_acked; ++i) {
        std::string v;
        if (!db->Get({}, Key(i), &v).ok() || v != Value(i)) ++missing;
      }
    }
    // Earlier rounds' data must still be there too (spot check).
    for (uint64_t i = 0; i < next; i += 97) {
      std::string v;
      if (!db->Get({}, Key(i), &v).ok() || v != Value(i)) ++missing;
    }
    std::string prev;
    uint64_t scanned = 0;
    auto it = db->NewIterator({});
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++scanned) {
      if (scanned && !(prev < it->key())) {
        std::fprintf(stderr, "round %d: scan out of order\n", round);
        return 1;
      }
      prev.assign(it->key());
    }
    if (!it->status().ok()) {
      std::fprintf(stderr, "round %d: scan error %s\n", round, it->status().ToString().c_str());
      return 1;
    }
    total_acked += count;
    std::printf(
        "round %2d: killed after %4llu acked sync writes, %5llu keys, files L0/L1/L2 = %d/%d/%d, missing=%llu\n", round,
        static_cast<unsigned long long>(count), static_cast<unsigned long long>(scanned), db->NumFilesAtLevel(0),
        db->NumFilesAtLevel(1), db->NumFilesAtLevel(2), static_cast<unsigned long long>(missing));
    if (missing) return 1;
    if (any) next = max_acked + 1;
  }
  std::printf("PASS: %d crash/recover rounds, %llu acknowledged writes verified\n", rounds,
              static_cast<unsigned long long>(total_acked));
  DestroyDB(dir);
  return 0;
}
