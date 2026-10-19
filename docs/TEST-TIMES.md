# Test times

Where the time of `scripts/ci.sh quick` goes, and why each job has the size it has. Measured with two lanes under the 2-core cap
(`SIFT_LANES=2`, `CPUQuota=200%`, `nice`), the non-ASan build, jobs run one per lane at a time.

## Measuring

```
KEEP=1 SIFT_CI_TMP=/tmp/ci-times scripts/ci.sh quick
SIFT_CI_TMP=/tmp/ci-times scripts/ci.sh --times     # every job: wall, CPU, lane, start; the 15 longest first
```

`logs/results` has one line per job: `id suite status wall skips lane start end cpu`. CPU is the user + system time of the job and every process it
started (`scripts/cpu_time.py`), plus the lane's postmaster for jobs that use the lane cluster. Under ASan the postmaster share is under-recorded:
compare sanitize runs by wall time.

## Quick CI, 2 lanes

| Job | Before | After | Size now | What it is for |
|---|---|---|---|---|
| production-regress | 121 s | 61 s | the quick tests, no `SIFT_TESTING` build | the tests that need no test-only function, against the production library; `build_large_docs` (fixed size, below) and the slow tests are not repeated |
| fuzz | 117 s | 60 s | 2 s per target, 50k kernel rows, grid stride 7, 100 codec iterations | every fuzz target and kernel still runs; `make -C test/fuzz regress` replays the corpus |
| difftest-col | 157 s | 52 s | 1500 fixture rows, 3000 rows, variants `on,on-par`, scenarios `dml,abort,vacuum,held-lite` in 3 slices | the shape tags and the 0 DIFFER hold the same at 1500/20000 as at 4000/100000 (checked once); `--scenario-slices` rotates the turns instead of running every scenario in every slice |
| difftest | 75 s | 44 s | 2500 documents, 12 queries, 2 rounds | past 2200 documents the corpus crosses a superblock boundary (the `drop_sb_boundary` mutant survives below it) |
| hardened | 48 s | 45 s | build only | compile cost with the hardening flags, nothing to shrink |
| difftest-mutants | 48 s | 24 s | 3000 documents, 10 queries, round 0, all 8 mutants | 2400 documents let `tie_ctid_desc` survive, 2000 let `drop_sb_boundary` survive; rounds add nothing a mutant needs |
| difftest-col-selftest | 69 s | 19 s | one variant, 141 tagged queries | the harness cost is about 35 ms per query-variant run; the selftest checks the harness, not the variants |
| difftest-nopos | 34 s | 11 s | 1000 documents, 12 queries, 2 rounds | the no-positions index path |
| installcheck-index_build_postings | 74 s | 18 s | 40000 documents | the postings build, expected output unchanged |
| installcheck-facets | 32 s | 8 s | `MATERIALIZED` reference, text[] expected values | the same plan nodes, one pass over the table |
| installcheck-build_large_docs | 26 s | 19 s | fixed | the size is the test: documents beyond the large-document limits |
| tap-shell-leakcheck_gate | 40 s | 6 s | `\watch 0.02`, 0.25 s sampling | the sleep-holding subshells kept `prove` waiting for their pipes; they no longer hold them |
| soak | 32 s | 16 s | 15 s, 20000 rows, 400 rows/s, 2 clients | the table still gets autovacuum, merges and a check every 5 s |
| soak-col | 45 s | 18 s | 18 s, 6 clients | column maintenance under concurrent writers |
| tap-shell-store_replica | 29 s | 27 s | 3 s of load | bound by replica catch-up sleeps, not by size |

Whole run, one lane after the other: 1423 s of job wall and 1223 s of CPU before, 803 s and 666 s after. Wall of the pool: 729 s before, 414 s after. After the tests other branches added meanwhile (new vacuum stress tests among them) it is 433 s;
the ASan run went from 1656 s to 764 s.

## Not shrunk, on purpose

The known-bug tests keep their sizes: `col_vacuum_scan`, `vacuum_count_snapshot`, `vacuum_count_stress`, `tbl_cap`, `tbl_vblocks`,
`t/011_tbl_fetch_vacuum`, `read_corrupt`, the column-chunk seeds, `col_agg_nofilter` and `col_topn`. Their sizes are what makes the bug reach its
code path.

`cpu_gate` and the BM25 gate are benchmarks, not CI jobs, and measure the same thing as before.

## Other changes

* The short regression tests run twelve to a `pg_regress` run (a run costs 0.3 s of CPU before its first statement). The tests with a
  `sift_table` keep a database of their own: VACUUM seals only the rows older than every snapshot of the database, and an autovacuum worker on
  the tables an earlier test left behind holds one.
* `tap-010_tbl_settings` waited for the autovacuum worker of the previous table only by chance; it now polls until no autovacuum worker runs
  before ALTER TABLE.
* `difftest-col` in the full run uses 1500 fixture rows and 20000 rows.
