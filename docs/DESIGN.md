# sedimentdb design notes

## Write path

```
Put/Delete/Write ──► writer queue ──► leader: MakeRoomForWrite
                                         │  (maybe swap memtable + WAL, or stall)
                                         ▼
                       one WAL record for the whole group (+1 fsync if any writer asked)
                                         ▼
                       apply group to the memtable (lock-free readers)
                                         ▼
                       publish last_sequence, wake every writer in the group
```

- **Group commit.** Writers enqueue under `mu_`. The writer at the head of the queue folds queued batches into one, up to about 1 MB, and does the I/O with `mu_` released. A sync writer never joins a group led by a non-sync writer, so durability is never weaker than what the caller asked for.
- **Sequence publication.** `last_sequence` is advanced only after the memtable insert, so a snapshot taken during a write never later starts to see that write.
- **Single WAL owner.** Only the current leader touches the WAL. `CompactAll()` needs to swap the WAL, so it sets a flag and sends an empty control batch through the queue instead of touching the log directly.

## Read path

`Get` takes `mu_` only to copy three pointers (memtable, immutable memtable, current version) and the sequence number. It then searches, in order: the memtable, the immutable memtable, L0 files newest-first (they may overlap), and one file per deeper level, found by binary search on largest key. The first entry with sequence ≤ snapshot decides the answer, and a tombstone means NotFound.

Tables are memory-mapped. Each data block's CRC32C is verified on every access (≈ 0.2 µs per 4 KB with the ARMv8 CRC instructions), so corruption is reported as a `Corruption` status rather than returned as data.

## Lifetimes without reference counting by hand

- `Version` objects are immutable and shared via `shared_ptr`. Each `FileMeta` in a version owns the open `Table` (the mmap).
- Readers and iterators hold the version (and memtables) they started with.
- Obsolete files are unlinked as soon as the current version stops referencing them. A reader still holding an older version keeps the mapping alive, and POSIX keeps an unlinked file's pages valid for an existing mapping. No reader can ever fail to open a file that compaction deleted.

## On-disk state and recovery

| File | Contents |
|---|---|
| `NNNNNN.log` | WAL: records `[masked crc32c][len][write batch]` |
| `NNNNNN.sst` | table: data blocks, one bloom filter, index block, 40-byte footer |
| `MANIFEST-NNNNNN` | log of `VersionEdit`s (files added/removed per level, next file number, last sequence, oldest live WAL) |
| `CURRENT` | name of the live MANIFEST, replaced atomically (temp file, fsync, rename, fsync dir) |

Recovery runs in four steps:

1. Replay the MANIFEST into a version and open its tables.
2. Replay every WAL numbered at or above the MANIFEST's log number, in order. Replay stops at the first torn or corrupt record, which can only be an unacknowledged tail.
3. Flush the replayed memtable to an L0 table.
4. Write a fresh MANIFEST and start a new WAL.

The log number recorded by a *compaction* edit is the previously persisted one, not the current WAL. An unflushed immutable memtable may still live in an older WAL, and recovery must not skip it.

## Compaction

- **Trigger.** L0 reaching `l0_compaction_trigger` files (4 by default), or a level's total size exceeding its target (10 MB for L1, then ×10 per level).
- **Inputs.** For L0, every L0 file plus the overlapping L1 files. For deeper levels, one file chosen round-robin by key plus its overlap in the next level.
- **Drop rules,** applied while merging newest-first per user key:
  1. An entry is dropped if a newer entry for the same user key exists with sequence ≤ the oldest live snapshot, because every reader would see the newer one.
  2. A tombstone is dropped if its sequence ≤ the oldest snapshot and no deeper level could contain an older value.
- **Output.** Files of about 2 MB, split only at user-key boundaries so that L1+ files have disjoint user-key ranges.
- **Back-pressure.** At 8 L0 files each write sleeps 1 ms; at 12 writes block until compaction catches up.

## Testing strategy

- `tests/model_test.cc` runs 20,000 random operations per seed against a `std::map`. These cover puts, deletes, multi-key batches, gets, snapshots with full-scan verification, forced compactions and reopens, with tiny buffers so every code path runs constantly.
- `tools/crashtest.cc` (part of `ctest`) runs a child process that writes with `sync=true` and reports each acknowledged key over a pipe. A second thread churns non-sync writes to keep flushes and compactions in flight. The parent `SIGKILL`s the child at a random moment, then checks every acknowledged key, earlier rounds' data, and a full ordered scan.
- Corruption tests flip bytes in table blocks and WAL records and truncate files.
