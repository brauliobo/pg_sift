# Vulkan compute for aggregates (experimental, opt-in, 3.1)

`SiftAgg` can run grouped and filtered integer counts and sums of a `sift_col` index on a Vulkan compute device. It is **off by default** twice: the library is built without it unless `make VULKAN=1`, and a build that has it runs on the CPU unless `sift.gpu` says otherwise. A query the device cannot take, a unit it cannot read and a device that fails all run on the CPU, with the same rows.

**6.0 status.** Vulkan stays a first-class opt-in feature of 6.0 and its 4 `sift.gpu*` settings are part of the 27 release GUCs (no knob is added). The 6.0 device path is an overhaul for Format Major 2 and `SiftAggV6`, summarized below; the eligibility rules and limits in the sections after it describe the 5.x backend and are replaced when the 6.0 backend is integrated. The GPU-versus-CPU numbers for 6.0 are regenerated at release (`bench/gpu/bench-pg.sh`, [benchmarks/scalability.md](benchmarks/scalability.md#vulkan-gpu-against-cpu-opt-in)); the figures marked *development host* are from the staging suite and are replaced by the integrated run.

## 6.0 Overhaul Summary

Why 5.x only tied one CPU core at 10M rows: the kernel took 0.34 ms of a 1.96 ms device wall time, and the rest was per-unit dispatch (364 submissions, 1,820 allocations), synchronous 16 MB staging, host-side decompression before the PCIe copy, one group key of at most 64 values, and a device context opened by every backend. The 6.0 backend removes each of those:

| 5.x limit | 6.0 |
|---|---|
| One unit per dispatch | Whole parts and multi-granule jobs, reduced on the device into one compact result |
| Synchronous single-buffered staging | Double-buffered transfer ring over the dedicated transfer queue and, where the device has it, resizable BAR |
| Chunks decompressed on the host | Format Major 2 encodings (constant, frame-of-reference, delta, runs, dictionary, ALP, bit-packed booleans, LZ4) decoded on the device |
| One key, at most 64 groups, integer sums | Multi-key and high-cardinality hash aggregation (dense, shared-memory, and global lock-free tiers chosen by estimated distinct count), `min`, `max`, `float8`, `NULL`s, `FILTER`, `IN`/`OR`, dictionary text predicates, and computed expressions |
| A context and cache per backend | One shared device context and VRAM chunk cache, accounted by `sift.gpu_cache_mb` |
| Leader only | Parallel workers use the shared device |
| `sift_col` only | `sift_col` and `sift_table` through the same part reader |

- **Device choice:** `sift.gpu_device = ''` ranks usable devices by type, then VRAM, then effective PCIe bandwidth (link width and speed, capped by the upstream bridge), so on a host with two identical GPUs in slots of different width the faster slot is taken.
- **Routing:** `auto` takes a query only when the cost model says the device wins (cold uploads and tiny scans stay on the CPU); `on` takes every eligible query. A software renderer is never chosen by `auto`.
- **Exactness and safety are unchanged:** integer sums are exact 128-bit, a float sum follows the CPU's order, a lost device, a timeout, or an overflowing hash table re-runs the scan on the CPU with identical rows, and a missing driver (`VK_DRIVER_FILES=/nonexistent`) is a notice, never an error. The checked-in SPIR-V and `dlopen("libvulkan.so.1")` keep the build free of the Vulkan SDK; the packages are built without Vulkan.
- **Development host, standalone staging figures (2 x GeForce RTX 3060 12 GiB on a Ryzen 9 5900XT, one slot PCIe 3.0 x16 and one PCIe 2.0 x4; shared, loaded machine):** the fast kernels (specialized for the shape of the job: columns as stored, four rows per invocation, one reduction per workgroup) stream a warm ungrouped aggregation at **271-295 GB/s**, **89-97 %** of the 304 GB/s of VRAM bandwidth measured at open (before: 18-19 GB/s). With chunks resident on the device, ungrouped scans ran **4.8-6.8x** faster than 32 CPU worker processes decoding the same stored chunks and **9.5-17.9x** faster than 4; a 100-group dense aggregation 3.0-3.3x (32 workers) and 6.2-7.2x (4); a two-key hash aggregation of about 1M groups **8.2-9.5x** warm and 4.4-5.0x cold. Computed expressions and top-100 stay at 0.8-2.4x warm and a cold first query of a simple shape still loses (0.1-0.6x), so `auto` leaves it on the CPU. Results are **0 differences** against the scalar oracle on both devices (211 aggregate runs, 372 decoded chunks, with sanitizers). These come from the standalone device suite, not from a PostgreSQL run, and are replaced by the integrated PostgreSQL run of Round 5 (Stage 7); quote only the integrated run in `docs/results/`.

## Build

```sh
make VULKAN=1            # or SIFT_VULKAN=1;  make clean when you switch
VULKAN=1 scripts/pgtest.sh install
```

- Only `-ldl` is linked. `libvulkan.so.1` is opened with `dlopen` the first time a backend uses the device, never in the postmaster. A host without a Vulkan loader or driver runs the CPU path and says so once (NOTICE).
- Vulkan declarations are a generated subset of the Khronos `vulkan_core.h` (`src/agg/vk_min.h`) and the shader is checked in as SPIR-V (`src/agg/vk_spv.h`): no Vulkan SDK or shader compiler needed. After editing `src/agg/vk/shaders/` (needs `glslc`) or `agg/vk/`, `make gpu-shaders` regenerates both; `scripts/gen-aggvk-min.py --check` and `scripts/gen-aggvk-spv.sh --check` report stale files.
- Without `VULKAN=1` the settings below exist and are forced off: `SET sift.gpu = on` gives one NOTICE per session and the query runs on the CPU, never an error. Packages and CI build without it; no job builds with `VULKAN=1`.

## Settings

| setting | default | what |
|---|---|---|
| `sift.gpu` | `off` | `off`, `auto` (a grouped aggregate when the index has at least `sift.gpu_min_rows` rows; never an ungrouped one, never a software renderer such as lavapipe) or `on` (whenever the query is eligible, grouped or not; a software device is taken only here) |
| `sift.gpu_min_rows` | 5000000 | the threshold of `auto` (grouped aggregates); a user setting |
| `sift.gpu_device` | empty | the index (`0`, `1`, ...) or a part of the name (`3090`, `radeon`) of the device; empty: the best one (discrete before integrated before virtual before software) |
| `sift.gpu_cache_mb` | 256 | device memory per backend for the column chunks that stay on the device between queries (at least 16) |

`sift.gpu`, `sift.gpu_device` and `sift.gpu_cache_mb` are superuser settings, so `ALTER ROLE analytics SET sift.gpu = 'auto'` is how a role gets it.

## What runs on the device

A `SiftAgg` scan of a `sift_col` index (not `sift_table`) at row level with at most one group key, all of these:

- **Group key:** none, a plain `int2` / `int4` / `date` column, or a dictionary-coded text column (`varchar`, `char(n)`, `name`), with at most 64 distinct values in a unit.
- **Aggregates:** `count(*)`, `count(col)` of a column with no NULL in the unit, `sum` and `avg` of an `int2` / `int4` column and of a `numeric(p, s)` column (`p <= 18`), at most four different summed columns. Integer sums and counts are exact: the device adds 16-bit halves in 32-bit words and the host adds them in 128 bits, so the result does not depend on the order of additions.
- **Filters:** at most four comparisons (`=`, `<>`, `<`, `<=`, `>`, `>=`) of an integer, date, timestamp or numeric column with a constant, the conjuncts the source already applies on the stored values (`Filters In Source` in EXPLAIN).
- **Units:** stored in an immutable segment, clean and all-visible to the snapshot (`sift_col_unit_exact`), no NULL and no overflowed value in the columns the kernel reads, and chunks the kernel decodes: constant, bit-packed (`FORBIT`) or byte (`FORBYTE`) fields, under `SCALE`, or a sorted dictionary of those, with an LZ4 / zstd wrapper undone on the host. `DELTA`, `RLE` and `ALP` chunks go to the CPU; so does a segment written with the plain `FOR` / `PACK` forms (not the codec).

Eligibility is per unit: units the device cannot take (the write buffer, a delta batch, a page with a dead or not-yet-visible row, a column with NULLs, a chunk of another shape, a group key of more than 64 values) are read by the CPU in the same scan, and the groups merge. A query the pre-aggregates answer (a roll-up by a histogram key with no filter) never reaches the device.

**Not eligible (the CPU runs the whole query):** `float4` / `float8` aggregates (a float sum depends on the order of its additions and the CPU's order is the contract: results must be bit-identical), `min`, `max`, `count(DISTINCT)`, `FILTER`, `sum` of an `int8` column (PostgreSQL's own aggregate), expressions, more than one key, text (including text filters such as `status = 'shipped'`, which could become a range of dictionary codes), `IN` and `OR` filters, grouping sets, joins, arms, top-N, search, `sift_table`.

## Residency, workers and cost

- **One device context per backend**, opened lazily by the first eligible query: about 0.2 s and about 100 MB of resident memory (the driver's). Use it from persistent connections only (a pool, a long-lived session); a connection per query pays it every time and should leave `sift.gpu` off.
- **Cache.** Compressed chunks a query uploaded are kept, keyed by index, segment generation, unit and column, so a repeated or similar query moves no chunk over PCIe (`GPU Chunks: .. resident` in EXPLAIN ANALYZE). 2 MB slabs, evicted whole, least recently used first; cold in every backend, so many connections multiply it (`sift.gpu_cache_mb` caps each). A segment is immutable and a new generation has new keys, so a stale chunk is never read.
- **Parallel workers:** a scan that uses the device runs in the leader alone (workers would each open a context). A scan runs on the device only when `sift.gpu` says so, so `max_parallel_workers_per_gather` still serves the CPU path.
- **Memory:** staging is 16 MB for chunks and 16 MB for results per backend, plus the cache.

## EXPLAIN

```
Custom Scan (SiftAgg)
  Filters: quantity >= 2
  Group Keys: country
  GPU: units 364 / 364
  GPU Device: NVIDIA GeForce RTX 3090
  GPU Chunks: 1 resident, 1092 uploaded (31.1 MB), 2 jobs
  GPU Time: upload 1.238 ms, kernel 0.343 ms, readback 0.025 ms (wall 1.961 ms)
  Units: 364 read
```

(the first query of a backend, 10M rows: the device context made up most of its 234 ms).

`GPU: units n / total` counts the units the device answered. `GPU: not used (reason)` gives the reason (the shape, the build, the threshold, no device); a failed device shows `(.., rerun on the CPU)`. Times are device timestamps; the wall time includes submission. Plain `EXPLAIN` does not show the line.

## Failure and safety

- **No device, no driver, an unusable device:** one NOTICE per session, the query runs on the CPU; a device that failed to open is not looked for again for 30 s, so a host with no driver loads no library per query.
- **Device lost, a submission longer than 10 s, a failed allocation:** the scan is run again from its first item on the CPU, one NOTICE (never a WARNING or an ERROR), no partial result; the next query closes the device and opens it again (the worker and the backend that has a device of its own), and runs on the CPU for 30 s when that fails.
- **Cancel and timeout:** the wait for the device is made in 5 ms slices that ask whether the query was cancelled; the run that is cancelled frees what it holds and returns to its caller, which raises the cancel, and the submissions it left are settled by the next call.
- **Float sums:** a sum or average of floats that the device reports as an infinity or a NaN runs again on the CPU, which raises `value out of range: overflow` for finite values that overflow and returns the infinity a table holds.
- **Tiered parts:** a part in object storage is read on the CPU; the parts of the table that are local stay on the device.
- **Resources:** the device context is released at backend exit (`on_proc_exit`); nothing is created in the postmaster. Error paths drop the staged chunks, so an `ERROR` between staging and running leaves nothing half uploaded in the cache.
- **Signals:** instance creation and queue submission run with signals blocked, because the driver's threads inherit the mask.
- **Same answers:** the regress test `gpu_agg` runs every eligible shape and the shapes left to the CPU with `sift.gpu` on and off and prints whether the rows are equal; its output is the same without Vulkan, without a device and with a device (`VK_DRIVER_FILES=/nonexistent` simulates none).

## When it helps

The measurements below come from earlier releases and are kept until the 6.0 run replaces them.

The CPU columns are the same build with `sift.gpu = off`; "first" is the first query of the session (device: after it is open, the compressed chunks cross PCIe), "median" is the median of nine repeats (chunks resident on the device). Milliseconds of `Execution Time`.

Reading the table:

- **Resident**, the device ties the CPU: 0.77x to 1.07x at 1M rows and 0.99x to 1.00x at 10M rows. The CPU kernels already answer these counts and sums in a few milliseconds; the rest is the executor enumerating units and descriptors, not the kernel.
- **First query** (every query uploads its chunks) it ties the CPU at both sizes: the chunks cross PCIe on top of a CPU path that is already fast. `auto` takes grouped aggregates only, from 5M rows (an ungrouped count or sum runs on the device with `sift.gpu = on` only); no query of this table gains from it today.
- A small index, a lookup, a search and a roll-up answered from the pre-aggregates do not benefit and are not routed. The first query of a backend pays the device context (0.2 s).

## Code layout

`SiftAggV6` asks for the device once, before it scans: `sift_gpu_v6_agg` (`src/agg/gpu_v6.c`) gives back the groups the device made of the parts it could read, or NULL when the CPU runs everything, and the CPU scans the rest into the same groups. The settings are in `src/agg/gpu_cfg.h`, the device layer is `agg/vk/` and the chunk plans are in `src/col/gpu.c`. The persistent worker and the connection of a backend to it are `src/agg/gpu_worker.c`. A backend sees a narrow view of the scan and hands back exact sums. Without `VULKAN=1` the path is only the settings and the NOTICE.

## Limits

- `sift_col` indexes only (`sift_table` has no device provider); one key; integer and numeric sums; no floats, no min/max.
- The cache is per backend.
- A software renderer (lavapipe) works with `sift.gpu = on` for testing and is never taken by `auto`.
