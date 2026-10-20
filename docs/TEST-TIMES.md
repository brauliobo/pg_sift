# Test times

Where the time of `scripts/ci.sh quick` goes, and why the jobs have the size they have. The sizes themselves are in `scripts/ci.sh` (each job line, with the
comment that says what the size must keep); this file is how to measure and what must not shrink.

## Measuring

```
KEEP=1 SIFT_CI_TMP=/tmp/ci-times scripts/ci.sh quick
SIFT_CI_TMP=/tmp/ci-times scripts/ci.sh --times     # every job: wall, CPU, lane, start; the 15 longest first
```

`logs/results` has one line per job: `id suite status wall skips lane start end cpu`. CPU is the user + system time of the job and every process it
started (`scripts/cpu_time.py`), plus the lane's postmaster for jobs that use the lane cluster. Under ASan the postmaster share is under-recorded:
compare sanitize runs by wall time. Compare runs with the same number of lanes (`SIFT_LANES`) and the same CPU cap.

## What sets a job's size

A quick job is the smallest input that still reaches what the job tests, checked once against the full run: a differential corpus keeps the same shape
tags and 0 `DIFFER` as the full one; the document counts cross a superblock boundary (past 2200 documents; the `drop_sb_boundary` mutant survives below it)
and let every mutant be killed (2400 documents let `tie_ctid_desc` survive); a fuzz target runs a few seconds while the corpus is replayed in full
(`make -C test/fuzz regress`); a job bound by sleeps (replica catch-up) or by its own size (`build_large_docs`, the documents beyond the large-document
limits) is not shrunk. The short regression tests run twelve to a `pg_regress` run (a run costs 0.3 s of CPU before its first statement), except the tests
with a `sift_table`, which keep a database of their own: VACUUM seals only the rows older than every snapshot of the database, and an autovacuum worker on
the tables an earlier test left behind holds one.

## Not shrunk, on purpose

The known-bug tests keep their sizes: `col_vacuum_scan`, `vacuum_count_snapshot`, `vacuum_count_stress`, `tbl_cap`, `tbl_vblocks`, `tbl_bigtx`, `t/013_tbl_bigtx`,
`t/011_tbl_fetch_vacuum`, `read_corrupt`, the column-chunk seeds, `col_agg_nofilter` and `col_topn`. Their sizes are what makes the bug reach its
code path.

`cpu_gate` and the BM25 gate are benchmarks, not CI jobs.
