# Column compression: encodings, selection and cost

Research behind `src/sift_colcodec.[ch]`, the chunk codec of the column engine (`sift_col`, the index over a heap table, and `sift_table`, the table
access method). The interface for the engine is `docs/COL-CODEC.md`. Labels: **MEASURED** (the harness `tools/colcodec/research.c`, numbers
reproducible with `make -C tools/colcodec research`), **SOURCE** (a URL), **ESTIMATE** (reasoned, not measured), **UNVERIFIED** (a source that
could not be read in full).

## 1. Result in one page

- A chunk (the values of one column for 4 k to 74 k rows) is compressed in two steps. **Lightweight encodings** (constant, frame of reference with
  bit-packing, delta, run-length, dictionary, decimal floats) keep every value O(1) addressable, decode at billions of values per second and let
  filters run on the encoded form. A **general-purpose layer** (LZ4, zstd) is then applied per stream, only where it pays under an explicit decode
  cost rule (section 5.4).
- **Selection is exact, not sampled**, for the lightweight step: one statistics pass per chunk gives the exact size of every candidate, children of
  a cascade (run values and lengths, dictionary, decimal digits) are chosen by the same procedure one level down (depth 3, as BtrBlocks). Only the
  general-purpose layer is trial compressed, on at most 16 KB samples when a stream is larger.
- **MEASURED, order items table (1.25 M rows, 17 B/row plain in 7 columns): 3.68 B/row with the lightweight cascade, 3.31 B/row when zstd may also
  be layered everywhere; plain bit-packing is 7.38 B/row, plain + zstd 4.28 B/row.** Selection per column beats any single scheme: the smallest
  scheme changes from column to column (delta for adjacent ids, run-length for values repeated per parent row, bit-packing for dense codes) and with
  the physical order (`store_id`: 781 KB bit-packed in heap order, 21.7 KB as runs when the table is clustered by store).
- Decimal floats (prices in 0.5 or 0.01 steps, coordinates with 6 decimals) become small integers with an exact round trip (ALP-style): 2.3x to
  6.4x smaller than the raw floats and 1.3x to 2.1x smaller than zstd on the raw bytes; XOR codecs (Gorilla) are 1.9x to 4.6x larger than that on the same data.
- The general-purpose layer buys 5 % to 25 % on skewed low-cardinality codes and counters, nothing on dense or sorted ids, and costs 3x to 10x the
  decode time of a bit-packed stream (zstd 0.3 to 1.1 ns/value against 0.1 to 0.3 for unpacking). It is therefore **not applied to the streams a scan
  filters in `auto` unless it saves a quarter of the bytes (LZ4) or more**, and it is applied freely to dictionaries, strings and exception lists
  (never scanned per row) and whenever the column option asks for it.
- Format: a self-describing tree of nodes with a one-byte tag, so a new algorithm is a new tag or algorithm id; the writer never depends on the SIMD
  level; a chunk that fails any length, range or checksum test raises ERROR.

## 2. Data and method

**Tables (MEASURED setup).** `tools/colcodec/gen.h` generates an e-commerce orders table (400 000 rows, 156 MB in native widths) and its flattened
order-items table (1 249 413 rows, 62 MB): ever-growing `id` and `updated_at` (microseconds, small jitter), `store_id` with a squared-uniform
popularity over 40 000 stores, `channel_id` over 30 values, an `order_date` (days) with a square-root skew, 12 locales, 60 countries and 6 customer
kinds as dictionary text, `cancelled` (2 %) and `paid` (70 %) flags, `n_items` 0..8 and the cumulative item offsets of the nested array, a nullable
`discount` in 0.1 steps (12 % NULL), `total` with 2 decimals, `lat`/`lon` with 6 decimals, a full-entropy double (`noise`), a store name, a url, a short
note and a free-text description (Zipf-sampled pseudo-words: ratios of the text columns are indicative only). Items: `order_id` repeated 0..8 times,
`item_idx`, a 15-value category (squared-uniform skew), a variant (8, uniform), a 3-value state, `price` in 0.5 steps (float4 and float8) and as x2
integers, and the denormalized `store_id` and `order_date`. Two physical orders: **heap** (insertion order: ids adjacent, everything else random)
and **clustered** by (`store_id`, `order_date`). Chunks of 16 384 rows unless stated (4 096 and 65 536 also run). NULL slots take the previous value
(the writer does the same) so runs and deltas survive.

**What is measured.** For every chunk the byte size of: plain native width; plain + LZ4 / zstd; each lightweight encoding forced at the top
level (children chosen freely); the smallest; the smallest + {LZ4, LZ4HC 9, zstd 1, 3, 9}; the same with byte-aligned widths + zstd; and
`autosim`, the smallest of everything per chunk (an upper bound on what a per-chunk chooser with free general-purpose layering can do). Encoders are the
real ones (vertical 32-lane bit-packing, cascades of depth 2), not formulas; decoders round-trip every chunk. Speeds are CPU time
(`CLOCK_PROCESS_CPUTIME_ID`), best of three runs of at least 40 ms, on a Ryzen 7 9700X (Zen 5) at a load average of 20 to 25 on 16 threads (treat
as lower bounds), GCC `-O2 -march=x86-64` (the scalar baseline, which GCC partly auto-vectorizes with SSE2). The research decoders are scalar
C; the shipped SIMD kernels are measured in section 7.

## 3. State of the art (SOURCE)

- **BtrBlocks** (TUM, SIGMOD 2023): per 64 000-value block, a cascade of lightweight schemes (RLE, One Value, Dictionary, Frequency, FastPFOR /
  FastBP128, FSST, Roaring, Pseudodecimal), recursion depth 3 by default. Selection: one statistics pass (min, max, unique count, average run
  length) excludes schemes (RLE below average run 2, Frequency at 50 % unique, Pseudodecimal above 50 % exceptions), then trial compression of the
  remaining schemes on a sample of 10 runs of 64 values (1 %). Combined compression 11.3x on Public BI against Parquet 7.1x and Parquet+zstd 17.1x, at
  2.6x to 3.8x the decode speed of Parquet(+Snappy/zstd). It refrains from general-purpose compression on top because it is "quite inefficient to
  decompress". https://www.cs.cit.tum.de/fileadmin/w00cfj/dis/papers/btrblocks.pdf
- **ALP** (CWI, SIGMOD 2024): doubles as `d = round(v * 10^e * 10^-f)`, decoded `d * 10^f * 10^-e`, `(e, f)` chosen per 1 024-vector from a two-level
  sample (8 vectors x 32 values, top 5 pairs, then 32 values per vector), exceptions stored raw with positions, the first good `d` standing in so the
  bit width is unaffected; digits are FOR bit-packed. Decode 2.6 tuples/cycle against Gorilla 0.047, Chimp 0.039, Patas 0.157, Elf 0.012, zstd 0.101.
  `ALP_rd` (split front bits dictionary + packed back bits) for real doubles. https://ir.cwi.nl/pub/33334/33334.pdf
- **FastLanes** (CWI, VLDB 2023): bit-unpacking over a virtual 1 024-bit register in an interleaved ("transposed") layout, so any SIMD width does the
  same work; 40+ values per cycle on four CPU families in scalar code the compiler auto-vectorizes (70 on AVX-512); delta runs on lanes with
  independent chains. https://www.vldb.org/pvldb/vol16/p2132-afroozeh.pdf
- **FSST** (CWI, VLDB 2020): static table of up to 255 symbols of 1 to 8 bytes; about 2x on text, decode 1 to 3 GB/s, encode about 1 GB/s with AVX-512,
  random access per string, equality on compressed strings; beats LZ4 on small strings. https://www.vldb.org/pvldb/vol13/p2649-boncz.pdf
- **DuckDB**: each compression function exposes analyze / final_analyze / compress / scan / fetch_row; the checkpointer feeds every vector of a segment to
  every candidate and keeps the lowest `final_analyze` score (ideally the exact byte size), with no hard-coded penalties; constant and uncompressed
  compete like the rest. Bit-packing chooses per 2 048-value group CONSTANT, CONSTANT_DELTA, FOR or DELTA_FOR (delta only when the delta width is
  smaller); `fetch_row` is O(1) except for DELTA_FOR. No sampling and no general-purpose layer by design (large blocks needed, single-row fetches
  would decompress megabytes). FSST is only selected at a ratio of 1.2 or better. https://duckdb.org/2022/10/28/lightweight-compression.html,
  https://raw.githubusercontent.com/duckdb/duckdb/main/src/storage/table/column_data_checkpointer.cpp,
  https://raw.githubusercontent.com/duckdb/duckdb/main/src/storage/compression/bitpacking.cpp
- **ClickHouse**: default LZ4 (self-managed) or ZSTD (Cloud); LZ4HC levels 1 to 12 (default 9); ZSTD 1 to 22 (default 1, "rarely justifies" going past 3).
  Specialised codecs are preparation steps chained with a general-purpose one: Delta, DoubleDelta, Gorilla, FPC, T64 (64x64 bit transpose that drops
  unused high bits), GCD, ALP (beta). Their own experiments: Delta+ZSTD or DoubleDelta cut a date column from 2.24 GiB to about 24 MiB; Gorilla and FPC
  increased size on floats where plain ZSTD won. LowCardinality is advised below about 10 000 distinct values. An experimental adaptive codec picks
  per block the smallest of the default codec, NONE and type-specific chains and never emits a block larger than the default.
  https://clickhouse.com/docs/reference/statements/create/table/codec,
  https://clickhouse.com/docs/data-compression/compression-in-clickhouse,
  https://clickhouse.com/blog/optimize-clickhouse-codecs-compression-schema
- **Parquet / Arrow**: PLAIN, RLE_DICTIONARY (one dictionary per column chunk, indexes as the RLE/bit-packing hybrid), DELTA_BINARY_PACKED (blocks of
  128, miniblocks of 32, one width per miniblock), DELTA_LENGTH_BYTE_ARRAY, DELTA_BYTE_ARRAY (front coding), BYTE_STREAM_SPLIT (does not shrink, helps the
  next compressor). Writers try the dictionary first and fall back when it exceeds 1 MB (or, on the first page, when dictionary + indexes are not
  smaller than the raw data). https://parquet.apache.org/docs/file-format/data-pages/encodings/ and the parquet-java `DictionaryValuesWriter` /
  `FallbackValuesWriter`. **UNVERIFIED**: Arrow's own default dictionary size limit.
- **Vertica, Redshift**: Redshift ENCODE AUTO; AZ64 for integers/dates (AWS claims 5 to 10 % smaller than ZSTD and 70 % faster; independent tests
  disagree, https://www.hydrogen18.com/blog/redshift-az64-performance-vs-zstd.html), BYTEDICT (256 values per 1 MB block), DELTA, MOSTLY8/16/32
  (outliers raw), RUNLENGTH. https://docs.aws.amazon.com/redshift/latest/dg/c_Compression_encodings.html. Vertica's encoding list (BLOCK_DICT,
  COMMONDELTA_COMP, DELTARANGE_COMP, GCDDELTA, RLE) is **UNVERIFIED** (page not readable).
- **Roaring / Elias-Fano / front coding**: Roaring picks per 65 536 range an array (up to 4 096 values), bitset (8 KB) or run container so a value costs
  at most 16 bits and containers stay addressable (https://github.com/RoaringBitmap/RoaringFormatSpec); Elias-Fano needs at most `2 + log2(u/n)` bits per
  sorted element with select-based access (https://arxiv.org/pdf/1206.4300); front-coded string dictionaries in buckets reach about 5 % of the text with
  microsecond lookups (https://users.dcc.uchile.cl/~gnavarro/ps/is15.2.pdf).
- **PostgreSQL columnar systems.** Citus columnar: chunk groups of 10 000 rows in stripes of 150 000, one codec per table (`none`, `pglz`, `lz4`,
  `lz4hc`, `zstd` with levels, default zstd), min/max per chunk group, no type-specific encodings. TimescaleDB: delta-of-delta + Simple-8b with RLE for
  integers and timestamps, Gorilla for floats, dictionary (indexes Simple-8b+RLE) for low cardinality, an LZ array for the rest; batches of 1 000
  values. pg_deltax: segments of about 30 000 rows, integers pick the smallest of Constant, FOR bit-packing and Delta-Varint, floats Gorilla,
  timestamps delta-of-delta, text a dictionary under 50 % cardinality and 65 536 values else block LZ4, per-segment min/max/sum/bloom, writes to
  compressed partitions rejected. https://raw.githubusercontent.com/citusdata/citus/main/src/backend/columnar/README.md,
  https://github.com/timescale/timescaledb/blob/main/tsl/src/compression/README.md, https://github.com/xataio/deltax
- **General-purpose compression on top of lightweight encodings.** "Formats should not apply general-purpose block compression by default" (Zeng et
  al., VLDB 2024, https://www.vldb.org/pvldb/vol17/p148-zeng.pdf); Vortex: zstd decodes at about 1 to 2 GB/s against 100+ G values/s for FastLanes
  kernels, its default strategy is lightweight only and a "compact" strategy adds zstd (TPC-H SF10: Parquet+zstd 4 465 MB, default 2 776 MB, compact
  2 016 MB; https://vortex.dev/blog/btrblocks-compressor); LeCo finds zstd still shrinks lightweight-encoded files, more for weaker encodings
  (https://arxiv.org/pdf/2306.15374). ClickHouse is the counterexample: chains with ZSTD are its best on dates and integers. **UNVERIFIED**: no
  source measures LZ4/zstd on top of FastLanes-style packed data specifically.

## 4. Measurements

All sizes are the sum over the chunks of a column, KB (1 024 B), heap order, 16 384-row chunks. "best" is the smallest lightweight cascade; "+zstd3" is
the whole encoded chunk zstd-compressed at level 3; "autosim" is the per-chunk minimum over every variant listed in section 2.

### 4.1 Integer columns

| column | native | plain+zstd3 | best lightweight | best+zstd3 | autosim | best chosen in |
|---|---:|---:|---:|---:|---:|---|
| orders.id (adjacent) | 1562 | 1014 | **3.3** | 2.0 | 1.6 | delta |
| orders.store_id (u^2 over 40 000) | 1562 | 934 | 781 | 756 | 756 | for |
| orders.channel_id (30) | 781 | 307 | 245 | 231 | 225 | for |
| orders.order_date (1 400 days) | 1562 | 708 | 537 | 538 | 537 | for |
| orders.locale_code (12) | 391 | 188 | 196 | 163 | 163 | for |
| orders.country_code (60) | 391 | 272 | 293 | 280 | 272 | for |
| orders.cancelled (2 %) | 391 | 17 | 23 | 12 | 11.5 | rle |
| orders.paid (70 %) | 391 | 73 | 49 | 46 | 46 | for |
| orders.n_items (0..8) | 391 | 180 | 196 | 152 | 152 | for |
| orders.updated_at (int64 us) | 3125 | 1286 | 1078 | 1078 | 1078 | delta |
| orders.item_offsets (cumulative) | 1562 | 768 | 199 | 155 | 155 | delta |
| items.order_id (0..8 repeats) | 4881 | 528 | 253 | 189 | 187 | rle |
| items.item_idx (0,1,2,.. per order) | 1220 | 228 | 458 | 437 | **183** | for (aligned bytes + zstd) |
| items.category_code (15, skewed) | 1220 | 620 | 611 | 555 | 555 | for |
| items.variant_code (8, uniform) | 1220 | 549 | 458 | 460 | 458 | for |
| items.state_code (3, skewed) | 1220 | 288 | 306 | 201 | 197 | for |
| items.store_id (denormalized) | 4881 | 810 | 746 | 707 | 707 | rle |
| items.order_date (denormalized) | 4881 | 862 | 588 | 566 | 564 | rle |
| items.price_x2 (0.5 steps as int) | 2440 | 1571 | 1526 | 1371 | 1371 | for |

- A dictionary of the chunk never beat bit-packing on the integer columns here: values are dense in their range, so the codes need the same width
  (the dictionary only pays when values are sparse in a wide range, or as a child of the string dictionary).
- **PFOR (patched frame of reference) is ESTIMATED** (exceptions as a u16 position plus the bits above the packed width, best width per chunk): no gain
  on any column (`store_id` squared-uniform included: 9 % of the values exceed 15 bits, the exceptions cost more than the saved bit). Not implemented.
- In these tables every chunk of a column picked the same winner, but the winner depends on the column and the physical order: clustering by (store,
  date) turns `orders.store_id` from bit-packing (781 KB) into runs (21.7 KB) and `items.store_id` from 746 KB to 36.5 KB, while the narrow codes do not
  change (items integers: 4 947 KB heap, 4 849 KB clustered). Per-chunk choice still matters at the edges (partial chunks, merged segments, data with
  phases), and costs nothing beyond the statistics pass.
- `orders.updated_at` has a range above 2^32 (microseconds over a year) and, clustered, no small deltas: the harness stores it plain (2 234 KB); the
  shipped FORBYTE node keeps only the 6 bytes of the range (ESTIMATE 1 680 KB).

### 4.2 Floats

| column | native | plain+zstd3 | byte-split+zstd3 | Gorilla | ALP-style decimal | ALP+zstd3 |
|---|---:|---:|---:|---:|---:|---:|
| orders.discount (float4, 0.1 steps) | 1562 | 524 | 723 | 1587 | **342** | 343 |
| orders.total (float8, 2 decimals) | 3125 | 1316 | 2200 | 2832 | **831** | 828 |
| orders.lat (float8, 6 decimals) | 3125 | 2864 | 2879 | 3223 | **1368** | 1368 |
| orders.lon | 3125 | 2891 | 2863 | 3223 | **1417** | 1417 |
| orders.noise (full-entropy float8) | 3125 | 2934 | 2740 | 2837 | 3906 | 3737 |
| items.price (float4, 0.5 steps) | 4881 | 2021 | 1875 | 2864 | **1529** | 1373 |
| items.price (float8, 0.5 steps) | 9761 | 2356 | 1884 | 3315 | **1529** | 1373 |

- ALP with an exact round trip check (`(double) d / 10^e` bit-identical to the input) needs no exception list on values that came from a decimal; on
  `noise` it produces 100 % exceptions and loses to plain, so the chooser must stop early (a sample with fewer than 90 % exact values skips ALP).
- Division by the exact power of ten is correctly rounded and therefore reproduces the parsed decimal; a multiplication by `10^-e` (the published
  form) does not (73 * 0.1 is not 7.3), which costs exceptions on 0.1-step data. Division costs 1.2 ns/value on doubles in scalar code
  (`vdivpd`), 0.3 on float4.
- Gorilla (XOR with the previous value) is 3 % larger than plain on `discount` and 9 % to 41 % smaller on the others, 1.9x to 4.6x larger than ALP, with the decoder 23 to 55 ns per
  value in this harness (a careful decoder is perhaps 2 to 5 ns, ESTIMATE; ALP's paper reports 0.047 tuples/cycle for Gorilla, about 20 cycles per
  value). Rejected. Chimp, Patas, Elf and FPC are the same family (sequential bit-level state) and were not built.
- Byte-stream-split plus zstd is the only thing that helps `noise` (12 % smaller than plain, 7 % than plain+zstd); it is not implemented (section
  5.8) because full-entropy doubles are rare in the analytic columns the index targets.
- Float keys in the engine are order-preserving integers; the codec converts back to the float, so the decimal view is exact.

### 4.3 Strings (dictionary-coded in the engine, so integer codes) and string lists

The engine stores a string leaf as dictionary codes (integer columns above: `category_code` 555 KB for 1.25 M rows = 3.6 bits/row) and the dictionary
itself as bytes. For a chunk-local dictionary of the raw strings (dictionary text in the harness):

| column (distinct per 16 k chunk) | native | plain+zstd3 | lengths + zstd blob | dictionary (raw blob) | dict + zstd blob | dict + codes zstd | smallest |
|---|---:|---:|---:|---:|---:|---:|---:|
| items.category (15) | 13220 | 6212 | 1792 | 622 | 623 | 567 | 567 |
| items.state (3) | 13543 | 5503 | 776 | 309 | 310 | **204** | 204 |
| orders.locale (12) | 2344 | 1808 | 246 | 196 | 197 | 164 | 164 |
| orders.country (60) | 2344 | 1886 | 323 | 296 | 295 | 282 | 274 |
| orders.customer_kind (6) | 4412 | 1930 | 465 | 149 | 149 | 141 | 141 |
| orders.store_name (about 8 700) | 10248 | 4608 | 3290 | 7281 | 3232 | 3232 | 3230 |
| orders.url (unique) | 18751 | 3812 | **2396** | 18000 | 3110 | 3110 | 2396 |
| orders.note (unique, 20 chars) | 14770 | 6231 | **4962** | 14184 | 5443 | 5443 | 4962 |
| orders.description (unique, 100 to 200 chars) | 81215 | 27646 | **26524** | 80775 | 27100 | 27101 | 26524 |

- Front coding the sorted dictionary of a chunk saves nothing measurable once zstd runs on the blob (`dict+front` equals `dict+zstd blob` within 1 %
  on `store_name`/`url`) and breaks first-seen-order dictionaries; not implemented.
- For unique text the cheapest representation is the lengths (cascade-coded) + the concatenated blob compressed with zstd (urls 7.8x, free text 3.1x
  against native). Per-chunk dictionaries of unique strings are larger than the plain form (the dictionary is the data plus codes). FSST (SOURCE: about 2x on text, per-string random access) is the candidate for strings that must stay
  addressable without a block decode; it is reserved as an algorithm id, not built here.

### 4.4 Null bitmaps

| column | raw bitmap | + zstd3 | positions of the minority value (cascade) | run lengths | smallest |
|---|---:|---:|---:|---:|---:|
| orders.discount, 12 % NULL, random | 48.8 | 30.3 | 40.3 | 65.5 | 30.3 |

At 12 % density a plain bitmap is the floor for lightweight forms (entropy 0.53 bit/row = 26 KB). Sparse flags are better as runs or positions:
`orders.cancelled` (2 % set) is 22.7 KB as the run-length cascade against 49.1 KB bit-packed. Chunks without NULLs store nothing. The engine already stores `nullbits`; the codec offers a
bitmap node (constant, raw, positions of the minority bit) for the same reason.

### 4.5 Chunk size and order

| rows per chunk | orders total (autosim) | items total (autosim) | 7 item columns (best lightweight, B/row) |
|---:|---:|---:|---:|
| 4 096 | 48.8 MB (128.0 B/row) | 7.9 MB (6.60 B/row) | 3.71 |
| 16 384 | 46.7 MB (122.4 B/row) | 7.6 MB (6.34 B/row) | 3.68 |
| 65 536 | 45.2 MB (118.6 B/row) | 7.5 MB (6.28 B/row) | 3.68 |

Going from 16 k to 4 k rows costs 4 to 5 %, from 16 k to 64 k saves 1 to 3 %: the fixed per-chunk cost (dictionary, base, checkpoints, tail rows) is
small enough that the superblock size (about 4 k to 74 k rows depending on the row width) does not need to change for compression.

### 4.6 General-purpose compressors (MEASURED, see section 7 for the table)

The research harness (`make -C tools/colcodec research`) compresses the actual lightweight streams of each column with every algorithm (the `plain+lz4`,
`zstd1` .. `zstd9` columns of the tables above); the implemented codec's result per policy, `lz4`, `zstd` and `zstd:9` included, is in section 7.

### 4.7 Decode speed of the research decoders (scalar C, ns per value, 16 384 rows)

| column (chosen encoding) | encode (tries every candidate) | decode | decode + scalar range filter | raw array + the same filter | zstd3 unwrap | lz4 unwrap |
|---|---:|---:|---:|---:|---:|---:|
| orders.id (delta) | 21 | 0.30 | 0.73 | 0.65 | - | - |
| orders.store_id (for) | 146 | 0.31 | 0.75 | 0.67 | 1.12 | 0.03 |
| orders.order_date (for) | 124 | 0.33 | 0.76 | 0.68 | - | - |
| orders.country_code (for, 6 bits) | 92 | 0.49 | 1.12 | 0.90 | 0.52 | 0.01 |
| orders.cancelled (rle) | 45 | 0.56 | 1.19 | 0.88 | 0.10 | 0.03 |
| items.order_id (rle) | 80 | 2.05 | 2.70 | 0.90 | 0.30 | 0.04 |
| items.store_id (rle) | 136 | 1.78 | 2.22 | 0.68 | 0.40 | 0.02 |
| items.category_code (for, 4 bits) | 70 | 0.30 | 0.74 | 0.75 | 0.32 | - |
| orders.item_offsets (delta) | 86 | 0.55 | 1.10 | 0.86 | 0.38 | 0.01 |
| float4 ALP decode | 89 | 0.32 | | | | |
| float8 ALP decode | 87 to 197 | 1.1 to 1.2 | | | | |
| float Gorilla decode | 41 to 208 | 23 to 55 | | | | |

(A zstd or LZ4 "unwrap" of 0.01 means the block was stored raw because it did not compress.) Reading: unpacking costs 0.3 ns/value in scalar code, the
scalar range filter another 0.4 to 0.6 (SIMD removes most of it, section 7), zstd adds 0.3 to 1.1 ns/value, run-length expansion into values costs 1.8
to 2.0 ns/value, which is why run-length chunks must be filtered on the runs and not expanded. Encoding with every candidate is 20 to 200 ns/value; the
shipped chooser computes sizes from statistics and encodes only the winner (section 7).

## 5. Decisions

### 5.1 Encoding set

All integer-family nodes describe `n` signed 64-bit values (the engine's key domain: integers as they are, floats as order keys, dictionary codes).

| tag | name | body | value i | filter on the encoded form | when |
|---:|---|---|---|---|---|
| 1 | CONST | `i64 v` | O(1) | whole chunk or nothing | all equal |
| 2 | FORBIT | `i64 base`, `W` in 1..32, vertical bit-packed `v - base` | O(1) | SIMD unpack + range fused, no values materialized | range < 2^32 |
| 3 | FORBYTE | `i64 base, u8 B`, `B` bytes (1..8) per `v - base` | O(1) | range kernel in place on u8/u16/u32/u64 | range >= 2^32, or the aligned variant for the general-purpose layer |
| 4 | DELTA | `i64 first, i64 mind`, `W` in 0..32, an `i64` checkpoint per 1 024 values, packed `d - mind` | O(<=1024) | unpack + prefix sum + range | sorted or slowly varying (ids, timestamps) |
| 5 | RLE | `u32 nruns`, run values node, run lengths node (run starts are rebuilt when the chunk is opened) | O(log runs) | range on the run values, then bit ranges | average run length >= 2 |
| 6 | DICT | `u32 d`, sorted distinct values node, codes node (FORBIT in practice) | O(1) | value range -> code range (sorted) -> FORBIT kernel on codes | few distinct values spread in a wide range |
| 7 | ALP | `u8 e`, `u32 nexc`, `i64 dmin, dmax`, digits node, exception positions node, raw exception keys | O(log exc) | key range -> digit range -> digits node | float keys that are decimals |
| 8 | WRAP | `u8 alg`, `u32 rawlen`, `u32 clen`, compressed child node | whole child | after unwrapping | per stream, cost rule 5.4 |
| 9 | SCALE | `i64 base, i64 g` (>= 2), child node of `(v - base) / g` | child's, then one multiply-add | `[lo, hi]` -> `[ceil((lo-base)/g), floor((hi-base)/g)]` on the child | gcd of the offsets from the minimum is >= 2: timestamps of whole seconds in microseconds, amounts in cents, fixed steps |

Every node is a multiple of 8 bytes and starts 8-aligned (16-byte header: tag, one parameter byte, `u32` count, 8 bytes of fields). Children (run values, run
lengths, dictionary, codes, digits, exception positions) are nodes chosen by the same procedure, to depth 3; only leaf streams (FORBIT, FORBYTE, DELTA) are wrapped. Plus the two
non-chunk codecs the engine needs for its metadata: a **string list** (lengths node + blob, wrapped under the cold rule) for dictionaries, and a
**bitmap** (constant, raw, positions of the minority bit) for NULL and liveness bits.

**Bit-packing layout.** 32 lanes of 32 bits ("vertical" or interleaved, FastLanes-like but without the transposed tuple order): value `i` of a block of
1 024 is row `i / 32`, lane `i % 32`; lane `l` is a bit stream of its rows, stored so word `k` of lane `l` is at `32 * k + l`. Consequences:
(1) one layout for scalar, 128-, 256- and 512-bit code, so the bytes on disk do not depend on the CPU (a replica with another CPU reads the same data);
(2) unpacking is shifts, ORs and masks with compile-time constants per width, auto-vectorizable, and the output is already in row order, so filters
produce the row bitmap with no permutation; (3) value `i` is two loads (random access, needed by the heap-visibility fallback and parent-ordinal lookups); (4) the
tail (fewer than 32 rows of the last block) costs at most 31 padded values. The measured scalar unpack is 0.3 ns/value (SSE2 auto-vectorized);
SIMD numbers in section 7. Delta decoding keeps this layout (unpack, then an in-register prefix sum) instead of FastLanes' transposed order, which would
force every column of a unit into the same tuple permutation.

**Rejected or deferred.** Gorilla, Chimp, Patas, Elf, FPC, DoubleDelta (sequential bit-level decode, 20 to 50 ns/value measured, no SIMD, no filter on the encoded
form, and ALP is smaller on every decimal column); T64 (frame of reference + delta + the aligned variant cover it); PFOR (no gain, 4.1);
Elias-Fano and Roaring containers for column values (offsets/positions of sparse bitmaps use the integer cascade, which is within 10 % of
Elias-Fano on the positions measured); front-coded dictionaries (4.3); byte-stream-split and ALP_rd (rare in target columns, +6 % at most on
full-entropy doubles); FSST (reserved id, text is covered by the zstd wrapper); Frequency / top-value encoding (BtrBlocks) and DICT-of-ints beyond the sorted
variant (no gain here).

### 5.2 Selection

Per chunk, per column, `sift_cc_encode` does:

1. **Statistics**, one pass over the (NULL-filled) values: min, max, number of runs, min and max delta. The distinct values are collected (open-addressing
   set, early exit above `n / 2` or 65 536 values) only when a dictionary can beat the frame of reference; for float kinds a 64-value stride sample
   decides the decimal exponent `e` (0..18 for float8, 0..10 for float4) as the one with the most exact round trips; below 90 % exact, or with more than a
   quarter exceptions, the ALP candidate is dropped.
2. **Exact sizes** of every applicable candidate: CONST `16`; FORBIT `16 + 4 * words(n, W)`; FORBYTE `16 + n * B` (rounded to 8); DELTA
   `24 + 8 * ((n-1)/1024) + 4 * words(n, W_delta)`; RLE, DICT, ALP and SCALE build their (small) child arrays and recurse one level down (SCALE only when the
   greatest common divisor of the offsets from the minimum is at least 2, found in a few steps for most data). Cost stays near
   15 to 40 ns/value in the implementation (section 7) against 20 to 200 for trying everything with the research encoders.
3. **Smallest wins**; ties go to the cheaper decode (FORBIT over FORBYTE; DELTA, RLE, DICT and ALP only when strictly smaller).
4. **General-purpose layer**: walk the chosen tree bottom-up; for each stream (a node whose body is bytes: packed words, blobs, exception values) run the
   cost rule of 5.4; under an explicit layer (and for cold streams) the byte-aligned FORBYTE twin of a narrow bit-packed stream is tried as well, because
   zstd and LZ4 find more in bytes than in bits, and the smaller result wins.
5. The writer **never reads the SIMD level**: statistics are integer arithmetic, trial compression is by libraries whose output is a function of
   input and level only; the format is byte-for-byte identical on every CPU.

Why exact sizes instead of a sample (BtrBlocks) or analyze/finalize per vector (DuckDB, the same idea): chunks are small (at most 74 k values) and
the statistics are a single pass, so sampling would only add error; DuckDB's authors came to the same conclusion. The sample is kept only where an
exact answer is expensive: trial compression of streams above 32 KB uses four 4 KB slices.

### 5.3 Per-column control: the `compression` option

A column option (a reloption of the access method, one value for the table or `default; column=policy; ...`; parsed by `sift_cc_policy_parse` /
`sift_cc_policy_for_column`, registered by the access method):

| value | meaning |
|---|---|
| `auto` (default) | lightweight cascade, general-purpose layer under the cost rule 5.4 |
| `none` | no compression: FORBYTE at 1, 2, 4 or 8 bytes (what the engine's plain `FOR` chunk is), strings plain; maximum scan speed, the chunks are filtered in place |
| `lz4`, `lz4:N` | auto cascade, then LZ4 (HC for N >= 3) on every stream that shrinks by 3 % |
| `zstd`, `zstd:N` | auto cascade, then zstd (level N, default 3) on every stream that shrinks by 3 %; also tries the byte-aligned variants; an error on a build without the library |
| encoding names, `+`-joined, optionally followed by an algorithm: `for`, `delta`, `rle`, `dict`, `alp`, `scale`, `for+zstd:9` | restrict the cascade to these encodings (children still free); falls back to FORBYTE when none applies |

No GUC: the policy belongs to the column; `sift.simd` does not affect it. The default is on (`auto`), as the task requires; the thresholds are compile-time
constants in `sift_colcodec.h` next to the measured costs they come from.

### 5.4 Cost rule of the general-purpose layer

A stream of `R` raw bytes compresses to `C` with algorithm `a`, which unwraps at `u_a` ns per output byte. The extra decode time per scan is `R * u_a`.
`auto` accepts the wrapper when `C + 16 + rho * R * u_a < R * (1 - 0.03)` (the node header and the 3 % margin included), with `rho` the number of bytes one
nanosecond of per-scan decode must save:

| class | streams | rho | LZ4 (u=0.25) needs a saving of | zstd (u=0.7) needs |
|---|---|---:|---:|---:|
| **hot** | packed words / aligned bytes of a column's value or code stream, DELTA and RLE payloads | 1.0 | 25 % | 70 % (practically never) |
| **cold** | dictionaries, string blobs, exception values and positions, checkpoint arrays | 0.15 | 4 % | 10 % |
| explicit (`lz4`, `zstd`) | all | 0 | 3 % | 3 % |

`u` comes from MEASURED unwrap speeds (section 7: LZ4 4 to 5 GB/s, zstd 1.4 to 2 GB/s on the streams here); `rho = 1` is "a byte saved must pay 1 ns of
CPU per scan", about 1 GB/s, the order of one cold NVMe read, and 10x the cost of a DRAM read (the reason the hot rule is strict: in cache, packed words are read at
10 GB/s or more and any general-purpose decode loses). Rationale and counter-arguments: Zeng et al. and BtrBlocks (SOURCE above) advise no layer by
default; ClickHouse chains ZSTD by default because it has no SIMD-friendly lightweight layer to lean on. The measured gain on the hot streams is 9 to 23
%, which at `rho = 1` is accepted only for LZ4-compressible streams (`item_idx`'s counter pattern, 60 %). A user who prefers footprint to scan speed sets
`zstd` on the column. Whole-table effect is in section 7.

### 5.5 Format, versioning, integrity

```
blob:   u8 magic 0xC5 | u8 version (1) | u8 kind (0 int, 1 float4 key, 2 float8 key) | u8 flags | u32 n | i64 min | i64 max | root node | u32 check | u32 0
node:   u8 tag | u8 parameter (width, byte width, exponent, algorithm) | u16 0 | u32 count or raw length | 8 bytes of fields | body as in 5.1
```
- `kind` selects the key transform of ALP; `min`/`max` of the non-NULL values (also what zone maps need, so the engine does not recompute them) and the
  range < 2^32 flag tell the reader which decode domain to use (u32 offsets from `min`, else 64 bit).
- **Adding an algorithm** = a new WRAP algorithm id (or node tag); unknown ids/tags ERROR with "chunk uses an encoding this build does not know" (a
  version-skew message, not corruption). Algorithm ids: 1 LZ4, 2 zstd; 3..15 reserved (FSST, LZ4 frame, Zstd dictionary).
- `check` is `sift_term_hash` (the repository's stable 32-bit hash, an xxh64-style single-lane hash) of everything before it. It is verified when the chunk is
  opened from storage if the caller asks (`verify`); independently every decoder is **memory safe on any bytes**: lengths, widths, counts, run sums,
  code ranges, exception positions, decimal digit bounds, decompressed sizes (bounded by `n * 9 + 4 KB` per node, against decompression bombs) and the
  recursion depth are checked, and a violation is `ERRCODE_DATA_CORRUPTED`.
- Compatibility with the engine's current chunk header (`enc`, `bits`, `base`, null bitmap): the codec blob replaces `enc >= 4` payloads; `CONST`, `FOR` and
  `PACK` chunks of minor 0 stay readable (see `docs/COL-CODEC.md`).

### 5.6 Zone maps, random access, MVCC, write lane, CPU budget

- **Zone maps are never compressed and never decoded**: they live outside the chunk (the engine's ZONES arrays). The codec computes min/max/NULL
  count/constant flag and the integer sum in its statistics pass and hands them to the engine (`SiftCcInfo`), so pruning reads no chunk, and the chunk header
  carries `min`/`max` again for the filter domain. A unit pruned by its zone costs zero decode; an unpruned unit costs the filter on the encoded form.
- **Random access** (needed by the heap-visibility fallback and parent-ordinal lookups): `sift_cc_get(i)` is O(1) for CONST, FORBIT, FORBYTE and DICT;
  O(log runs) for RLE (run starts are rebuilt once when the chunk is opened); at most 1 023 additions for DELTA (a checkpoint every 1 024 values);
  O(log exceptions) for ALP. A chunk with WRAP nodes is opened once (`sift_cc_open` unwraps into its own memory context) and `get` then costs the same;
  the visibility fallback opens the chunk it needs once per unit.
- **MVCC and the write lane**: rows of the mutable lane stay uncompressed (the engine's row records) until sealed; compression happens at promotion and
  merge, where the engine already decodes and re-encodes whole superblocks into batches. Chunks are immutable; only liveness bits change after the
  write, so compression never interacts with MVCC. Dead rows are only dropped by the merge's re-encode.
- **CPU budget** (ESTIMATE, section 7.3 for the measured): the statistics pass and winner encode are about 10 ns/value, trial compression of the streams
  about 1 to 3 ns/value for LZ4 and 5 to 15 ns/value for zstd 3 on the packed streams; for a table of 10 columns, build and merge cost on the order of
  100 to 300 ns per row per core, against the heap scan and index sort that dominate a build. Merge-time re-encode may use a higher zstd level
  (`zstd:N`) at no read cost.

### 5.7 Verification plan

Round-trip randomized tests per node type and per policy on random and adversarial data (empty, all NULL, one value, minimum and maximum int64, NaN, -0.0,
infinities, huge runs, alternating, ramps, all-equal-but-one, unicode strings), a differential of the decoders and every kernel at every supported SIMD
level against the scalar tables, equivalence of the policies against the uncompressed chunk, and a corruption fuzzer (bit flips, truncations,
oversized lengths, mutated tags) that must produce ERROR and nothing else under ASan/UBSan (`SANITIZE=1`). Details: `docs/COL-CODEC.md`.

### 5.8 Not done (and why)

- Byte-stream-split for full-entropy doubles (+6 %, 6 ns/value); ALP_rd; FSST; PFOR; Gorilla family; front coding (4.1 to 4.3).
- A GUC for the level: the column option covers it.
- Cross-chunk or cross-segment dictionaries for the general-purpose layer (zstd dictionaries): would couple chunks.

## 6. What the engine gets

`docs/COL-CODEC.md` is the interface: encode a chunk (values, NULL bitmap, policy) to a blob plus the statistics the zone map needs; open, verify, decode
to the key domain or to unsigned offset arrays, filter an inclusive key range into a bit vector on the encoded form, get a value, sum under a mask; the
kernel table `SiftCcKernels` with scalar, AVX2 and AVX-512 levels dispatched through `sift_cpu_register` (`docs/KERNELS.md`).

## 7. Results of the implementation

All figures MEASURED with `make -C tools/colcodec bench` (CPU time, one core, 16 384-row chunks, heap order, library build with LZ4 and zstd) on the
generated tables of section 2 (400 k order rows with 67 B/row of fixed-width numeric columns, 1.25 M item rows with 30 B/row).

### 7.1 Size per policy (B/row, numeric columns; `ratio` = raw / policy)

| table | policy | int | f32 | f64 | numeric | ratio | strings | encode ns/value |
|---|---|---|---|---|---|---|---|---|
| orders (raw 67.00, strings 315.25) | none | 22.03 | 8.00 | 32.01 | 62.05 | 1.08x | 321.27 | 6.8 |
| | for | 14.78 | 7.00 | 30.26 | 52.04 | 1.29x | 318.40 | 4.2 |
| | auto | 9.23 | 0.88 | 16.51 | 26.63 | 2.52x | 97.54 | 12.4 |
| | lz4 | 9.23 | 0.88 | 16.51 | 26.62 | 2.52x | 203.39 | 16.0 |
| | zstd | 8.75 | 0.79 | 16.41 | 25.95 | 2.58x | 97.52 | 18.0 |
| | zstd:9 | 8.77 | 0.79 | 16.41 | 25.97 | 2.58x | 95.83 | 27.3 |
| items (raw 30.00, strings 13.93) | none | 12.02 | 8.00 | 8.00 | 28.03 | 1.07x | 15.94 | 6.7 |
| | for | 7.77 | 7.00 | 7.00 | 21.78 | 1.38x | 14.57 | 3.4 |
| | auto | 3.99 | 1.26 | 1.26 | 6.50 | 4.62x | 2.08 | 46.9 |
| | lz4 | 4.01 | 1.26 | 1.26 | 6.52 | 4.60x | 4.39 | 57.7 |
| | zstd | 3.50 | 1.13 | 1.13 | 5.76 | 5.20x | 2.06 | 68.1 |
| | zstd:9 | 3.48 | 1.13 | 1.13 | 5.74 | 5.22x | 1.76 | 118.3 |

`none` is the plain value at 1, 2, 4 or 8 bytes (about the raw size, the small gain is constants and the narrowest width), `for` is bit-packing
with a frame of reference, the ordinary lightweight baseline. `auto` is 2.3x to 4.3x smaller than `none` and 2.0x to 3.4x smaller than `for`.
The general-purpose layer on top of the lightweight cascade adds 2.5 % (orders) to 11 % (items) with zstd and nothing with LZ4 in `auto`'s
choices, which is the cost rule of section 5.4 working: a stream is only wrapped when it pays. Strings (dictionary codes plus the dictionary
bytes) gain from either layer because the dictionary is wrapped freely: 321 to 97.5 B/row (orders), 15.9 to 2.1 B/row (items).

Per column under `auto` (B/row, raw in brackets): orders `id` 0.01 (4), `store_id` 2.00 (4), `order_date` 1.38 (4), `discount` 0.88 (4), `total`
2.13 (8), `lat`/`lon` 3.51/3.63 (8; six decimals, 2.2x to 2.3x), `noise` 7.25 (8; random, nothing to gain), `updated_at` 2.76 (8), `cancelled` 0.05 (1);
items `order_id` 0.16 (4), `price` 1.26 (4), `price8` 1.26 (8), `store_id` 0.60 (4), `order_date` 0.47 (4).

Shared orders corpus used by the other column branches (400 k rows, 16 384-row chunks), B/row, `none` / `for` / `auto` / `zstd`:

| column | none | for | auto | zstd |
|---|---|---|---|---|
| id | 2.00 | 1.75 | 0.010 | 0.008 |
| store_id | 2.00 | 2.00 | 2.00 | 1.95 |
| customer_id | 4.00 | 2.38 | 2.38 | 2.38 |
| placed_at | 2.00 | 1.38 | 1.38 | 1.38 |
| created_at / updated_at (microseconds) | 8.00 | 6.00 | 3.38 | 3.38 |
| total | 8.00 | 8.00 | 1.82 | 1.64 |
| discount | 8.00 | 8.00 | 1.42 | 0.98 |
| 7 code columns (status, currency, source, country, lang, shipping, payment) | 7.00 | 3.01 | 3.01 | 2.76 |
| **all 15 columns** | **49.05** | **38.54** | **18.79** | **17.86** |

`auto` is 0.38 of `none` and 0.49 of `for`. The microsecond timestamps went from 6.00 (bit-packed) to 3.38 B/row with the common-divisor node
(`SCALE`): values that are whole seconds or milliseconds divide by the gcd of their offsets, and the quotients bit-pack or delta.

Against the private prototype figures on its own data (plain 60 MB, bit-packed about 35 MB, a tuned columnar server 39.3 MB), bit-packed is 0.58 of plain
and the tuned server 0.655 of plain; here `for` is 0.79 of `none` on the shared corpus and `auto` 0.38. The data differ, so only the ratios are
comparable, not the byte counts.

### 7.2 Speed per SIMD level (items table, ns/value, CPU time)

`open` unwraps and validates the chunk once; `decode` and `filter` run on the opened chunk (filter selects 10 % of the range); `get` is one random access.

| policy | level | open | decode | filter | get (ns) |
|---|---|---|---|---|---|
| none | scalar | 0.001 | 0.324 | 0.441 | 5.6 |
| | avx2 | 0.001 | 0.334 | 0.081 | 5.3 |
| | avx512 | 0.001 | 0.337 | 0.091 | 5.3 |
| auto | scalar | 0.066 | 1.460 | 0.740 | 60.5 |
| | avx2 | 0.066 | 0.879 | 0.224 | 84.3 |
| | avx512 | 0.066 | 0.710 | 0.152 | 66.8 |
| lz4 | scalar | 0.040 | 1.446 | 0.674 | 91.3 |
| | avx2 | 0.040 | 0.849 | 0.203 | 73.5 |
| | avx512 | 0.040 | 0.784 | 0.134 | 72.3 |
| zstd | scalar | 0.457 | 1.271 | 0.591 | 105.4 |
| | avx2 | 0.457 | 0.933 | 0.197 | 71.1 |
| | avx512 | 0.457 | 0.777 | 0.128 | 71.2 |

- `none` decodes at memory speed (a widening copy, 0.32 ns/value at every level) and filters 5.4x faster with AVX2 than scalar.
- `auto` decodes the whole mixed table (all column kinds, cascades included) at 0.71 to 1.46 ns/value, filters 3.3x (AVX2) to 4.9x (AVX-512) faster
  than scalar, and keeps `get` at 60 to 85 ns including delta checkpoints and run searches (the `get` figure is noisy and not SIMD dependent).
- zstd costs 0.46 ns/value at open (decompressing every wrapped stream once), LZ4 0.04, `auto` 0.07; after open all policies decode alike.
- The bytes written, the decode and the filter result are identical at the three levels (`colcodec_simd`, `tools/colcodec` test).

### 7.3 Build cost

Encode takes 3.4 to 12.4 ns/value for `none`, `for` and `auto` on the orders table and 47 ns/value for `auto` on the item table (many small
cascaded streams per chunk); trial compression with zstd raises it to 18 and 68, and zstd:9 to 27 and 118. In the CPU budget of section 5.6 (ESTIMATE 10 ns/value
for the statistics pass and winner) the measured cost is in range for the orders table and about 5x above it for the item columns; a
10-column table of that kind costs about 0.5 microseconds per row per core. Merge-time re-encode at `zstd:9` costs about 1.2 microseconds per row for the same table.
