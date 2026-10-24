# Focused scaling suite

`bench/scaling/scaling.py` measures time against size at 3-5 points on small tables and asserts the log-log slope. It answers whether the work of one request stays flat (or logarithmic) as the table grows in units, groups, partitions, keys or IN-list size, and whether whole-table work stays linear. It needs no large dataset: the largest case is one 8 GB padded table on tmpfs, and the sizes above that are extrapolated by arithmetic.

```sh
bench/scaling/scaling.py list
bench/scaling/scaling.py run --quick                  # the first sizes of every case, a few minutes on tmpfs
bench/scaling/scaling.py run --only units,groups      # the full sizes of two cases
bench/scaling/scaling.py run --only units --sizes units=64,128,256 --reps 3
```

It uses the cluster of `PGHOST`/`PGPORT`/`PGDATABASE` (pg_sift installed). Each case lives in the schema `scal_<case>`, dropped at the end (`--keep` leaves it).

**Method.** Per size and engine: one warm-up, then `--reps` (5) runs of `EXPLAIN (ANALYZE, TIMING OFF)` at `work_mem = 4GB` and no parallel workers; the median of Planning plus Execution Time. The `sift` engine has sequential scans off, so the sift plan is the one measured; a plan without a sift node fails the query (`needs_sift`). The `native` engine (sift GUCs off) is a reference for the part of a slope that belongs to PostgreSQL; it is information, never asserted.

**Output.** One CSV line per point (`case,query,engine,size,plan_ms,exec_ms,units,pruned,node`), then `SLOPE case/query engine slope=S last=L target=T PASS|FAIL|INFO x64=MS`: S is the least-squares log-log slope over the sizes, L the slope of the last segment, and `x64` the last point extrapolated 64 times with L. Targets: `flat` (per-request work) S <= 0.30; `linear` (whole-table work) S <= 1.15; a query whose times stay under 0.5 ms at every size passes (noise). Exit status 1 when a `sift` line fails.

| case | sizes | queries |
|---|---|---|
| units | 512 / 1k / 2k / 4k units | filter reading one unit, filter matching nothing, `count(*)`, `sum`, `ORDER BY .. LIMIT`, point lookup, `IN` of 51 values |
| groups | 1k / 10k / 100k / 1M groups | `GROUP BY` count and sum, `count(DISTINCT)` per group, top 10 groups |
| partitions | 16 .. 4096 partitions | query pruned to one partition, query over all of them |
| requests | 32 / 500 / 5k / 50k values in `IN (...)` | count and sum, by group |
| keys | 10k / 31.6k / 100k keys, 100 rows each | a 51-key request, the same with a date window, its count; clustered layout (asserted flat) and arrival order (information) |

A sift_col unit is 256 heap pages (about 2 MB of heap whatever the row width), so a table of one row per page has 256 rows a unit: 1k units are 2 GB and 64k units would be 134 GB. The units case therefore builds one padded heap and a partial index (`id <= size * 256`) for each size. The keys case pads rows to 170 bytes so that the table holds more units than the 51 a request can read; with narrower rows a request reads every unit until the table passes 51 units and the slope measures that saturation.

A case is a `Case(name, sizes, setup, at, queries, per_size, native, quick)` in `bench/scaling/cases/<dimension>.py` (module attribute `CASES`); a `Query(name, sql(size), target)` has `target` `flat`, `linear` or `None`.

## The CI gate

`test/t/scaling_slope.sh` (job `tap-shell-scaling_slope`, run by `scripts/ci.sh` when `bench/scaling`, `src/col`, `src/agg` or `src/core` change) runs `scaling.py run --gate` on a flavour
of this tree built with `make SIFT_TESTING=1 SIFT_SB_SHIFT=3` (`scripts/scaling-gate.sh build`, cached): column units of 8 heap pages instead of 256, so 16k units are 1 GB of tmpfs and the whole gate
takes minutes cold and seconds warm. That geometry is not the shipped one and must never be installed (the Makefile refuses `SIFT_SB_SHIFT` without `SIFT_TESTING`). It asserts, per class, the slope of
time against size: units pruned (every unit tested, none read), `count(*)`, `sum` and a query over all partitions at most 1.15 (linear); one unit read, `ORDER BY .. LIMIT`, a point and a 51-value IN
list through the point directory (the test function of the access method), and a request of 51 keys over 400-2000 partitions at most 0.5 (flat). A query whose plan stops using sift fails with `NOSIFT`.
The projection of these slopes to 100k, 1M and 10M keys is in [perf/SCALING-PROJECTION.md](perf/SCALING-PROJECTION.md).
