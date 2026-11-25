# Scalability and parallelism

Parallel guided-morsel scans, aggregation, build and compaction run on PostgreSQL's parallel workers (`sift.parallel_scan = on`); cold scans stream extents through the server's `io_method`. Summary: [README](../../README.md#benchmarks). Protocol: [BENCHMARKS.md](../BENCHMARKS.md).

| Axis | Ladder | Command |
|---|---|---|
| Row scale | `100k`, `1M`, `5M` (`10M` release tag) | `bench/bench store all --docs` |
| Workers | `1`, `2`, `4`, `8` | `bench/bench scaling-io --suites A --docs` |
| Cold I/O | `io_uring`, `worker`, `sync`; read stream on | `bench/bench scaling-io --suites B --docs` |
| Writers | `1`, `4`, `16` clients over 64 hash partitions (60% `INSERT`, 30% `UPDATE`, 10% `DELETE`) | `bench/bench scaling-io --suites C --docs` |
| Partitions | `1`, `16`, `256`, `4096` | `bench/bench scaling --docs --partitions 1,16,256,4096` |
| GPU (opt-in) | Vulkan against 4 and 32 CPU workers | `bench/gpu/pg.sh ROWS` |

## Workers and cold I/O

Release gates of `bench/bench scaling-io --docs` (`docs/results/v6-scaling.tsv`): warm probes against 1 worker, cold probes against `sync` without a read stream. A gate on fewer than 8 workers or 16 clients is scaled by workers / 8. A cold run whose reads are under 90% of its blocks is marked `notcold`.

<!-- bench:v6-gates:begin -->
*Not measured for 6.0 yet: `bench/bench scaling-io --docs` writes `docs/results/v6-scaling.tsv` and populates this block.*
<!-- bench:v6-gates:end -->

<!-- bench:v6-scaling:begin -->
*Not measured for 6.0 yet: `bench/bench scaling-io --docs` writes `docs/results/v6-scaling.tsv` and populates this block.*
<!-- bench:v6-scaling:end -->

## Vulkan GPU against CPU (opt-in)

`sift.gpu = on` (build with `make VULKAN=1`, [GPU.md](../GPU.md)) runs eligible integer aggregates of a `sift_col` index on a Vulkan device. `bench/gpu/pg.sh ROWS` times each query with `sift.gpu = off` (one CPU core) and `sift.gpu = on` (first query, then the median of the rest with chunks resident on the device) on the `store` orders table.

**Staging preview (standalone device suite, not PostgreSQL; the integrated run replaces it).** Two GeForce RTX 3060 12 GiB on a shared Ryzen 9 5900XT host, 10M and 50M rows, warm: decoded columns resident in device memory, each CPU figure from the same run.

| Shape (warm) | Device kernel | Against 32 CPU workers | Against 4 CPU workers |
|---|---|---|---|
| Ungrouped, `WHERE` on one column | **271-295 GB/s** (89-97 % of the 304 GB/s peak) | 4.8-6.8x | 9.5-17.9x |
| Dense, 100 groups | 80 GB/s | 3.0-3.3x | 6.2-7.2x |
| Hash, about 1M groups, 2 keys | 3.3-3.5 GB/s | **8.2-9.5x** | 9.3-9.5x |
| Expression, top-100 | unchanged | 0.8-1.1x | 1.2-2.4x |

Cold first queries of the simple shapes lose to the CPU (0.1-0.6x) and stay on it under `sift.gpu = auto`. Parity: **0 differences** against the scalar oracle on both devices (211 aggregate runs, 372 decoded chunks, ASan and UBSan clean). The loaded host disturbs every CPU figure; repeat on an idle host before quoting.

*Not measured for 6.0 yet: `bench/gpu/pg.sh 1000000` and `10000000` populate the integrated run at release.*
