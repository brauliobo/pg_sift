# Vulkan compute for aggregates (experimental, opt-in, 3.1)

`SiftAgg` can run grouped and filtered integer counts and sums of a `sift_col` index on a Vulkan compute device. It is **off by default** in two
ways: the library is built without it unless you ask (`make VULKAN=1`), and a build that has it runs on the CPU unless `sift.gpu` says otherwise.
The CPU path is untouched; a query the device cannot take, a unit it cannot read and a device that fails all run on the CPU, with the same rows.

## Build

```sh
make VULKAN=1            # or SIFT_VULKAN=1;  make clean when you switch
VULKAN=1 scripts/pgtest.sh install
```

- Nothing is linked but `-ldl`. `libvulkan.so.1` is opened with `dlopen` the first time a backend uses the device, never in the postmaster and never
  before a query asks for it. A host without a Vulkan loader or driver runs the CPU path and says so once (NOTICE).
- The Vulkan declarations are a generated subset of the Khronos `vulkan_core.h` (`src/agg/vk_min.h`) and the shader is checked in as SPIR-V
  (`src/agg/vk_spv.h`), so a build needs no Vulkan SDK and no shader compiler. After editing `src/agg/vk.comp` (needs `glslc`) or
  `agg/vk.c`, `make gpu-shaders` regenerates both; `scripts/gen-aggvk-min.py --check` and `scripts/gen-aggvk-spv.sh --check` say when they are stale.
- Without `VULKAN=1` the settings below exist and are forced off: `SET sift.gpu = on` gives one NOTICE per session and the query runs on the CPU. It is
  never an error.
- Packages and CI build without it; no job builds with `VULKAN=1`.

## Settings

| setting | default | what |
|---|---|---|
| `sift.gpu` | `off` | `off`, `auto` (a grouped aggregate when the index has at least `sift.gpu_min_rows` rows; never an ungrouped one, never a software renderer such as lavapipe) or `on` (whenever the query is eligible, grouped or not; a software device is taken only here) |
| `sift.gpu_min_rows` | 5000000 | the threshold of `auto` (grouped aggregates) |
| `sift.gpu_device` | empty | the index (`0`, `1`, ...) or a part of the name (`3090`, `radeon`) of the device; empty: the best one (discrete before integrated before virtual before software) |
| `sift.gpu_cache_mb` | 256 | device memory per backend for the column chunks that stay on the device between queries (at least 16) |

`sift.gpu`, `sift.gpu_device` and `sift.gpu_cache_mb` are superuser settings, so `ALTER ROLE analytics SET sift.gpu = 'auto'` is how a role gets it.
`sift.gpu_min_rows` is a user setting.

## What runs on the device

A `SiftAgg` scan of a `sift_col` index (not `sift_table`) at row level with at most one group key, all of these:

- **Group key:** none, a plain `int2` / `int4` / `date` column, or a dictionary-coded text column (`varchar`, `char(n)`, `name`), with at most 64 distinct
  values in a unit (a unit with more goes to the CPU).
- **Aggregates:** `count(*)`, `count(col)` of a column with no NULL in the unit, `sum` and `avg` of an `int2` / `int4` column and of a
  `numeric(p, s)` column (`p <= 18`), at most four different summed columns. Integer sums and counts are exact: the device adds 16-bit halves in 32-bit
  words and the host adds them in 128 bits, so the result does not depend on the order of additions.
- **Filters:** at most four comparisons (`=`, `<>`, `<`, `<=`, `>`, `>=`) of an integer, date, timestamp or numeric column with a constant, the conjuncts the
  source already applies on the stored values (`Filters In Source` in EXPLAIN).
- **Units:** stored in an immutable segment, clean and all-visible to the snapshot (`sift_col_unit_exact`), no NULL and no overflowed value in the columns
  the kernel reads, and chunks the kernel decodes: constant, bit-packed (`FORBIT`) or byte (`FORBYTE`) fields, under `SCALE`, or a sorted dictionary of
  those, with an LZ4 / zstd wrapper undone on the host. `DELTA`, `RLE` and `ALP` chunks go to the CPU.

Per unit means per unit: units the device cannot take (the write buffer, a delta batch, a page with a dead or not-yet-visible row, a column with NULLs, a
chunk of another shape, a group key of more than 64 values) are read by the CPU in the same scan, and the groups merge.

**Not eligible (the CPU runs the whole query):** `float4` / `float8` aggregates (a float sum depends on the order of its additions and the CPU's order is the
contract; a result that is not bit-identical is not allowed), `min`, `max`, `count(DISTINCT)`, `FILTER`, `sum` of an `int8` column (PostgreSQL's own
aggregate), expressions, more than one key, text, `IN` and `OR` filters, grouping sets, joins, arms, top-N, search, `sift_table`.

A query that the pre-aggregates answer (a roll-up by a histogram key with no filter) never reaches the device: they are cheaper than anything.

## Residency, workers and cost

- **One device context per backend**, opened lazily by the first eligible query: about 0.2 s and about 100 MB of resident memory of the process (the driver's).
  **Use it from persistent connections only** (a pool, a long-lived session): each backend pays the context once and holds its own cache, so a connection per query pays it every time and should leave `sift.gpu` off.
- **The cache** keeps the compressed chunks a query uploaded, keyed by index, segment generation, unit and column, so a repeated or similar query moves no
  chunk over PCIe (`GPU Chunks: .. resident` in EXPLAIN ANALYZE). It is cut in 2 MB slabs evicted whole, least recently used first. A segment is immutable and a
  new generation has new keys, so a stale chunk is never read. It starts cold in every backend.
- **Parallel workers:** a scan that uses the device runs in the leader alone (workers would each open a device context). A scan is run on the device only when
  `sift.gpu` says so, so `max_parallel_workers_per_gather` still serves the CPU path.
- **Memory:** staging is 16 MB for chunks and 16 MB for results per backend, plus the cache.

## EXPLAIN

```
Custom Scan (SiftAgg)
  Filters: quantity >= 2
  Group Keys: country
  Filters In Source: 1
  GPU: units 364 / 364
  GPU Device: NVIDIA GeForce RTX 3090
  GPU Chunks: 1 resident, 1092 uploaded (31.1 MB), 2 jobs
  GPU Time: upload 1.238 ms, kernel 0.343 ms, readback 0.025 ms (wall 1.961 ms)
  Units: 364
```

(the first query of a backend, 10M rows: the device context made up most of its 234 ms).

`GPU: units n / total` counts the units the device answered. `GPU: not used (reason)` says why not (the shape, the build, the threshold, no device); a failed
device shows `(.., rerun on the CPU)`. The times are those of the device (timestamps), the wall time includes submission. Plain `EXPLAIN` does not show the line.

## Failure and safety

- **No device, no driver, an unusable device:** one NOTICE per session, the query runs on the CPU.
- **Device lost, a submission that takes longer than 10 s, an allocation that fails:** the scan is run again from its first item on the CPU, the device is not
  used again in that session, one NOTICE (never a WARNING or an ERROR), no partial result.
- **Cancel and timeout:** the wait for the device is made in 5 ms slices that call `CHECK_FOR_INTERRUPTS`; a cancelled query leaves the device in a state the next
  call settles first.
- **Resources:** the device context is released at backend exit (`on_proc_exit`); nothing is created in the postmaster. Error paths drop the staged chunks, so
  an `ERROR` between staging and running leaves nothing half uploaded in the cache.
- **Signals:** instance creation and queue submission run with signals blocked, because the driver's threads inherit the mask.
- **Same answers:** the regress test `gpu_agg` runs every eligible shape and the shapes left to the CPU with `sift.gpu` on and off and prints whether the rows
  are equal; its output is the same on a build without Vulkan, one without a device and one with a device (`VK_DRIVER_FILES=/nonexistent` simulates none).

## When it helps

Measured on the online-store table of `bench/sql/store` (8 leaves, one segment of 19 200-row units), one backend, one core on the CPU side, with pg_sift 3.1.0 built
with `VULKAN=1` on a shared host with an NVIDIA GeForce RTX 3090 (`bench/bench extras gpu`, which runs `bench/gpu/bench-pg.sh`). The CPU columns are the same build with
`sift.gpu = off`; "first" is the first query of the session (for the device: after it is open, the compressed chunks cross PCIe), "median" is the median of nine repeats
(the chunks are resident on the device). Milliseconds of `Execution Time`.

<!-- bench:gpu:begin (generated by `bench/bench extras`; do not edit by hand) -->
Vulkan aggregate path against the CPU on the store table (bench/gpu/bench-pg.sh ROWS 9), pg_sift v3.1.0 built with VULKAN=1, one backend, one core on the CPU side, NVIDIA GeForce RTX 3090, measured 2026-10-20 under the perf lock.

Queries on the table of the online-store benchmark, with one sift_col index over the columns they read: `q7` `SELECT count(*) FROM orders WHERE amount > 100 AND created_at >= '2025-12-02'`; `q8` `SELECT count(*) FROM orders WHERE quantity >= 4 AND amount < 50`; `q9` `SELECT count(*), sum(amount), sum(quantity) FROM orders WHERE amount > 20 AND quantity >= 2`; `g1` `SELECT country, count(*), sum(amount) FROM orders WHERE quantity >= 2 GROUP BY country`; `g2` `SELECT category, count(*), sum(quantity) FROM orders WHERE amount > 100 GROUP BY category`; `g3` `SELECT date_trunc('month', created_at)::date, sum(amount) FROM orders WHERE quantity >= 2 GROUP BY 1`.

| rows | query | CPU, first (ms) | CPU, median (ms) | device, first (ms) |
|---:|---|---:|---:|---:|
| 1,000,000 | `q7` | 2.19 | 1.27 | 5.18 |
| 1,000,000 | `q8` | 1.88 | 0.70 | 4.36 |
| 1,000,000 | `q9` | 4.68 | 3.62 | 4.28 |
| 1,000,000 | `g1` | 4.83 | 3.65 | 4.62 |
| 1,000,000 | `g2` | 3.54 | 2.55 | 4.24 |
| 1,000,000 | `g3` | 4.96 | 4.15 | 4.85 |
| 10,000,000 | `q7` | 19.2 | 10.4 | 26.6 |
| 10,000,000 | `q8` | 13.5 | 8.31 | 18.0 |
| 10,000,000 | `q9` | 28.1 | 24.6 | 16.2 |
| 10,000,000 | `g1` | 41.2 | 32.4 | 17.5 |
| 10,000,000 | `g2` | 25.8 | 18.9 | 18.4 |
| 10,000,000 | `g3` | 30.2 | 26.6 | 18.0 |

| rows | query | device, median, resident (ms) | resident vs CPU |
|---:|---|---:|---:|
| 1,000,000 | `q7` | 0.36 | 3.55× |
| 1,000,000 | `q8` | 0.35 | 2.03× |
| 1,000,000 | `q9` | 0.39 | 9.25× |
| 1,000,000 | `g1` | 0.40 | 9.07× |
| 1,000,000 | `g2` | 0.52 | 4.93× |
| 1,000,000 | `g3` | 0.66 | 6.28× |
| 10,000,000 | `q7` | 1.96 | 5.30× |
| 10,000,000 | `q8` | 2.30 | 3.62× |
| 10,000,000 | `q9` | 1.71 | 14.4× |
| 10,000,000 | `g1` | 1.46 | 22.3× |
| 10,000,000 | `g2` | 2.01 | 9.40× |
| 10,000,000 | `g3` | 2.06 | 12.9× |

Reproduce: `bench/bench extras gpu` (a Vulkan device and `VULKAN=1`; results: [results/gpu.tsv](results/gpu.tsv)).
<!-- bench:gpu:end -->

Reading the table (the host is shared and a single run has a few tenths of a millisecond of noise at 1M rows):

- **Resident**, the device is 2x to 9x faster at 1M rows and 3.6x to 22x at 10M rows. The filtered counts at 1M rows are the smallest gain (2x to 3.6x): the
  rest is the executor enumerating units and descriptors, not the kernel.
- **First query** (every query uploads its chunks) it ties or loses at 1M rows. At 10M it wins about 1.4x to 2.4x for `q9`, `g1`, `g2` and `g3` and loses on the
  filtered counts `q7` and `q8` (0.7x to 0.8x): the chunks cross PCIe and the CPU kernels are already fast on a count. That is why `auto` takes
  grouped aggregates only, from 5M rows (an ungrouped count or sum runs on the device with `sift.gpu = on` only), and why the cache matters more than the kernel.
- A small index, a lookup, a search and a roll-up answered from the pre-aggregates do not benefit and are not routed.
- The first query of a backend pays the device context (0.2 s).

## Code layout

`SiftAgg` talks to its backends through one small interface, `src/agg/back.h` (`init`, `begin`, `run_unit`, `stopped`, `finish`, `explain`): the CPU kernels of
`agg/agg.c` are the default, and the Vulkan backend is `src/agg/gpu.c` (settings, device context, plan cache, job queue, EXPLAIN) over the device layer
`agg/vk.c` and the chunk plans of `src/col/gpu.c`. A backend sees a narrow view of the scan, is offered every unit after the pre-aggregates and
hands back exact sums (`sift_agg_fold_sums`). Without `VULKAN=1` `agg/gpu.c` is only the settings and the NOTICE; `agg/vk.c` is empty.

## Limits

- `sift_col` indexes only (`sift_table` has no device provider); one key; integer and numeric sums; no floats, no min/max.
- Text filters (`status = 'shipped'`) are not eligible yet: they are tests on the dictionary codes, a range of codes the kernel could take.
- A segment written with the plain `FOR` / `PACK` forms (not the codec) is read by the CPU.
- The cache is per backend, so many connections multiply it; `sift.gpu_cache_mb` caps each.
- A software renderer (lavapipe) works with `sift.gpu = on` for testing and is never taken by `auto`.
