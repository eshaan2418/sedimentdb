| Benchmark | sediment | leveldb | sqlite | sediment / leveldb |
|---|---:|---:|---:|---:|
| fillseq | 204,449 | 242,809 | 93,792 | 0.84x |
| fillrandom | 162,172 | 173,328 | 49,885 | 0.94x |
| overwrite | 138,492 | 149,752 | 61,985 | 0.92x |
| readrandom | 898,292 | 888,730 | 204,884 | 1.01x |
| readmissing | 3,366,921 | 1,792,842 | 204,579 | 1.88x |
| readseq | 17,744,235 | 34,235,243 | 16,016,946 | 0.52x |
| fillsync | 325 | 329 | 1,365 | 0.99x |

Median ops/s over 5 runs.
