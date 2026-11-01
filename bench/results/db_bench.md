| Benchmark | sediment | leveldb | sqlite | sediment / leveldb |
|---|---:|---:|---:|---:|
| fillseq | 205,514 | 241,677 | 94,553 | 0.85x |
| fillrandom | 165,148 | 176,462 | 50,185 | 0.94x |
| overwrite | 141,598 | 150,400 | 63,720 | 0.94x |
| readrandom | 905,732 | 954,730 | 215,375 | 0.95x |
| readmissing | 3,455,743 | 1,833,228 | 216,718 | 1.89x |
| readseq | 17,537,437 | 33,962,825 | 16,716,957 | 0.52x |
| fillsync | 326 | 324 | 1,546 | 1.01x |

Median ops/s over 5 runs.
