# Coverage and plan gates

What `pg_sift` answers, which plan the planner picks, and the 6.0 ClickHouse-class feature gates. Summary: [README](../../README.md#benchmarks). Protocol: [BENCHMARKS.md](../BENCHMARKS.md).

## Coverage

[COVERAGE.md](../COVERAGE.md) lists every query shape and feature with its sample query: **Yes** (runs on a sift index or table), **Partly** (a condition), **No** (the normal PostgreSQL plan, never a wrong result) or **Error** (names the setting that avoids it). `scripts/coverage.sh --check` runs each sample with `EXPLAIN` on a test cluster and fails when a row claims what the plan does not do.

## Plan gate

`scripts/plangate.sh` runs each shape of `test/plan/shapes.sql` three ways in one session: native (`sift.enable_agg = off`), chosen (the planner's own choice) and scan (the other scans off). It checks that the results are equal, that the chosen plan matches `test/plan/bands.tsv` and that the cost ratio stays inside its band, so a drift is seen before it flips a choice. A scan that loses to native by more than 1 ms and 20% is recorded as `lose`; one that wins but was not chosen is recorded as `miss`.

## ClickHouse-parity gates

Each gate uses the same engines, datasets and `md5_equal` rule as the benchmark pages; none adds a setting or an option (27 `sift.*` settings and 12 / 14 / 10 / 0 options are unchanged).

| Gate | What is measured | Release target |
|---|---|---|
| `CH-01` | Unprojected filtered scans `05` and `07`, 10M orders, warm and cold | `sift_col` and `sift_table` tie or beat ClickHouse: `05 ≤ 7.5 ms`, `07 ≤ 4.5 ms` warm |
| `CH-02` | `sift_table` 10M orders `COPY` load time and footprint | Load `≤ 14.0 s` (≤ 1.5× ClickHouse `9.3 s`), size `≤ 420 MiB` (below ClickHouse `421.4 MiB`) |
| `CH-03` | Format: 424 B metapage, 224 B part descriptor, page types 44 and 45, three new sections | All static asserts pass; `ALTER TABLE ADD/DROP COLUMN` rewrites 0 parts; `pg_checksums` clean |
| `CH-04` | `SIFT_PROJ_SORT` and `SIFT_PROJ_AGG` under concurrent DML, `VACUUM` and crash recovery | 0 oracle diffs; projected rollups under 1 ms, matching ClickHouse `PROJECTION`s |
| `CH-05` | `file://` and S3 SigV4 tiering, `O(1)` TTL drop | 0 diffs, 0 leaks, bounded local cache; TTL is one metadata commit |
| `CH-06` | Wide and multi-column `COUNT(DISTINCT)`, `arg_max`/`arg_min`, 16-branch `FILTER` pivot, trigram `LIKE`, Grace join, `ASOF`, `date_bin`, windows | 0 diffs against stock PostgreSQL on 0, 1, 2 and 4 workers; ≥ 10× faster than stock per class |
| `CH-07` | ClickBench `Q0`-`Q42` plus the `ClickHouse + PROJECTION` arm | 43 of 43 queries `md5_equal`; results in [clickbench.md](clickbench.md) |
| `CH-08` | Zero-knob freeze and packaging | Still 27 settings and 12 / 14 / 10 / 0 options; multi-arch images, signed APT and DNF repositories ([INSTALL-REPOS.md](../INSTALL-REPOS.md)), version-sync lint |

*Not measured for 6.0 yet: `bench/bench v6-release --docs` records the gates at release; no number here is a measurement.*
