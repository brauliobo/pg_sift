# Options: the case for each default and opt-in

Every `WITH (...)` option of the index kinds and every `sift.*` setting, with its class and the measured case behind it. The table below lists the name, type, default and range of the C sources. The reference tables
with range, context and description are in [USAGE.md](USAGE.md#maintenance-and-operations); the one-line version is in the README ("Options at a glance").

A **default** is automatic behaviour: the build or the planner decides and a NOTICE at `CREATE INDEX` says what and how to opt out. It is on when it passes the default formula below,
for the columns where a statistic of the table (correlation, cardinality, type, row count) says it pays, not for every column. An **opt-in** failed a term of the formula (or changes results, needs hardware,
takes memory, or has not run on real workloads) and is kept for the workloads where the win is clear; its `why` names the term, and its gain, growth and write cost are the measured price.
A cell that reads **not measured** is a measurement the owning work still owes:

## The default formula

Each structure is measured in instructions of one backend (`perf stat -e instructions:u`, a table with the shape the structure is for):

| Term | Meaning |
|---|---|
| S | speedup of the relevant query class: instructions without the structure over instructions with it; it must save at least 100k instructions a query |
| G | growth of the index in %, for the structure alone |
| W | write cost in %: instructions of the build, the merge or the promotion |
| R | relevance: 1 for a class most workloads run, 0.5 for a niche one |

The benefit is `B = R * log2(S) / log2(1.05)` (one point for each 5% of speedup) and the cost `C = G / 3 + W / 5` (one point for each 3% of growth and each 5% of write cost).
A structure is automatic when `S >= 1.05`, `B >= C` and `G <= 25%` for any single structure; otherwise it is an opt-in whose `why` names what it failed: `gain` (S under 1.05 or under 100k instructions saved),
`cost` (B under C), `ceiling` (G over 25%) or `detect` (no statistic of the table says the columns it pays on, so it cannot be automatic). `result`, `hardware`, `memory` and `risk` mark the reasons the formula does not cover.
The terms come from the `s`, `save`, `g`, `w` and `r` cells of the record of each option, which is invalid when an opt-in passes the formula, when its `why` misses a term it fails, or when a default fails it
(`Formula waived: reason` in `default_why` is the one way to keep such a default).

Every option and every automatic heuristic (an `auto(...)` row, which has no setting of its own) documents seven things, and none may be empty: what it does,
the measured gain (class of queries, numbers, bench case), the impact (index size, build, merge and write cost, plus planning time, memory and WAL where they apply), when it applies and when it does not,
the default and why, how to turn it off (`n/a: reason` for a limit or guard that has no off switch) and the upgrade note with the column format minor it writes. `-` or `n/a: reason` is accepted where there is nothing to say; empty and `tbd` are not.

Gain, growth and write cost carry their conditions (rows, columns, version); `-` means the option has no such effect. PR is the pull request that introduced or last changed the behaviour;
Bench is a case of `bench/cases.tsv` or a file of stored results.

<!-- sift-cases:start -->
| Class | Entries | Meaning |
|---|---|---|
| [default](#default) | 115 | on by default: automatic behaviour, or a knob whose default is the recommended value |
| [opt-in](#opt-in) | 17 | off or empty by default; `why` names the term of the default formula it failed (or why the formula does not cover it) |
| [diagnostic](#diagnostic) | 13 | to measure, compare or explain a plan; not for a deployed configuration |
| [safety](#safety) | 18 | a limit or guard on memory, locks, I/O or replicas |
| [deprecated](#deprecated) | 2 | an older spelling that is still accepted |

### default

#### sift(k1)

`BM25` · default · since 1.0 · default 1.2

- **What it does:** term-frequency saturation of the BM25 score
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: REINDEX to change; no size effect
- **When:** sift (BM25) indexes; scales the score of every ranked query. A match, its rows and count(*) do not depend on it. Not an option of sift_col or sift_table.
- **Default:** 1.2 — 1.2: the value of the ranked top-10 kernel benchmarks; no size or write effect (gain, growth, write are -), so the default carries no cost.
- **Turn off:** n/a: a score parameter. ALTER INDEX .. RESET (k1) returns 1.2 but the index fails queries and inserts until REINDEX; build a new index with CREATE INDEX CONCURRENTLY and drop the old one.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 1.2. Higher lets repeated words keep raising the score, lower flattens that. Changing it needs REINDEX; the index size does not change.

#### sift(b)

`BM25` · default · since 1.0 · default 0.75

- **What it does:** length normalization of the BM25 score
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: REINDEX to change; no size effect
- **When:** sift (BM25) indexes; scales the length normalization of every ranked score. A match, its rows and count(*) do not depend on it. Not an option of sift_col or sift_table.
- **Default:** 0.75 — 0.75: the value of the ranked top-10 kernel benchmarks; no size or write effect (gain, growth, write are -), so the default carries no cost.
- **Turn off:** n/a: a score parameter (0 ignores document length). ALTER INDEX .. RESET (b) returns 0.75 but the index fails until REINDEX; prefer CREATE INDEX CONCURRENTLY with the new value, then drop the old one.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 0.75. 0 ignores document length, 1 normalizes fully by length. Changing it needs REINDEX; the index size does not change.

#### sift(case_folding)

`BM25` · default · since 1.0 · default true

- **What it does:** matches regardless of case
- **Gain:** -
- **Impact:** index size: 0.0% (8.78 MB with and without; 100,000 documents, a quarter in capitals); build, merge and writes: build +9.0% (446 to 486 ms), insert of 20,000 documents -2.1%: within noise; other: REINDEX to change
- **When:** Every text index: terms and queries are lower-cased the same way. Turn it off only when 'Word' and 'word' must be different terms; the keyword analyzer indexes whole values.
- **Default:** true — On since 1.0 so that case does not decide a match. Measured: 0.0% size, insert -2.1% (noise), build +9.0% (446 to 486 ms, 100,000 documents).
- **Turn off:** CREATE INDEX .. WITH (case_folding = false). On an existing index ALTER INDEX .. SET (case_folding = false) then REINDEX; queries and inserts fail until then. Prefer CREATE INDEX CONCURRENTLY and swap.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Turn it off only when 'Word' and 'word' must be different terms. Needs REINDEX.

#### sift(accent_folding)

`BM25` · default · since 1.0 · default true

- **What it does:** matches regardless of accents
- **Gain:** -
- **Impact:** index size: 0.0% (8.78 MB with and without; 100,000 documents, a fifth with an accent); build, merge and writes: build -3.8%, insert of 20,000 documents -5.6%: within noise; other: REINDEX to change
- **When:** Every text index: accents are stripped from terms and queries (resume = resume). Turn it off only when accented and plain letters must differ.
- **Default:** true — On since 1.0 so that accents do not decide a match. Measured: 0.0% size, build -3.8% and insert -5.6% (noise, 100,000 documents).
- **Turn off:** CREATE INDEX .. WITH (accent_folding = false). On an existing index ALTER INDEX .. SET (accent_folding = false) then REINDEX; queries and inserts fail until then. Prefer CREATE INDEX CONCURRENTLY and swap.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Turn it off only when accented and plain letters must be different terms. Needs REINDEX.

#### sift(positions)

`BM25` · default · since 1.2 · default true

- **What it does:** phrase queries are decided by the index, not rechecked in the heap
- **Gain:** phrase queries 4.1x and phrase counts 7.3x faster than positions = off (218,058 documents); word queries unchanged
- **Impact:** index size: +149% of the index (52.5 to 130.9 MiB, 218,058 documents); v3.1: +44% (1M short rows), +138% (50,000 documents); build, merge and writes: build about 30% slower (1.7 to 2.2 s, 218,058 documents); v3.1 build CPU +13% (1M short rows), +38% (50,000 documents); other: index about 2.5x the size of positions=false (130.9 against 52.5 MiB on 218,058 articles); REINDEX to change
- **When:** Phrase queries ("two words"). Word, AND, OR queries are unchanged. Phrases are rechecked in the heap for rows without positions: positions = off, rows inserted since an old build, phrases under NOT.
- **Default:** true — On: phrases 4.1x and phrase counts 7.3x faster (>= 5% on a relevant class) although the index is 2.5x larger and the build about 30% slower, over the 3% and 5% limits; on since 1.2.
- **Turn off:** CREATE INDEX .. WITH (positions = off): smaller index, phrases rechecked in the heap. Existing index: ALTER INDEX .. SET (positions = off) then REINDEX; units written before keep their positions until then.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on so phrases are decided by the index. Set positions = off when phrases are rare and index size matters; phrases are then rechecked in the heap. Needs REINDEX.

#### sift(write_buffer_limit)

`BM25` · default · since 3.0 · default 4096

- **What it does:** size at which the write buffer is sealed; the base of the merge tiers
- **Gain:** 1024 against 4096 kB (100,000 documents): top-10 OR 12.5 to 4.1 ms, AND 6.1 to 2.6 ms; promotion 37 to 13 ms but 3.7x as often
- **Impact:** index size: +37% of the index (72 to 98 MB) and 17 to 78 segments at 1024 against 4096 kB (100,000 documents); build, merge and writes: 16-client INSERT 107k to 87k rows/s, COPY 261k to 137k rows/s at 1024 against 4096 kB; other: 1024 against 4096 kB (100,000 documents): promotion 37 to 13 ms but 3.7x as often, 16-client INSERT 107k to 87k rows/s, index 72 to 98 MB
- **When:** sift indexes: size at which the open write buffer is sealed; it is also where the merge tiers start. sift_col has the same option; a sift_table uses sift.table_write_buffer_limit.
- **Default:** 4096 — 4096 kB: 1024 made queries on the grown index faster (top-10 OR 12.5 to 4.1 ms) but the index 37% larger and 16-client INSERT -18%, INSERT..SELECT -17%, COPY -47%, over the 3% and 5% limits.
- **Turn off:** n/a: a size, not a switch (64 to 1048576 kB). ALTER INDEX .. RESET (write_buffer_limit) returns 4096 from the next seal; no REINDEX; segments already sealed stay.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 4096. Lower trades writer throughput and index size for faster queries on the grown index; it is also where the merge tiers start. ALTER INDEX applies from the next seal, no REINDEX.

#### sift(vacuum_segments)

`BM25` · default · since 2.0 · default 2

- **What it does:** segments VACUUM leaves the index with: fewer segments, faster queries
- **Gain:** 60 segments of 50,000 documents: count(*) 0.08 ms at 2 against 0.35 ms at 16; ranked top-10 0.18 against 0.33 ms
- **Impact:** index size: -; build, merge and writes: VACUUM 0.19 s at 2 against 0.10 s at 16 (60 segments of 50,000 documents); other: 2 against 16 (60 segments of 50,000 documents): VACUUM 0.19 s against 0.10 s, count(*) 0.08 against 0.35 ms
- **When:** VACUUM, autovacuum and the worker's merges of a sift index: the directory entries they leave. A segment more than merge_fanin times larger than the others is left alone; dead_rewrite_pct still applies.
- **Default:** 2 — 2: fastest queries of the measured values (count(*) 0.08 ms against 0.35 at 16, top-10 0.18 against 0.33) for a VACUUM 0.09 s longer (0.19 against 0.10 s; 60 segments of 50,000 documents).
- **Turn off:** n/a: a target (1 to 128). ALTER INDEX .. SET (vacuum_segments = 128) merges least; RESET returns 2; applies at the next VACUUM, no REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 2. Raise it for a shorter VACUUM of an index with many segments, at the price of slower queries. A segment far larger than the rest is left alone. ALTER INDEX applies at the next VACUUM.

#### sift(merge_fanin)

`BM25` · default · since 2.0 · default 8

- **What it does:** segments of one size tier that trigger a merge: lower keeps fewer segments
- **Gain:** none measured: query after 20 inserts of 2,000 documents 1.49 ms at fanin 2, 1.40 ms at 8 (140,000 documents)
- **Impact:** index size: 26.7 MB at fanin 2, 25.8 MB at 8 (-3.2%); build, merge and writes: 20 inserts of 2,000 documents 844 ms at fanin 2, 703 ms at 8 (-16.7%); other: lower merges more often (more write work), higher keeps up to merge_fanin - 1 segments a tier
- **When:** Merges chosen by the worker, by inserters needing room and by sift.merge(): segments of one size tier (a factor of merge_fanin, from write_buffer_limit). Range 2 to 32. VACUUM follows vacuum_segments.
- **Default:** 8 — 8: against 2, 20 inserts of 2,000 documents took 703 against 844 ms (-16.7%), the index was 3.2% smaller and queries equal (1.40 against 1.49 ms); the directory is kept under greatest(32, 2*merge_fanin) entries.
- **Turn off:** n/a: a tuning value (2 to 32). ALTER INDEX .. RESET (merge_fanin) returns 8 at the next merge; no REINDEX. No value turns merging off; async_maintenance = off moves it to inserters and VACUUM.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 8. Lower merges sooner (more write work, fewer segments to query); higher merges less and keeps up to merge_fanin - 1 segments per size tier. ALTER INDEX applies at the next merge.

#### sift(dead_rewrite_pct)

`BM25` · default · since 2.0 · default 50

- **What it does:** a lone segment is rewritten to drop dead documents above this percent
- **Gain:** 5 against 50, after deletes (400,000 documents, 10 rounds of 6%): count(*) 3.5 to 0.15 ms, top-10 4.1 to 1.5 ms
- **Impact:** index size: 5 against 50: file 133 against 92 MB after updates, 89 against 67 MB after deletes; build, merge and writes: 5 against 50: VACUUM 37.8 against 20.2 s (updates), 15.8 against 2.3 s (deletes); WAL 455 against 86 MB (deletes); other: 5 against 50 (400,000 documents, 10 update rounds): VACUUM 37.8 s against 20.2 s, index 133 against 92 MB; 100 never rewrites
- **When:** A lone segment of a sift index (one no merge picks) whose dead documents exceed this percent at VACUUM or a worker step. sift_col has the same option with default 10; sift_table uses sift.table_dead_rewrite_pct.
- **Default:** 50 — 50: 5 rewrote at every round and cost VACUUM 1.9x (updates) and 6.9x (deletes), WAL 5.3x on deletes and a 1.3-1.4x larger file for faster queries only after deletes (400,000 documents).
- **Turn off:** SET (dead_rewrite_pct = 100) never rewrites a segment on its own; dead postings are still cleared by VACUUM. ALTER INDEX .. RESET returns 50; applies at the next VACUUM or worker step, no REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 50. Lower it only for delete-heavy tables whose queries must be fast right after VACUUM; it costs VACUUM time, WAL and file size. 100 never rewrites. ALTER INDEX, no REINDEX.

#### sift(vacuum_truncate)

`BM25` · default · since 2.0 · default true

- **What it does:** VACUUM gives the free blocks at the end of the file back to the operating system
- **Gain:** -
- **Impact:** index size: index after churn and VACUUM 8.1 MiB against 34.7 before the space step (1,000,000 rows); v2.0 example 722 to 83 blocks; build, merge and writes: a brief exclusive lock when it is free; none while a physical standby streams (a cut would stall its replay for as long as its longest query on the index runs), the free end then stays in the file for the next rewrite; other: a brief exclusive lock when it is free; none while a physical standby streams (a cut would stall its replay for as long as its longest query on the index runs), the free end then stays in the file for the next rewrite
- **When:** VACUUM of a sift index when the free end is 1000 blocks or a 16th of the file and the exclusive lock is free at once. Not with VACUUM (PARALLEL n); free blocks inside the file are reused either way.
- **Default:** true — On, like vacuum_truncate of a heap table: the index fell from 34.7 to 8.1 MiB after churn and VACUUM (1,000,000 rows); the lock is taken only if free and held for the cut.
- **Turn off:** ALTER INDEX .. SET (vacuum_truncate = false); RESET returns it on; no REINDEX. Free blocks are then reused but not returned to the system, and a standby no longer replays the lock.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Set false when a hot standby must not cancel queries at replay of the lock or VACUUM must never take it; free blocks are then reused but not returned. ALTER INDEX, no REINDEX.

#### sift_col(write_buffer_limit)

`sift_col` · default · since 3.0 · default 4096

- **What it does:** size at which the write buffer is sealed; the base of the merge tiers
- **Gain:** 1024 against 4096 kB: ranked queries 2 to 4 times faster (sift text index, 100,000 rows; no sift_col figure)
- **Impact:** index size: 1024 against 4096 kB: index 72 to 98 MB (+37%), 4.5 times the segments (sift text index, 100,000 rows); build, merge and writes: 1024 against 4096 kB: promotion 37 to 13 ms but 3.7 times as many; 16-client INSERT 107k to 87k rows/s, COPY 47% lower (sift text index); other: the trade-off measured for the sift index (1024 against 4096 kB: 3.7x as many promotions); ALTER INDEX applies from the next seal
- **When:** Rows written after CREATE INDEX wait in the write buffer of a sift_col index until it is sealed at this size; it is also the base of the merge tiers (fan-in 8). Sealed segments are not changed.
- **Default:** 4096 — 4096 kB: 1024 promotes 3.7x as often, 16-client INSERT 107k to 87k rows/s, COPY 261k to 137k, index +37% (sift text index, no sift_col run), so writers pay more than reads gain.
- **Turn off:** n/a: a size, not a switch. ALTER INDEX .. SET (write_buffer_limit = N) applies from the next seal; RESET returns to 4096 kB; no REINDEX. Range 64 to 1048576 kB.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 4096 kB. A smaller buffer is promoted more often (more write work, a larger index) for faster reads of recent rows. ALTER INDEX applies it from the next seal; no REINDEX.

#### sift_col(dead_rewrite_pct)

`sift_col` · default · since 2.0 · default 0

- **What it does:** a lone segment is rewritten above this percent of dead rows, so that its units answer from pre-aggregates again; 0 rewrites it when the scans of its dirty units would cost more than the rewrite
- **Gain:** GROUP BY after three rounds of 30% updates and VACUUM: 0.82 ms (never rewritten) to 0.16 ms (10) (100,000 rows)
- **Impact:** index size: 4.05 MB (never rewritten) to 2.15 MB (10) (-46.8%, 100,000 rows after three rounds); build, merge and writes: VACUUM of the third round 61 ms (never rewritten), 56 ms (10): within noise (100,000 rows); other: a rewrite writes the whole segment and re-encodes every live row (4,200 instructions a row at 7 bytes, 35,400 at 75); 100 never rewrites
- **When:** VACUUM rewrites a lone segment of a sift_col index when half of it is dead or, with 0, when 100 scans of its dirty units (about 430 instructions a dead row up to 45 a row of a unit a tenth dead) cost more than the rewrite of its live rows (900 + 463 * bytes of a row); with a percent, above that share of dead rows. The tier merge and sift.col_auto_merge handle the rest. A segment below it keeps its dirty units, which answer from their pre-aggregates net of the dead rows. The BM25 index keeps 50.
- **Default:** 0 — 0, by cost: a rewrite re-encodes 900 + 463 * bytes instructions a live row, a query pays 430 a dead row of a dirty unit up to 45 a row, so 100 scans between two VACUUMs break even at about 9% dead rows in every unit of a 7 bytes a row segment and never below half for 75. 300,000 rows, 13% dead: VACUUM 1,356 against 1,354 Minstr at 7 bytes a row (unchanged), 292 against 9,528 at 75 bytes a row (6 array columns), where a grouped sum costs 17.2 against 0.5 Minstr until the rewrite.
- **Turn off:** ALTER INDEX .. SET (dead_rewrite_pct = 100) never rewrites a segment for dead rows; a percent from 1 fixes it; RESET returns to 0; no REINDEX. sift.col_auto_merge can still rewrite the index.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 0: a segment is rewritten when half of it is dead, or when the scans of its dirty units cost more than the rewrite (dead rows in every unit of a narrow segment from about 9%, a wide one not before half). Set a percent to rewrite earlier or later (WAL); 100 never rewrites. ALTER INDEX, no REINDEX.

#### sift_col(vacuum_truncate)

`sift_col` · default · since 2.0 · default true

- **What it does:** VACUUM gives the free blocks at the end of the file back to the operating system
- **Gain:** orders-per-country after VACUUM 11.49 to 7.75 ms (median, 1,000,000 rows after update/delete churn)
- **Impact:** index size: smaller: free MiB inside the column file after VACUUM 16.055 to 0.008; table and indexes 200.3 to 154.5 MiB (1,000,000 rows after churn); build, merge and writes: update/delete throughput medians unchanged; VACUUM takes a brief exclusive lock that it skips when busy; other: a brief exclusive lock when it is free; none while a physical standby streams (a cut would stall its replay for as long as its longest query on the index runs), the free end then stays in the file for the next rewrite
- **When:** VACUUM of a sift_col index: the exclusive lock only if free at once, tail segments copied into free runs, then the free end returned. A VACUUM that uses parallel index workers leaves it to the next one.
- **Default:** true — On, as for a heap: table and indexes 200.3 to 154.5 MiB and orders-per-country 11.49 to 7.75 ms after churn (1,000,000 rows); the lock is brief and skipped when busy, update/delete throughput unchanged.
- **Turn off:** ALTER INDEX .. SET (vacuum_truncate = false), no REINDEX; RESET restores true. sift.vacuum_rewrite_limit = 0 only stops the copy of tail segments, not the truncation of a free end.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Turn it off (ALTER INDEX SET vacuum_truncate = false) only if the brief exclusive lock is not acceptable; while a physical standby streams VACUUM cuts nothing anyway.

#### sift_col(paths)

`sift_col` · default · since 3.0 · default ''

- **What it does:** jsonb leaves are shredded into typed columns that filters and aggregates read like any column; with no option the paths come from the documents (sift.suggest_paths) and a jsonb column no longer needs them declared
- **Gain:** GROUP BY of an array element key 192 to 4.9 ms (100,000 documents); jsonb_each over a map 338 to 9.5 ms (1,000,000 rows)
- **Impact:** index size: index 2.3 MB against 56 MB heap and 6.2 MB for a GIN on the same documents (100,000 documents); the declared leaves are stored once more; build, merge and writes: build 609 ms against 651 ms for the GIN (100,000 documents); other: the declared leaves are stored once more; the chosen paths are written to the option as 'auto: spec' and a REINDEX keeps them; a table with no key that qualifies still refuses a jsonb column
- **When:** A sift_col index with a jsonb column: its leaves are stored as typed columns. Other columns are untouched; paths written by hand are never changed; a table with no qualifying key still refuses the jsonb column.
- **Default:** '' — Empty or auto so that a jsonb column needs no declaration: GROUP BY of an array element key 192 to 4.9 ms and an index of 2.3 MB against 6.2 MB for a GIN (100,000 documents).
- **Turn off:** n/a: the layout of a jsonb column; leave the column out of the index to shred nothing. SET (paths = 'auto') or RESET (paths) re-reads the sample; any change needs REINDEX at once, writes fail until then.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave empty or auto for a jsonb column: paths come from a sample at CREATE INDEX and REINDEX keeps them. Declare them by hand to choose leaves and types; SET paths = 'auto' re-reads the sample.

#### sift_col(compression)

`sift_col` · default · since 3.0 · default ''

- **What it does:** smaller chunks: zstd:3 -7.0%, lz4 -6.2% of the index against auto (6 columns, 100,000 rows)
- **Gain:** -
- **Impact:** index size: zstd:3 -7.0% and lz4 -6.2% of the index against auto, none +111.6% (2.12 MB auto; 6 columns, 100,000 rows); zstd:3 -2.8% on the 1,000,000-row store table; build, merge and writes: build 69 ms (auto, lz4), 81 ms (zstd:3), 54 ms (none); a grouped scan 10.1 ms (auto), 14.4 ms (zstd:3, 1.4x), 11.7 ms (none) (100,000 rows); other: zstd:3 builds 17% slower and a grouped scan reads 1.4x slower than auto (100,000 rows); REINDEX to apply
- **When:** Every chunk of the columns of a sift_col index when a segment is written; per column with 'auto; col=zstd:9'. Segments already written keep their chunks until REINDEX.
- **Default:** '' — auto adds LZ4 or zstd only where the cost rule says the saved bytes pay (LZ4 needs 25% on hot streams, zstd 70%). zstd:3 is -7.0% but builds 17% slower and scans 1.4x slower, over the limits.
- **Turn off:** SET (compression = 'none') stores plain chunks (index +111.6%, fastest scans); 'auto' or RESET (compression) is the default. REINDEX applies it to segments already written.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at auto: it adds a general-purpose layer only where it pays. Set zstd on a column for a smaller index at slower scans and builds; none for the fastest scans. REINDEX to apply.

#### sift_col(preagg)

`sift_col` · default · since 3.0 · default ''

- **What it does:** GROUP BY sums and counts answered from pre-aggregates stored with every unit
- **Gain:** GROUP BY one key 8.0 to 2.1 ms, by month 35.0 to 1.4 ms, top 10 groups 46.8 to 3.6 ms (10,000,000 rows, units scanned against pre-aggregates); 16 columns, 8 dimensions of 50 values: GROUP BY a dimension 1.92 to 0.32 ms, 5 of 8 dimensions answered to 8 of 8 (1,000,000 rows)
- **Impact:** index size: +1.1% to +1.2% of the index (300,000 and 3,000,000 rows; the store table's index is the same file with the 40% budget); a wide index with 20 to 200 values per dimension +2.1% to +41.4% (8 to 32 columns, 1,000,000 rows; 16 columns of 50 values +8.9% against +6.7%); off costs 0.07%; preagg = 'auto' adds 5.7% to 9.0% (40,000 to 300,000 rows, 26 columns); build, merge and writes: +3.9% serial build CPU (3,000,000 rows), inside the noise with workers; the 40% budget changes no build time beyond 0.1 s (0.7 to 0.8 s at 16 columns, 1.5 to 1.6 s at 32, 1,000,000 rows); preagg = 'auto' CREATE INDEX +23 to +25 ms ranking at 300,000 rows, REINDEX +8% to +22%; other: the default takes at most 40% of the encoded bytes of a unit as built (every measure of a kept dimension and its min and max; at least 8 kB; about a quarter of that on disk for dimensions of few values) and gives the cube's 16 dimensions to the columns in their order; auto ranks the plain columns by data at CREATE INDEX instead (the ones whose histograms fit 256 groups in a unit first) and writes the list as 'auto: a, b'; off keeps leaf sums only; applies to segments written afterwards
- **When:** GROUP BY, sums and counts answered from whole units of a sift_col index; the cube holds up to 16 dimensions. A unit with a dead row or a page not all-visible is answered from its rows. Segments written afterwards.
- **Default:** '' — The cube of a unit takes at most 40% of its encoded bytes as built (at least 8 kB), a decision for 3.4 that the index size does not bind the cube: GROUP BY a dimension 1.92 to 0.32 ms at 16 columns of 50 values, +8.9% of the index, at most +41% (32 columns of 200 values); the store index is unchanged. auto adds +5.7% to +9.0%, over the 3% limit: opt-in.
- **Turn off:** SET (preagg = 'off') keeps leaf sums only (index -8.0% on a 13-column index); RESET returns to default. Segments written afterwards; sift.col_refresh(index) or REINDEX rewrite the older ones.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at default. With more than 16 plain columns try auto (ranks them by data, +6% to +9% of the index); off keeps leaf sums only; cube(dim: measure) names the dims.

#### sift_col(rollup)

`sift_col` · default · since 3.1 · default ''

- **What it does:** names the leaves of array elements and map values a rollup groups by
- **Gain:** GROUP BY of an array element key 1,888 ms normal plan, 9.2 scanned, 2.0 ms from rollups (1,000,000 documents); 151 to 0.08 ms at 100,000
- **Impact:** index size: +2.0% of the index (100,000 and 1,000,000 documents, default rollups against rollup = 'off'); build, merge and writes: +3.4% to +4.2% build instructions (1,000,000 documents); serial build cpu 268 against 257 ms (100,000), inside the noise; other: explicit names hold up to 4096 tuples a unit with no byte budget; the default rule has a budget of 2% of the unit per list
- **When:** Array elements and map values of shredded jsonb paths (lists) of a sift_col index: the rollup groups them by their low-cardinality leaves, up to 4096 tuples a unit. Scalar columns use rollup_dims.
- **Default:** '' — The automatic rule has a 2% byte budget a list: +2.0% of the index, +3.4% to +4.2% build instructions (1,000,000 documents), GROUP BY of an array key 1,888 to 2.0 ms: inside the 3% and 5% limits.
- **Turn off:** SET (rollup = 'off') removes the rollups and keeps the cube; 'default' or RESET returns to the automatic rule. Segments written afterwards; older units change at a merge, REINDEX or sift.col_refresh(index).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at default: the rollups of array elements and map values are chosen by a 2% byte budget. Name the leaves (leaf, leaf) to answer more groupings, up to 4096 tuples a unit; off removes them.

#### sift_col(rollup_on_demand)

`sift_col` · default · since 3.1 · default true

- **What it does:** a scan that reads units for lack of their rollups asks the worker to rebuild them
- **Gain:** every unit answers from the rollups 1.9 to 3.1 s after ALTER INDEX SET rollup (1,000,000 documents, 276 units, worker on)
- **Impact:** index size: -; build, merge and writes: background rewrite of the segment (about 0.7 s for an 8 MB segment); the query itself writes nothing; other: a background rewrite of the segment; the query itself writes nothing
- **When:** A SiftAgg scan of exact units that lack their rollup (other dims, a missing measure). Not asked for: dirty units, minor 3 segments, units over the 4096-tuple cap; a hot standby never queues a request.
- **Default:** true — On: the only way old units get rollups without REINDEX or a merge: all answer from them 1.9 to 3.1 s after ALTER INDEX SET rollup (1,000,000 documents); the query writes nothing, a worker rewrites 8 MB in 0.7 s.
- **Turn off:** ALTER INDEX .. SET (rollup_on_demand = false) for one index; SET sift.col_rollup_on_demand = off for a session; sift.col_rollup_worker = off keeps requests queued for sift.col_rollup_refresh(index).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: a scan that lacks rollups queues a background rewrite. Turn it off to keep units as they are until a merge or REINDEX, or to refresh on a schedule with sift.col_rollup_refresh.

#### sift_col(dict_lookup)

`sift_col` · default · since 3.3 · default auto

- **What it does:** equality, IN and prefix tests read a dictionary of 1,024 strings or more in place, without parsing it whole: auto stores the order of its strings for the columns of point_columns, key_columns and return_columns, on for every column, off for none (the strings are read in place from blocks where that adds at most 3%)
- **Gain:** first query of a new backend, 300,000 rows: uuid equality 12.1 to 0.43 ms, text equality 29.3 to 0.54, text prefix 36.7 to 6.5
- **Formula:** S 191.00x saving 497855k instructions, G 7.7%, W 7.7%, R 1: B 107.7 against C 4.1, automatic
- **Impact:** index size: the order of the strings is 4 bytes a string: +10% to +70% of an index of one column; auto writes it only for point_columns, key_columns and return_columns; build, merge and writes: a point column of 600,000 unique 70-byte strings sharing a 23-byte prefix: CREATE INDEX +7.7% instructions (4,777 M to 5,147 M; +20.2% when the order was sorted by comparing whole strings); other: format minor 14; the order is 4 bytes a string: +10% to +70% of an index of one high-distinct text or uuid column, and since 3.6 the hash of the strings 5 bytes more (minor 25), for every dictionary in blocks of 1,024 strings or more too (a plain index: uuid +1.0 MB of 3.4 MB on 200,000 rows), none with off
- **When:** The order of the strings of a dictionary of 1,024 strings or more (text, varchar, uuid) for equality, IN and prefix tests, in segments written at sift.col_format_minor >= 14. Smaller dictionaries stay compact.
- **Default:** auto — auto: the order is 4 bytes a string (+10% to +70% of a one-column index), so only point_columns, key_columns and return_columns get it; their first lookups of a backend 12.1 to 0.43 ms (uuid, 300,000 rows).
- **Turn off:** SET (dict_lookup = off) stores no order (a lookup parses the dictionary whole; blocks still where they add <= 3%); sift.col_format_minor <= 13 writes neither. REINDEX changes older segments.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at auto: strings are searched in place and the lookup order is stored for the columns named in point_columns, key_columns and return_columns. on for every column; off for none. REINDEX to apply.

#### sift_col(cube_minmax)

`sift_col` · default · since 3.3 · default true

- **What it does:** min and max per group read from the cube: GROUP BY kind with min/max of an integer 5.5 to 0.34 ms (200,000 rows)
- **Gain:** GROUP BY with min/max of an integer 5.5 to 0.34 ms (200,000 rows); 7.4 to 0.20 ms against 115 ms normal plan (1,000,000 rows)
- **Formula:** S 19.60x saving 63505k instructions, G 8.4%, W 1.4%, R 0.5: B 30.5 against C 3.1, automatic
- **Impact:** index size: +0.17% of the index alone, +1.5% with superunit 16 (200,000 rows, 13 columns); +2.6% of a 13-column wide index; build, merge and writes: insert, promotion, merge, UPDATE and VACUUM CPU inside the noise (200,000 rows); other: format minor 7; +0.17% of the index alone, about +1.5% with superunit 16; insert, promotion and merge CPU inside the noise
- **When:** min() and max() of an integer or numeric(p,s) measure per GROUP BY group, from the cube of units written afterwards. Float and text have none; a unit with a dead row is scanned.
- **Default:** true — On: min/max GROUP BY 5.5 to 0.34 ms for +0.17% of the index (+1.5% with superunit 16), write CPU in the noise; the cost is column format minor 7, which a 3.2 library refuses.
- **Turn off:** SET (cube_minmax = false) writes what 3.2 wrote; RESET restores true. Units written afterwards; sift.col_refresh(index) or REINDEX rewrite older ones; sift.col_format_minor <= 6 also writes none.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Turn it off (WITH cube_minmax = false) only to write segments a 3.2 library can read or to save the extra bytes when no query takes min or max of an integer per group.

#### sift_col(array_lengths)

`sift_col` · default · since 3.6 · default auto

- **What it does:** cardinality(x), array_length(x, 1) and x IS [NOT] NULL of an array column are read from two integer columns kept next to the arrays, without decoding them
- **Gain:** presence and length tests of arrays, 1,000,000 rows, six array columns (instructions of one backend): count with IS NOT NULL AND cardinality(x) > 0 1,047 M to 10.8 M (135.9 to 0.91 ms; the sift_col plan before was 4,088 M, 538.9 ms); three arrays at once 1,214 M to 23 M; Inventory QA with count(DISTINCT id) FILTER over five arrays grouped by two columns 21,676 M to 256 M (2,049 to 25 ms); avg(cardinality(x)), max(array_length(x, 1)) grouped 1,867 M to 134 M (374 to 7.9 ms)
- **Formula:** S 97.00x saving 1036500k instructions, G 10.2%, W 12.0%, R 1: B 93.8 against C 5.8, automatic
- **Impact:** index size: +10.2% of the index (73,015,296 to 80,486,400 bytes; six array columns, 1,000,000 rows, 2 lanes each: 0.4 to 0.8 MB a lane); +9.8% at 300,000 rows (22.8 to 25.1 MB); build, merge and writes: CREATE INDEX +12.0% instructions (8,114 M to 9,090 M; 300,000 rows), REINDEX +12.5%, insert of 20,000 rows +9.0% (388 M to 423 M), update of 20,000 rows +3.5%, promotion +17%, merge +15%; delete and VACUUM unchanged; other: about 1 byte a row for each array column and lane (+10% of the index on six array columns, 1,000,000 rows); a build, merge or promotion writes two more columns for each array column; after a change the table takes no write until the REINDEX
- **When:** cardinality(x), array_length(x, 1), x IS [NOT] NULL and the same in FILTER, CASE, GROUP BY and aggregates, for int[], text[], numeric[] and any other array column of the index. Not array_length(x, 2) or higher, array_ndims, @>, &&, <@, = ANY or unnest: those still read the arrays. A range, inet or other image has no lengths.
- **Default:** auto — Automatic: S 97 on the presence and length tests against G 10.2% and W 12.0% pass the default formula (B 94 against C 5.8) for every array column, so no sample is needed; the NOTICE at CREATE INDEX names the columns and the off switch. The images cost a hash, an interning and a decompression for every row of an almost unique array column (4,100 instructions a row for 30-element arrays against 11 for the lane).
- **Turn off:** ALTER INDEX .. SET (array_lengths = off), then REINDEX (or array_lengths = off on CREATE INDEX): the index keeps the images alone, as before 3.6. No column format minor: the leaves are ordinary integer leaves of the schema.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave it. CREATE INDEX keeps the lengths of every array column and says so; array_lengths = off drops them (ALTER INDEX .. SET (array_lengths = off), then REINDEX).

#### sift_col(rollup_dims)

`sift_col` · default · since 3.3 · default ''

- **What it does:** GROUP BY and filter of two low-cardinality scalar columns read from the rollup of each unit: the pair is picked at CREATE INDEX (auto) or named
- **Gain:** GROUP BY of the pair, count 26.5 M to 17.7 M instructions a query and with a sum 36.4 M to 21.4 M (600,000 rows); 171.8 ms normal plan, 8.0 scanned, 3.5 ms from the rollup (1,000,000 rows); a filter on both values of the pair is slower (12.3 M to 16.0 M instructions)
- **Formula:** S 1.50x saving 8871k instructions, G 5.7%, W 1.6%, R 1: B 8.3 against C 2.2, automatic
- **Impact:** index size: +5.7% of the index (15,296,488 against 14,466,400 bytes; 600,000 rows, 214 units); +4.6% for two dims (96 cells a unit), +19% for three (200,000 rows); build, merge and writes: CREATE INDEX +1.6% instructions (13,214 M to 13,503 M; 600,000 rows); CREATE INDEX +12 to +47 ms of sampling; other: format minor 9; +5.3% of the index, build +25% (two columns); auto picks the pair at CREATE INDEX
- **When:** GROUP BY of two or more dims, or a key plus a filter on another dim, over 2 to 6 boolean, integer, date, numeric(p,s) or text columns; auto takes the pair with the fewest combinations that fits the cells of a unit (a tenth of its rows, at most 4,096) in 90% of 8 sampled stretches. Not float, count(DISTINCT) or a non-dim filter; too many cells in a unit: none.
- **Default:** '' — Automatic: S 1.50 on the GROUP BY of the pair against G 5.7% and W 1.6% pass the default formula (B 8.3 against C 2.2), for the pair a statistic of the table picks (sampled stretches); a point filter on both values of the pair reads more. The NOTICE at CREATE INDEX names the columns and the off switch, not the MB: the size of the cells is known only after the build (sift.col_sections).
- **Turn off:** ALTER INDEX .. SET (rollup_dims = 'off'), then REINDEX (or rollup_dims = 'off' on CREATE INDEX): no rollup of rows in new units; older units keep theirs until a merge or REINDEX. Column format minor 9.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave it. CREATE INDEX picks the pair when two columns of a unit have combinations that fit its cells and tells which; rollup_dims = 'off' writes none, a list of columns names the pair.

#### sift_col(calendar_columns)

`sift_col` · default · since 3.3 · default ''

- **What it does:** GROUP BY date_trunc('month', d) and date ranges from calendar cells: 2.4 to 0.54 ms (200,000 rows)
- **Gain:** GROUP BY date_trunc('month', d) 2.4 to 0.54 ms (200,000 rows); 7.7 to 0.74 ms against 320 ms normal plan (1,000,000 rows)
- **Formula:** S 6.42x saving 26296k instructions, G 0.3%, W 4.0%, R 1: B 38.1 against C 0.9, automatic
- **Impact:** index size: +1.0% of the index for one column, +1.8% for two; beside jsonb paths only when named: +9.3% to +12.1% of a 13-column wide index; build, merge and writes: build, insert, promote and merge CPU within noise (200,000 rows); other: format minor 8; +1.0% of the index for one column, +1.8% for two (superunits no longer merge the cells); with JSON paths only when named (3.4: `all` or a list; +9.3% to +12.1% of the main index of an application); the cube holds at most 16 dimensions and the calendar cells come first: the default dims give way (the dates' own histograms, then the last columns; a NOTICE names them), a cube(...) of the user does not, so a list that does not fit is refused, `all` keeps the columns that fit and the build names the ones it left out
- **When:** date, timestamp, timestamptz columns: GROUP BY date_trunc, extract or a cast, and ranges, from calendar cells. The cube holds 16 dimensions, cells first. A timestamptz key in the session zone is scanned.
- **Default:** '' — all: +1.0% of the index for one column, +1.8% for two, build and merge CPU in the noise, GROUP BY month 2.4 to 0.54 ms. Beside jsonb paths +9.3% to +12.1%, so only when named.
- **Turn off:** SET (calendar_columns = 'off') or a list of the columns you group by; RESET returns to all. Units written afterwards; older units keep their cells until a merge or REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at default (every date and timestamp column; not with jsonb paths or a hand-made cube). List the grouped columns, or off, to save bytes; the cube holds at most 16 dimensions.

#### sift_col(key_columns)

`sift_col` · default · since 3.3 · default ''

- **What it does:** count(DISTINCT col) over whole units reads no key chunk (2.0x to 8.6x on the foreign keys of three 200,000-row tables); an equality or IN test skips the units that lack the key (integer equality 0.82 to 0.41 ms)
- **Gain:** count(DISTINCT fk) over whole units 16.1 to 1.11 ms (1,000,000 rows, 123 units); 2.0x to 8.6x on three 200,000-row tables
- **Formula:** S 3.49x saving 3946k instructions, G 4.3%, W 5.5%, R 1: B 25.6 against C 2.5, automatic
- **Impact:** index size: auto 0.0% on a column of 100 distinct values and a named column of 5,000 random keys 0.0% (2.12 MB; 100,000 rows); +1.1% to +1.4% on the foreign keys of three 200,000-row tables; a filter column named in the list +3.9% (1,000,000 rows, four columns); build, merge and writes: build 87 to 80 ms (auto) and 74 to 82 ms (named); insert of 20,000 rows and update of 10% within noise (100,000 rows); other: format minor 15; auto keeps a key list or bitmap of the int2, int4 and date foreign keys and *_id columns that fit 3% of a unit: +1.1% to +1.4% of the index on the copies; a named filter column of random keys adds no bytes and no gain on them (build within noise at 100,000 rows)
- **When:** count(DISTINCT col) over whole exact units without GROUP BY, and equality or IN tests that skip units lacking the key, on non-unique int2, int4, date foreign keys and *_id. A dead row or a cut zone: scanned.
- **Default:** '' — auto: lists only within 3% of a unit: +1.1% to +1.4% of the index, build in the noise, count(DISTINCT) of foreign keys 2.0x to 8.6x. A named filter column of random keys adds no bytes or gain. Minors 10 and 15.
- **Turn off:** SET (key_columns = 'off') then REINDEX (the build NOTICE names it); RESET returns to auto. ALTER INDEX alone applies to units written afterwards; older units are scanned.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at auto: non-unique foreign key and *_id columns of int2, int4 and date get a key list or bitmap within 3% of a unit. Name columns for equality filters; off removes it. REINDEX to apply.

#### sift_col(key_table)

`sift_col` · default · since 3.5 · default true

- **What it does:** count, sum, min and max WHERE a key = x, IN, a range or GROUP BY the key (a key_columns leaf with more than 256 values in a unit) read one record of each segment, not its units: 0.107 ms at 900,000 rows against 5.20 ms (300 keys)
- **Gain:** count, sum, min, max WHERE key = x over 900,000 rows 5.20 to 0.107 ms and 100,000 rows 0.49 to 0.054 ms (300 keys, 1 session, warm); count WHERE key IN (3 values) over 1,000,000 orders 78 to 0.33 ms (bench case key_table_point)
- **Impact:** index size: +2.2% of the segments' bytes at 100,000 rows, +0.75% at 300,000, +0.46% at 900,000 (300 keys, two measures); a segment whose table would pass 3% has none or fewer measures; build, merge and writes: CREATE INDEX +2.4% and +2.6%, insert and promotion of 20,000 rows +0.03% and +0.9%, merge +0.03% and +4.3%, VACUUM rewrite +0.03% and +3.0% instructions (100,000 rows, 200 and 3,000 keys); other: format minor 22; a table of at most 3% of the segment (+0.46% to +2.2% of the segments' bytes at 300 keys); +2.6% instructions on CREATE INDEX, +4.3% on a merge, +0.9% on insert and promotion
- **When:** WHERE a key_columns leaf (int2, int4, date) = x, IN, a range or GROUP BY, nothing else tested, count/sum/avg/min/max of the numeric, time and integer leaves the table keeps; every unit of the segment exact. A dead row, an IS NULL or another leaf: read from the units.
- **Default:** true — The rule of 3.3: over 5% on the relevant class (4x to 22x), growth at most 3% of a segment by construction, write cost under 5% on every path measured; a table that does not fit is not written.
- **Turn off:** ALTER INDEX .. SET (key_table = off), then REINDEX or the next merge; segments written before keep theirs and are still read.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Set off for an index that is written far more than it is read by key, or to keep the segments readable by 3.4. ALTER INDEX applies to the segments written afterwards.

#### sift_col(count_columns)

`sift_col` · default · since 3.5 · default ''

- **What it does:** count(*) of one test of the column (`LIKE 'abc%'`, an equality, `<`, `<=`, `>`, `>=` in the C collation) is read from two places of the column's dictionary, not from its rows: 4,012 matching rows of 1,000,000, 5.3 ms to 0.14 ms
- **Gain:** count(*) WHERE txt LIKE 'k2a%' (4,012 rows) 5.3 to 0.14 ms, LIKE 'k2%' (62,564 rows) 3.7 to 0.14 ms, < 'kf' 5.3 to 0.18 ms, 1,000,000 rows, one text column of unique 13-byte strings, 4 segments, warm, median of 40 runs
- **Formula:** S 38.00x saving 3200k instructions, G 0.2%, W 0.4%, R 0.5: B 37.3 against C 0.1, automatic
- **Impact:** index size: +131 KB (+0.17%) of 77.4 MB at 1,000,000 rows with 1,000,000 unique strings in one of eight columns (the first layout, a u32 for each string, was +4.0 MB, +4.5%); a column of few strings has none; build, merge and writes: CREATE INDEX +0.44% instructions (9,338 M without, 9,379 M with the counts, 1,000,000 rows, 8 columns); merge and promotion write the same blocks (the count of each code is kept while the units are written); other: format minor 24; 0.13 bytes for each distinct string of unique strings (+131 KB, +0.17% of the segments of the 8-column test table at 1,000,000 unique strings), about 0.4 bytes for strings of one to three rows; +0.44% instructions on CREATE INDEX
- **When:** count(*) and nothing else, one test of the column (a prefix, an equality or a range in the C collation), a text leaf whose dictionary is read in place, every unit of the segment exact (clean, all pages all-visible, the directory current), a segment with superunit summaries. A dead row, a second test, another aggregate or inline strings (a string over 255 bytes in a unit): read from the units; the rows of the write buffer are always read.
- **Default:** '' — The formula of DEFAULT-FORMULA.md: speedup 38x (count of a prefix 5.3 to 0.14 ms, 3.2 M instructions saved), growth 0.17%, write +0.44%: benefit 107 units against a cost of 0.14.
- **Turn off:** ALTER INDEX .. SET (count_columns = 'off'), then REINDEX or the next merge; segments written before keep theirs and are still read.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Set off for an index that is written far more than it is counted by prefix or range, or to keep the segments readable by 3.4. ALTER INDEX applies to the segments written afterwards.

#### sift_col(key_cut)

`sift_col` · default · since 3.4 · default ''

- **What it does:** a lookup or a range of the key reads the units of its range whatever the order of the heap: one key value in 1 to 3 of 419 units against all 419 by zones (1,500,000 rows, 8 columns, 15% of a key's rows outside their heap range): count/sum of one key 5.06 to 0.35 ms, the busiest key 4.53 to 0.31, a list of 10 keys 4.80 to 0.58, a key and a date range 4.20 to 0.41; the units keep their range under churn and after pg_repack, because promotion, merge and REINDEX cut again by the key
- **Gain:** chosen only where an equality on the key reads at least 8 times the units its rows fill and at least 64 units: count/sum of one key 5.06 to 0.35 ms, 4.53 to 0.31 for the busiest key, 4.80 to 0.58 for 10 keys (1,500,000 rows, 8 columns, 419 units); a scan that does not use the key 3.56 and 3.87 ms (noise); not cut: a heap-ordered column of the index, order_columns or point_tid_columns, an index whose predicted size grows by more than 3%
- **Formula:** S 8.18x saving 4855k instructions, G 18.4%, W 26.3%, R 1: B 43.1 against C 11.4, automatic
- **Impact:** index size: predicted from the sample and checked: -9.9% predicted, -13% serial and -5% parallel measured (1,500,000 rows, 8 columns, a key whose rows are partly scattered); an automatic cut needs a predicted growth of at most 3%, above it the NOTICE advises key_cut = 'col' with the predicted growth (+5.3% at 11 independent integer columns, 1,000,000 rows); build, merge and writes: build +41% (2.30 to 3.25 s serial, 1.29 to 1.83 s parallel, 1,500,000 rows; +30% without the sample), promotion +21% (0.047 to 0.057 s for 50,000 rows), insert of 50,000 rows 0.20 against 0.205 s; other: format minor 17; CREATE INDEX and REINDEX read a sample of the heap to decide (at most 400,000 rows, or 32 superblocks when they hold more) (0.07 to 0.25 s on a 1,500,000-row table); chosen, the build sorts the rows by the key and writes each once in its unit: build 2.30 to 3.25 s serial and 1.29 to 1.83 s parallel (+41%), a promotion of 50,000 rows 0.047 to 0.057 s (+21%), insert unchanged (0.20 s); the index 13% smaller serial and 5% smaller parallel on the measured table; the units list their TIDs (0.65 to 1.5 bytes a row, by how scattered the key is), so an index of independent columns grows 30% at 2 leaves, 9% at 6, 5.3% at 11, and the build then only advises; segments are minor 17, which 3.2 and older refuse
- **When:** CREATE INDEX or REINDEX of an index with no key_cut (or key_cut = 'auto') on a table of at least 64 superblocks (16,384 pages): the first key-shaped column (foreign key or *_id, not unique) of the index whose equality would read at least 8 times the units its rows fill and at least 64 units, when no other leaf follows the heap order (a cut would widen its zones), the index has no order_columns or point_tid_columns, and the predicted size of the index grows by at most 3%. When no key qualifies, the first date or timestamp column whose range holding the rows of one unit reads at least 8 times the units those rows fill and at least 64 units, under the same guards, and cut by or left alone, never advised (3,000,000 rows, ts scattered: a day 9.05 to 0.23 ms, 30 days 11.1 to 0.38, a key and a time window 6 to 12 times faster; the rows of one key 12.6 to 28.4 ms). Not chosen: a small table, a key already clustered, a predicted growth above 3% (advised).
- **Default:** '' — Default since 3.4: where it applies an equality on the key reads 5 to 15 times less (the busiest key 4.53 to 0.31 ms) and the 3.3 rule holds: the index is predicted, from a sample, to grow by at most 3% (it shrank 5 to 13% on the measured table); a build 41% longer and a promotion 21% longer pay for it. The narrow, heap-ordered, order_columns and growing cases it loses on are left out by the rule.
- **Turn off:** ALTER INDEX .. SET (key_cut = 'off') then REINDEX (or CREATE INDEX .. WITH (key_cut = 'off')); the choice is written as 'auto: col' and the NOTICE names it. ALTER INDEX .. RESET (key_cut) and a REINDEX decide again.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave it automatic: CREATE INDEX writes key_cut = 'auto: col' and says what it measured and predicted; where it would grow the index it prints the command instead. sift.key_cut_decision(index) says what a REINDEX would decide. Set key_cut = 'off' on an index that also sorts or ranges by another column without the key (an unkeyed ORDER BY .. LIMIT 0.30 against 0.91 ms at 200,000 rows), or name the columns yourself. Segments are format minor 17; RESET and REINDEX roll back.

#### sift_col(key_cut_rows)

`sift_col` · default · since 3.4 · default 0

- **What it does:** the rows a key-cut unit aims at; the default is the rows of an average superblock (1,024 when the table was not analyzed), which keeps the number of units and the planner's per-unit costs
- **Gain:** none at 100,000 rows: a key range of 40 values 0.20 ms with 256 rows and with the default, 0.22 against 0.23 ms with 4,096
- **Impact:** index size: +69.0% of the index at 256 rows, -9.4% at 4,096 (100,000 rows, default 0); build, merge and writes: build +6.8% at 256, -0.5% at 4,096; insert of 20,000 rows and update of 10% within noise (100,000 rows); other: format minor 17; fewer rows, more units; more rows, a coarser range per unit
- **When:** Read only with key_cut: the rows a key-cut unit aims at in a build, promotion or merge. 0 is the rows of an average superblock (reltuples / relpages * 256, 1,024 when never analyzed); 64 to 32,768.
- **Default:** 0 — 0 keeps the number of units and the planner's per-unit costs of the superblock cut; at 100,000 rows 256 rows is +69.0% of the index, 4,096 -9.4%, with no query gain (0.20 against 0.22 ms).
- **Turn off:** n/a: a size, not a switch. RESET (key_cut_rows) or 0 returns to superblock-sized units; it has no effect without key_cut. Applies to units written afterwards.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 0: units aim at the rows of an average superblock. Fewer rows make more units, more rows a coarser key range per unit. Only used with key_cut.

#### sift_col(order_array_columns)

`sift_col` · default · since 3.4 · default ''

- **What it does:** A range of a column whose values are scattered over the heap (up to 16,384 rows) hands the heap the addresses between its bounds from the sorted keys of each segment, with no unit read: 790 of 800,000 rows 18.4M to 2.4M instructions, 7,900 rows 44.1M to 19.1M; ORDER BY col LIMIT from 1 to thousands (no join or aggregate; comparisons of the key with constants bound the part read) merges the arrays of the segments and fetches n rows of the heap, whatever the number of units; ORDER BY a text column LIMIT n (one key, no condition) reads n entries of each segment and merges them by the strings under the collation of the column: 500,000 rows 403M to 0.33M instructions (a b-tree 0.22M), 3,000,000 rows 2,429M to 0.86M
- **Gain:** narrow range (790 of 800,000 rows, random int4), instructions per statement: covering 18.4M to 2.4M, rows 18.7M to 2.6M, count(*) 8.3M to 2.6M; 7,900 rows covering 44.1M to 19.1M, rows 45.8M to 20.8M; range plus an equality of a directory column (grp = 7) 8.3M to 2.8M; ranges over 16,384 rows, IN lists and count(*) of 7,900 and 160,000 rows unchanged (8.7M, 11.5M, 0.46M); LIMIT n as in the earlier row; a date or timestamp column alone in the index (scattered, no sorted run), instructions per statement at 500,000 / 1,000,000 / 3,000,000 rows (a b-tree): ts >= x ORDER BY ts LIMIT 10 9.4 / 18.3 / 30.5M to 0.38 / 0.39 / 0.42M (0.19 / 0.21 / 0.20M), ORDER BY ts DESC LIMIT 10 2.83 / 2.80 / 1.94M to 0.28 / 0.27 / 0.28M (0.19 / 0.18 / 0.16M), max(ts) WHERE ts < x 24.1 / 18.3 / 30.6M to 0.43 / 0.45 / 0.50M (0.24 / 0.24 / 0.20M), OFFSET 1000 23.2 / 32.3 / 45.5M to 2.1 / 2.1 / 2.1M (1.6M), a day's count 4.0 / 7.8 / 22.3M to 1.6 / 3.0 / 8.6M (0.5 / 0.9 / 3.2M); min(ts), max(ts) unchanged (0.38 / 0.42 / 0.59M to 0.38 / 0.42 / 0.59M, the pre-aggregates answer it)
- **Formula:** S 7.00x saving 16000k instructions, G 21.0%, W 22.0%, R 1: B 39.9 against C 11.4, automatic
- **Impact:** index size: auto adds nothing to a table under 100,000 rows or to a column whose array would be more than 25% of the index; +21% of a 7-column index of 30 bytes a row and +7% of a 93-byte one (800,000 rows, one scattered int4 column); at most two columns, a date or timestamp before an integer; a one-column timestamp index 1.87 to 4.12 MB (3.7 to 8.2 bytes a row, +121%) at 500,000 rows, 10.8 to 25.2 MB at 3,000,000 (a b-tree on the column 11.2 and 67.1 MB); a text array adds the strings again (8.8 bytes a row for 9-byte strings, 38 for 61), +20.8% of the 9-column lab index (26.4 to 31.9 MB); build, merge and writes: CREATE INDEX +22% instructions (5.99G to 7.32G, 800,000 rows, one column, 7-column index); the earlier row's insert, merge and VACUUM figures per order column; a one-column timestamp index +34.5% instructions (1.031G to 1.387G, 500,000 rows, one process); a text array: 9-column index of 500,000 rows 6.32G to 17.90G instructions (+183%) under pt_BR.UTF-8, 4 columns under C 2.84G to 4.62G; other: format minor 30; auto: 4.4 bytes a row for a 4-byte key and 5.6 for an 8-byte one, plus the sorted run it extends (2.2 and 3.5): a 7-column index of 800,000 rows 24.3 to 29.4 MB (+21%), +7% of the 93 bytes a row of an index with point_tid_columns; the build +22% instructions (5.99G to 7.32G); segments are minor 30, which 3.5 and older refuse; a text column 7.4 bytes a row (+14% of a 9-column index), CREATE INDEX +81% under the C collation and +390% under pt_BR (the sort of the strings), segments are minor 32, which 3.5 and older refuse
- **When:** A range of a scattered column up to 16,384 rows, and ORDER BY col LIMIT n over one integer, date or timestamp key of order_columns with no join or aggregate; any other shape reads the units (a unit with a dead row is read).
- **Default:** '' — Default on by the formula of benefit B = log2(S)/log2(1.05) = 33 for S = 5 (narrow ranges, 18.4M to 2.4M instructions) against cost G/3 + W/5 = 7.0 + 4.4 = 11.4 for G = 21% and W = 22%; G is under the 25% ceiling and the saving is 16M instructions a query. Built only where it pays: 100,000 rows or more, 1,000 or more distinct values, correlation under 0.99, not a column that cuts the units, each array within 25% of the index; a time column first. A date or timestamp has no ceiling: B = log2(25)/log2(1.05) = 66 (ts >= x ORDER BY ts LIMIT, 9.4M to 0.38M) against C = G/3 + W/5 = 121/3 + 34.5/5 = 47 for the array alone (4.6 bytes a row over a one-column index of 3.7; the sorted run it used to bring, +2.1 bytes and +18% build, is not written, since the array answers what the run did for the same shapes), the index staying under half of the b-tree it replaces. A text or varchar column (strings up to 64 bytes) by the same formula: S = 300 at the 100,000-row floor (82.8M to 0.27M instructions), B = 117 against C = G / 3 + W / 5 with G the array in the share of the index and W 62% of the build of 4 columns under the C collation and 420% under a locale, scaled by 4 / the columns: the 9-column lab index (500,000 rows) C = 48 (G 31%, W 187%), a 4-column index under a locale 110, 3 columns or fewer over B; a narrow index under a locale gets none (the sort of the strings is a larger share of a smaller build), and the build says so once in a NOTICE that names the column, B and C and the explicit order_array_columns = 'col' that builds it
- **Turn off:** SET (order_array_columns = 'off') then REINDEX (the build NOTICE names the columns and the MB); RESET returns to auto, decided again at the next REINDEX. New segments only; older ones keep their arrays until a merge or REINDEX, and the scan of the units answers meanwhile.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at auto: a column of 1,000 or more distinct values whose correlation with the heap order is under 0.99, on a table of 100,000 rows or more, gets the array: a date or timestamp column when the default formula pays (whatever the width of the index: B = 66 against C = 47 for a one-column timestamp index of 3.7 bytes a row), an integer column when it is within 25% of the index (at most two columns, a date or timestamp before an integer one). The choice is made at CREATE INDEX and REINDEX from the statistics the table has then, and a table of 100,000 rows or more that has none is analyzed first (a NOTICE says so; an ANALYZE afterwards changes nothing until the next REINDEX). Name the columns of order_columns for ORDER BY .. LIMIT on a smaller table or a column the rule leaves out; off removes it. REINDEX to apply.

#### sift_col(order_array_group)

`sift_col` · default · since 3.4 · default ''

- **What it does:** WHERE group = x (or group IN of at most 16 values, read one after the other and merged) ORDER BY col LIMIT n (n from 1 to thousands, with a range or the page after a key), and min/max(col) WHERE group = x, read n entries of the group x in each segment and n rows of the heap, whatever the rows of the table: the shape a b-tree on (group, col) serves. By default the build adds this second order to each date and timestamp array, beside the plain one, which keeps answering the queries that do not pin the group; with a column named, every array is grouped and a query that does not pin the group reads the units
- **Gain:** WHERE grp = x ORDER BY ts DESC LIMIT 10, 400 to 1,000 groups, random timestamps, 500,000 rows, instructions of one backend: 3.34M to 0.42M (a b-tree on (grp, ts) 0.27M); ts > x ORDER BY ts LIMIT 10 3.85M to 0.52M (b-tree 0.32M); max(ts) WHERE grp = x 3.27M to 0.61M (b-tree 0.35M); plain ORDER BY ts LIMIT 10 0.281M to 0.284M
- **Formula:** S 7.90x saving 2900k instructions, G 13.0%, W 6.0%, R 1: B 42.4 against C 5.5, automatic
- **Impact:** index size: +13.2% of the index (500,000 rows, 9 columns, one scattered timestamp array); +74% to +77% over the runs for a named column; build, merge and writes: CREATE INDEX +5.7% instructions (6.347G to 6.708G, 500,000 rows, single process); other: format minor 31; auto: 6.0 B a row for each date or timestamp array (the plain order stays): 500,000 rows, random timestamps, a 9-column index 25.2 to 28.5 MB (+13.2%). With a column named: 6.0 B a row for each array column over the runs (the group is a third part of each entry), a row with no group is not in the arrays
- **When:** WHERE group = constant or IN (up to 16 constants) [AND range or page after a key on the column] ORDER BY column [, ids] LIMIT n, and min/max of the column with the group pinned, over an index with a second order, and a write buffer of at most 4,096 rows of the group (or 8 times n); any other shape reads the plain order or the units.
- **Default:** '' — It is on: a group of 400 values costs +13.2% of the index and +5.7% of the build, and the group shapes go from 12x to 44x a b-tree (500,000 to 3,000,000 rows) to 1.5x to 2x; no table with fewer than 100 values in the column, or fewer than 64 rows apiece, gets one.
- **Turn off:** order_array_group = 'off', then REINDEX: new segments have the plain order only; older ones keep theirs until a merge or REINDEX, and the plain order answers meanwhile.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave it on. The build picks the integer column of most values (at least 100 values and 64 rows apiece, scattered in the heap) when an array of a date or timestamp is built; set order_array_group = 'off' and REINDEX to drop the second order, or name a column to group every array.

#### sift_col(superunit)

`sift_col` · default · since 3.3 · default 16

- **What it does:** a scan no predicate cuts through reads one summary per group of units: whole-table count/sum 0.26-0.37 to 0.05-0.08 ms, flat as units grow
- **Gain:** whole-table count/sum 60.6 ms normal plan, 4.8 scanned, 0.11 ms with superunits (1,000,000 rows); 0.26-0.37 to 0.05-0.08 ms at 200,000
- **Formula:** S 3.20x saving 4539k instructions, G 2.6%, W 1.3%, R 1: B 23.8 against C 1.1, automatic
- **Impact:** index size: +1.0% of the index (groups of 64: +0.8%, of 4 to 8: +1.3% to +1.5%); +2.6% with cube_minmax off (13-column wide index); build, merge and writes: insert, promotion, merge, UPDATE and VACUUM CPU inside the noise; a range on a table of under 16 units about 40 microseconds more; other: format minor 12; +1.0% of the index (groups of 64: +0.8%, of 4 to 8: +1.3% to +1.5%); a range query on a 2-unit table 0.10 to 0.13-0.15 ms
- **When:** Whole-table aggregates and ranges covering a group of exact units (count, sum, GROUP BY); a predicate cutting a group, a float sum or count(DISTINCT) falls back to the units. Segments written afterwards; 2 to 256.
- **Default:** 16 — 16: +1.0% of the index (groups of 64 +0.8% but ranges fall back more; 4 to 8 +1.3% to +1.5%), whole-table count/sum 0.26-0.37 to 0.05-0.08 ms, write CPU in the noise; a 2-unit range 0.10 to 0.13-0.15 ms.
- **Turn off:** SET (superunit = 0) writes the bytes 3.2 wrote (no minor 12); RESET restores 16. Older segments keep summaries until a merge or REINDEX; SET sift.agg_super = off ignores them in a session.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 16. 0 turns summaries off (the 3.2 bytes); 2 to 256 sets the units a summary covers. Larger groups cost fewer bytes and answer fewer ranges whole. REINDEX or merge applies it to new units.

#### sift.prefix_cache_size

`BM25` · default · since 1.2 · default 8192 kB

- **What it does:** a backend keeps the postings of broad prefixes for the next query
- **Gain:** 1-letter prefix on a million short rows: 10-15 ms first call in a backend, under 1 ms from the cache
- **Impact:** index size: as `sift.max_expansions`; build, merge and writes: as `sift.max_expansions`; other: up to this much memory per backend; 0 keeps none
- **When:** Broad prefixes (h*, ab*) of a sift index: a backend keeps their postings for later queries; only broad prefixes are cached. The memory is per backend, so size it by the pool. Superuser setting.
- **Default:** 8192 kB — 8192 kB: 10-15 ms for a one-letter prefix of a million short rows on the first call in a backend, under 1 ms from the cache; the memory is the only cost, bounded by the setting.
- **Turn off:** SET sift.prefix_cache_size = 0 keeps no cache (superuser: postgresql.conf, ALTER SYSTEM, ALTER DATABASE / ROLE .. SET); every broad prefix then gathers its postings again.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 8192 kB. Raise it when many broad prefixes repeat in one backend; 0 keeps no cache. The memory is per backend. Superuser setting.

#### sift.filter_pushdown

`BM25` · default · since 1.1 · default on

- **What it does:** switches of the ranked scan, the count(*) pushdown, the bitmap of the other quals and the read-ahead
- **Gain:** 0.1% filter on 218,058 documents: 23.6 to 0.22 ms (clustered), 15.0 to 0.86 ms (scattered)
- **Impact:** index size: -; build, merge and writes: -; other: none; off gives the plan PostgreSQL has without them
- **When:** Ranked scans with other quals that have a btree, BRIN or GIN index; the bitmap is built where the planner costs it cheaper. A loose filter (a tenth of the rows or more) stays after the ranker. Not unranked scans.
- **Default:** on — on: the planner costs both plans and picks by cost: a 0.1% filter went from 23.6 to 0.22 ms (clustered) and 15.0 to 0.86 ms (scattered); a loose filter keeps the post-filter plan.
- **Turn off:** SET sift.filter_pushdown = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf) keeps the post-filter plan; force pushes down whenever a bitmap path exists; RESET returns on. Prepared plans keep their choice.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on (the planner costs both plans). force pushes the bitmap down whenever one exists, to test a plan; off keeps the post-filter plan. A filter keeping a tenth of the rows stays after the ranker.

#### sift.prefetch

`BM25` · default · since 1.0 · default on

- **What it does:** switches of the ranked scan, the count(*) pushdown, the bitmap of the other quals and the read-ahead
- **Gain:** none with a warm cache: 4.41 ms off, 4.49 ms on (100,000 documents, unordered scan); the read-ahead is for a cold heap, which this host cannot make (the page cache cannot be dropped)
- **Impact:** index size: -; build, merge and writes: -; other: none; off gives the plan PostgreSQL has without them
- **When:** Unordered sift scans of a heap table: reads ahead the heap pages about to be visited. Not ranked scans, not a sift_table. A cold heap gains; a warm cache does not (4.41 ms off, 4.49 ms on).
- **Default:** on — On: the read-ahead is for a cold heap, which the benchmark host cannot make (page cache cannot be dropped); with a warm cache it measured no difference, so the default rests on design, not on a figure.
- **Turn off:** SET sift.prefetch = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf), only to compare plans; RESET returns on.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Reads ahead the heap pages an unordered scan is about to visit; set off only to compare plans.

#### sift.async_maintenance

`all` · default · since 1.0 · default on

- **What it does:** promotion and merges run in a background worker that the settings below throttle
- **Gain:** 20 inserts of 2,000 documents into a text index that seals every 64 kB: 696 ms with the writer merging, 385 ms with the worker (-44.7%)
- **Impact:** index size: -; build, merge and writes: -; other: a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O)
- **When:** Promotion of write buffers and merges of sift and sift_col indexes in a dynamic background worker (needs a worker slot). Also required by table_auto_defrag, table_async_promote and col_auto_merge. Superuser setting.
- **Default:** on — On: 20 inserts of 2,000 documents into a text index sealing every 64 kB took 385 ms with the worker against 696 ms with the writer merging (-44.7%); after a bulk load it promotes the tail seals (1.3 ms against 8-16 ms).
- **Turn off:** SET sift.async_maintenance = off (superuser: postgresql.conf, ALTER SYSTEM, ALTER DATABASE .. SET): promotions and merges are left to VACUUM, sift.merge() and inserting sessions.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. A background worker promotes write buffers and merges segments so writers do not; off leaves them to VACUUM and inserting sessions. It needs a worker slot (max_worker_processes).

#### sift.maintenance_naptime

`all` · default · since 2.0 · default 2000 ms

- **What it does:** promotion and merges run in a background worker that the settings below throttle
- **Gain:** after a bulk load the worker promotes the tail seals: queries 1.3 ms instead of 8-16 ms on unpromoted segments
- **Impact:** index size: as `sift.async_maintenance`; build, merge and writes: as `sift.async_maintenance`; other: a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O)
- **When:** The maintenance worker of sift and sift_col indexes (and sift_table passes) stays this long after it starts, holding a worker slot, and asks again at most once per naptime. Range 200 to 60000 ms; reload (SIGHUP).
- **Default:** 2000 ms — 2000 ms: after a bulk load the worker still promotes the seals of the rest of the window (queries 1.3 ms instead of 8-16 ms on unpromoted segments); the value itself was not compared with others.
- **Turn off:** n/a: a period (200 to 60000 ms). Maintenance itself is turned off with sift.async_maintenance = off. Change it in postgresql.conf and reload.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 2000 ms. The worker stays this long after it starts and holds a worker slot; table passes ask at most once per naptime. Range 200 to 60000; reload to change.

#### sift.enable_col_scan

`sift_col` · default · since 3.3 · default on

- **What it does:** a sift_col index with the sift.col_*_ops classes as a bitmap index scan that returns rows
- **Gain:** b-trees dropped, 1,000,000 rows, wall ms off / on: id = x 29.5 / 0.21, k = x (10 rows) 31.3 / 0.77, id BETWEEN 100 rows 32.8 / 0.31, id IN (100) 43.4 / 0.62, ts range 1,000 rows 40.6 / 2.9; b-trees present: the same plan in 24 of 26 shapes at 100,000 and 1,000,000 rows (count(*) WHERE v = x 0.38 ms SiftAgg, 0.46 ms bitmap, at 100,000)
- **Impact:** index size: one sift_col index 29.1 MB against 170.2 MB for six b-trees (1,000,000 rows, 7 columns); build, merge and writes: n/a: a planner switch for reads; it writes nothing; other: on: the planner prices the bitmap scan and the Index Scan against the b-trees and the sequential scan; with the b-trees present the plans are the b-tree's
- **When:** Planner paths on a sift_col index with the sift.col_*_ops classes: bitmap scan, Index Scan and Index Only Scan of rows (point lookups, IN, ranges, top-N), chosen by cost. SiftAgg aggregate scans do not depend on it.
- **Default:** on — On: a few count(*) shapes that SiftAgg answered take the bitmap scan by cost, up to 0.16 ms slower (1,000,000 rows, count(*) WHERE k = x 1.08 / 1.24 ms).
- **Turn off:** SET sift.enable_col_scan = off (session, ALTER ROLE/DATABASE, postgresql.conf): the planner builds no bitmap path on a sift_col index. Nothing stored changes.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: with the b-trees the planner keeps them; without them a point, range or IN lookup reads the index instead of the table. Turn it off to keep the planner from bitmap scans of a sift_col index.

#### sift.vis_page_cache

`sift_col` · default · since 3.4 · default on

- **What it does:** the rows of a heap page that a scan reads twice (the units of a key-cut index list rows of the same page) are checked for visibility once
- **Gain:** none at 100,000 rows: a key-cut range query 0.20 ms off, 0.23 ms on (within noise)
- **Impact:** index size: about 48 bytes for each heap page the scan visits (at most 1,048,576 pages), freed with the scan; build, merge and writes: n/a: a read-side cache of one scan; it writes nothing; other: about 48 bytes for each heap page the scan visits (at most 1,048,576 pages), freed with the scan; off checks the page again each time
- **When:** Scans of an index with key_cut, whose units list rows of the same heap page: the visibility of a page read twice is kept for the rest of the scan. Other scans read each page once.
- **Default:** on — On: it costs about 48 bytes for each heap page visited (at most 1,048,576 pages) and writes nothing; at 100,000 rows a key-cut range query showed 0.20 ms off, 0.23 ms on (within noise).
- **Turn off:** SET sift.vis_page_cache = off (session, ALTER ROLE/DATABASE, postgresql.conf): the page is checked again each time. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: a heap page the scan reads twice (the units of a key-cut index list rows of one page) is checked for visibility once. Turn it off only to compare.

#### sift.col_rollup_on_demand

`sift_col` · default · since 3.1 · default on

- **What it does:** the rebuild of missing rollups by the worker, and the worker that makes them
- **Gain:** every unit answers from the rollups 1.9 to 3.1 s after ALTER INDEX SET rollup (1,000,000 documents, 276 units)
- **Impact:** index size: -; build, merge and writes: background rewrites of segments (about 0.7 s for an 8 MB segment); the query itself writes nothing; other: background rewrites of segments; off leaves the units as they are until a merge or REINDEX
- **When:** A SiftAgg scan of exact units that lack their rollup, in this session. Not asked for: dirty units, minor 3 segments, units over the 4096-tuple cap, a hot standby; the index option rollup_on_demand switches one index.
- **Default:** on — On: every unit answers from the rollups 1.9 to 3.1 s after ALTER INDEX SET rollup (1,000,000 documents, 276 units); the query writes nothing, the worker rewrites about 0.7 s for an 8 MB segment.
- **Turn off:** SET sift.col_rollup_on_demand = off (session, ALTER ROLE/DATABASE, postgresql.conf): units stay as they are until a merge or REINDEX. Per index: rollup_on_demand = false.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on, per session; the index option rollup_on_demand switches one index. Off leaves units as they are until a merge or REINDEX. Members: col_rollup_worker.

#### sift.col_rollup_worker

`sift_col` · default · since 3.1 · default on

- **What it does:** the rebuild of missing rollups by the worker, and the worker that makes them
- **Gain:** as `sift.col_rollup_on_demand`
- **Impact:** index size: as `sift.col_rollup_on_demand`; build, merge and writes: as `sift.col_rollup_on_demand`; other: background rewrites of segments; off leaves the units as they are until a merge or REINDEX
- **When:** A queued rollup refresh request (see col_rollup_on_demand) starts the background worker, one per database, that rewrites the segment. With it off the requests wait in the queue.
- **Default:** on — On so that a request is served without a call: the first query after ALTER INDEX SET rollup is fully answered from rollups 1.9 to 3.1 s later; off keeps the queue for sift.col_rollup_refresh(index) in your session.
- **Turn off:** SET sift.col_rollup_worker = off (session, ALTER ROLE/DATABASE, postgresql.conf): requests stay queued until sift.col_rollup_refresh(index) serves them, e.g. on a schedule.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: a rollup refresh request starts the background worker. Off keeps requests queued until sift.col_rollup_refresh(index) serves them in your session, for a scheduled refresh.

#### sift.col_format_minor

`sift_col`, `sift_table` · default · since 3.1 · default 15

- **What it does:** the newest column format the segments written use; 6 keeps them readable by a 3.2 library, 4 by 3.1
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: the options that write a minor above the setting are ignored; a 3.2 library refuses segments of minor 7 and above
- **When:** Every segment a sift_col index or sift_table writes (build, promotion, merge, refresh, worker); options needing a higher minor are ignored; segments already written keep theirs. Wide leaves force their own minor.
- **Default:** 15 — 15, the newest minor 3.3 writes (3.2: 6), so cube_minmax, superunit, calendar cells, dictionaries read in place and key lists are on by default; a 3.2 library refuses minors above 6.
- **Turn off:** SET sift.col_format_minor = 6 (3.2), 4 (3.1) or 3 (3.0) before writing. It is superuser-only and workers follow only the server or database value (ALTER SYSTEM, ALTER DATABASE); see docs/UPGRADING.md section 4.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at the default (15; point_tid_columns and key_cut stamp 16 and 17 themselves). Lower to 6 before creating indexes a 3.2 library must read; options needing a higher minor are ignored.

#### sift.col_auto_merge

`sift_col` · default · since 3.2 · default on

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** 5% of 200,000 rows updated: grouped sum 10.8 to 0.24 ms (2.6 VACUUM only); answerable units 32% to 51% (wide rows), 33% to 83% (mixed), 1,000,000 rows
- **Impact:** index size: file up to twice its live size after a rewrite until later VACUUMs; live index +3% over VACUUM only after 10 cycles (200,000 rows); build, merge and writes: WAL +0.9% to +1.9% total with full-page images and a checkpoint per round, +0.0% without, against +12% to +28% rewriting at every VACUUM (1,000,000 rows); one rewrite 63 ms and 1.5 MB WAL at 200,000 rows; other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** A scan or insert on a table whose changed rows reach a thousandth of it (50 to 100,000 rows) asks the worker. Needs sift.async_maintenance; not a DELETE alone, autovacuum_enabled = false, a standby.
- **Default:** on — On since 3.3 (3.2: off): units answered from pre-aggregates 32% to 51% (wide rows) and 33% to 83% (mixed), a grouped sum 10.8 to 0.24 ms, for +0.9% to +1.9% total WAL, within the budget of col_auto_merge_wal_pct.
- **Turn off:** Server or database only, the worker reads no session value: ALTER SYSTEM SET sift.col_auto_merge = off and reload, or ALTER DATABASE .. SET. sift.col_maintain(index) still applies the rule from a job.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on (set for the server or database). Off stops the background vacuums and rewrites. Tuning: _dirty_pct, _fragmentation, _buffer_pct, _wal_pct, _max_active, _partition_kb, _max_workers.

#### sift.col_auto_merge_dirty_pct

`sift_col` · default · since 3.2 · default 30

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** as `sift.col_auto_merge`
- **Impact:** index size: as `sift.col_auto_merge`; build, merge and writes: as `sift.col_auto_merge`; other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** One reason of the rewrite rule: units of a sift_col index holding a dead row; such a unit answers from rows, not pre-aggregates, however few rows are dead. sift.col_maintain(index) uses it too. 1 to 100.
- **Default:** 30 — 30, with the fragmentation and buffer reasons: a 1% churn spread over the table left 0 of 50 units answering from pre-aggregates, the rewrite 25 of 25 (200,000 rows,).
- **Turn off:** n/a: a threshold. 100 never fires this reason (it is 'above'); the other reasons stay, and sift.col_auto_merge = off stops the worker. Server or database level.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 30: the rewrite runs when more than this percent of the units hold a dead row (such a unit answers from rows, not pre-aggregates). Lower it to rewrite sooner, at more WAL.

#### sift.col_auto_merge_fragmentation

`sift_col` · default · since 3.2 · default 2

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** as `sift.col_auto_merge`
- **Impact:** index size: as `sift.col_auto_merge`; build, merge and writes: as `sift.col_auto_merge`; other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** One reason of the rewrite rule: units listed per superblock they cover; each promotion of the write buffer adds a segment over the superblocks the churn touched. 1.1 to 1000.
- **Default:** 2 — 2.0, with the dirty and buffer reasons: promotions pile up overlay segments over the same superblocks and nothing was rewritten; the rule took 0 of 50 units to 25 of 25 from pre-aggregates ().
- **Turn off:** n/a: a threshold. Raise it (up to 1000) to rewrite later; sift.col_auto_merge = off stops the worker. Server or database level.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 2.0: the rewrite runs when the units listed reach this many times the superblocks they cover (each promotion adds a segment over the same superblocks). Lower it to rewrite sooner.

#### sift.col_auto_merge_buffer_pct

`sift_col` · default · since 3.2 · default 1

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** as `sift.col_auto_merge`
- **Impact:** index size: as `sift.col_auto_merge`; build, merge and writes: as `sift.col_auto_merge`; other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** One reason of the rewrite rule: percent of the index's rows in the write buffer, read as one in-memory unit per superblock on every query until promoted. 0.01 to 100.
- **Default:** 1 — 1.0: when the buffer is the only reason only the buffer is promoted (insert-only 2% per cycle, 12 cycles, 200,000 rows: 33.4 to 4.6 MB written, same query times), so firing early is cheap.
- **Turn off:** n/a: a threshold. 100 never fires it; lower it to promote the buffer sooner. sift.col_auto_merge = off stops the worker. Server or database level.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 1.0: the write buffer holding more than this percent of the rows is promoted (alone, without a whole-index rewrite when it is the only reason). Lower it to keep the buffer smaller.

#### sift.col_unit_floor

`sift_col` · default · since 3.4 · default 2048

- **What it does:** after churn the promotions of the write buffer land a few dozen rows in each superblock they touch, and every one of them is a unit that a scan pays for like a thousand rows: rows that reach 16 superblocks or more with under half a unit each are cut by TID range into units of this many rows instead, so a steady state of overlays has a few hundred units where it had tens of thousands (3,000,000 rows, 20% of the rows changed a month: 10,552 units to 721; 10,000,000 rows: 35,107 to 2,415)
- **Gain:** 60 days of the churn simulator (bench/churn), both arms on one build (integration at 6d9da73 and the change, PostgreSQL 18.6, x86-64): 3,000,000 rows, 20% of the rows changed a month: a grouped sum 64.5 to 24.6 ms, a float sum over a json path 29.4 to 11.4 ms, a key lookup 35.0 to 7.5 ms; 10,000,000 rows, 20%: 231 to 97 ms, 88.3 to 34.1 ms, 141.6 to 40.2 ms; 10,000,000 rows, 5% a month: 135.7 to 68.7 ms, 45.1 to 24.4 ms, 67.2 to 11.2 ms; the same rows after a forced merge and VACUUM answer in the same time (0.07 to 0.13 ms)
- **Impact:** index size: the dirty index 71.8 to 28.7 MB (3,000,000 rows, 20%), 241.8 to 101.4 MB (10,000,000, 20%) and 143.3 to 66.8 MB (10,000,000, 5%); after a merge of everything the index is the size it was without the setting (30.24 and 30.18 MB, 106.95 and 107.22 MB, 93.13 and 93.41 MB: -0.2% to +0.3%); build, merge and writes: total WAL 834.5 to 759.8 MB (3,000,000 rows, 20%), 2,706 to 2,469 MB (10,000,000, 20%) and 707 to 638 MB (10,000,000, 5%) over 60 days, the maintenance share 367 to 292, 1,142 to 905 and 315 to 246 MB; other: a segment cut by TID range is minor 17 (a 3.3 library refuses it, REINDEX with the setting at 14 or less writes the superblock cut); a scan that reads the newest rows of a large table reads the 2,048-row unit that holds them, not a unit of a few rows (ORDER BY ts DESC LIMIT 20: +0.3 ms at 3,000,000 rows and 20% a month, +0.1 ms at 10,000,000 and 20%, +0.4 ms at 10,000,000 and 5%)
- **When:** A plain sift_col index over a table of 16 superblocks (4,096 pages) or more whose promotions reach 16 superblocks or more with under half a unit's rows in each, and the tier merges of such overlays. Not a key_cut index, not a sift_table, not a rewrite of a partition, and not once a merge holds half a unit's rows in every superblock.
- **Default:** 2048 — It passes the 3.3 rule on the classes it is for (a grouped sum 2.0 to 2.6 times faster in the dirty state, a float sum 1.9 to 2.6 times, a key lookup 3.5 to 6.0 times, all over the 5%), grows no index (smaller while dirty, within 0.3% after a merge, under the 3%) and writes less WAL, not more; the one class it costs, the newest rows by an ORDER BY .. LIMIT, costs 0.1 to 0.4 ms
- **Turn off:** SET sift.col_unit_floor = 0 (or ALTER SYSTEM / ALTER DATABASE for the maintenance worker): a promotion writes a unit for each superblock it reaches, as 3.3 does.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 2048. Raise it for fewer, larger overlay units on a table whose churn is spread over the whole heap; 0 writes a unit for each superblock a promotion reaches, as 3.3 does.

#### sift.enable_agg

`sift_col`, `sift_table` · default · since 3.0 · default on

- **What it does:** GROUP BY aggregates run as Custom Scan (SiftAgg) over the column index
- **Gain:** GROUP BY of 1,000,000 rows, enable_agg off to on: 60.6 to 0.11 ms (totals), 320 to 0.74 ms (month totals), 207 to 30 ms (grouping sets)
- **Impact:** index size: -; build, merge and writes: -; other: none; off gives the plan PostgreSQL has without sift
- **When:** GROUP BY and aggregate queries (and top-N, which needs it) on tables with a sift_col index; a query shape the node cannot take keeps the normal plan whatever this says.
- **Default:** on — On: totals of 1,000,000 rows go from 60.6 to 0.11 ms; the planner still compares costs and keeps the normal plan where it is cheaper.
- **Turn off:** SET sift.enable_agg = off (session), ALTER ROLE/DATABASE .. SET, or postgresql.conf; effective at once, no REINDEX. It also stops sift.enable_topn.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off gives the plan PostgreSQL has without sift (to compare, or to isolate a wrong result); it also stops the top-N scan. Set it per statement, role or session.

#### sift.agg_left_lateral

`sift_col`, `sift_table` · default · since 3.3 · default on

- **What it does:** a LEFT JOIN LATERAL over jsonb_to_recordset or jsonb_array_elements of a list of the scanned row is answered by the scan: a row none of whose elements passes is one tuple with NULL element columns
- **Gain:** LEFT JOIN LATERAL over a row's list, three captured queries: lateral part 27.1 to 10.1, 33.4 to 6.8, 30.9 to 6.8 ms (small data, median of 3)
- **Impact:** index size: -; build, merge and writes: -; other: none; off keeps the normal plan of such a query (the way to compare it and to isolate a wrong result)
- **When:** LEFT JOIN LATERAL over jsonb_to_recordset or jsonb_array_elements of a list of the scanned row; a WHERE above the join that sees the NULL element tuple, or a lateral condition that may raise, keeps the normal plan.
- **Default:** on — On: lateral part 2.7x to 4.9x faster on three captured queries (27.1 to 10.1, 33.4 to 6.8, 30.9 to 6.8 ms); the pushdown is checked by test agg_left_lateral.
- **Turn off:** SET sift.agg_left_lateral = off (session), ALTER ROLE/DATABASE .. SET, or postgresql.conf; the statement then keeps the normal plan. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off keeps the normal plan for a LEFT JOIN LATERAL over a row's list (to compare, or to isolate a wrong result). A WHERE that sees the NULL element tuple keeps the normal plan anyway.

#### sift.agg_preagg

`sift_col`, `sift_table` · default · since 3.0 · default on

- **What it does:** units whose rows are all visible are answered from the pre-aggregates stored with them, the exact count/sum/min/max, and groups of units from their superunit summary
- **Gain:** 1,000,000 rows, preagg off to on: min/max per group 7.4 to 0.20 ms, month totals 7.7 to 0.74 ms; 3,000,000 rows month totals 12.7 to 0.65 ms
- **Impact:** index size: +1.1% to +1.2% of the index (cube, 2.4 kB per unit; 300,000 and 3,000,000 rows); build, merge and writes: serial build +3.9% CPU (3,000,000 rows, quiet window); with 2 workers inside the +-5% noise; other: none on the index; off scans every unit
- **When:** Units of a sift_col index whose rows are all visible, for count/sum/min/max, and groups of units through their superunit summary; units with invisible rows are scanned, and an index built with preagg = 'off' has no cube.
- **Default:** on — On by the 3.3 rule: min/max per group 7.4 to 0.20 ms (over 5%) at +1.1% to +1.2% of the index (under 3%) and +3.9% serial build CPU (under 5%).
- **Turn off:** SET sift.agg_preagg = off (session, role, database or postgresql.conf): every unit is scanned, the stored cube stays. ALTER INDEX .. SET (preagg = 'off') plus REINDEX stops storing it.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off scans every unit (comparison). The scan skips the rollup read after 3 units whose rollups do not pay. The reloption preagg = 'off' stops storing the cube.

#### sift.agg_preagg_distinct

`sift_col`, `sift_table` · default · since 3.3 · default on

- **What it does:** units whose rows are all visible are answered from the pre-aggregates stored with them, the exact count/sum/min/max, and groups of units from their superunit summary
- **Gain:** count(DISTINCT) of an 8-value column, 25,000-200,000 rows: 0.25-1.47 ms to 0.10-0.52 ms; a 40,000-value key, 1,000,000 rows: 6.6 ms, 4.8 ms with preagg off
- **Impact:** index size: as `sift.agg_preagg`; build, merge and writes: as `sift.agg_preagg`; other: none on the index; off scans every unit
- **When:** count(DISTINCT col) of a 32-bit or text column with a cube histogram, over units whose rows are all visible (others are scanned into the same set); needs sift.agg_preagg. Tens of thousands of values gain nothing.
- **Default:** on — On since 3.3: 8-value column 0.25-1.47 to 0.10-0.52 ms; the measured exception is a 40,000-value key (6.6 ms against 4.8 ms with preagg off), named in bench case count_distinct_hist.
- **Turn off:** SET sift.agg_preagg_distinct = off (session, role, database or postgresql.conf): distinct values are counted from the rows of every unit. No REINDEX; the stored histograms stay.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off counts distinct values from the rows of every unit. A key with tens of thousands of values gains nothing from the histograms (see the bench case); measure it with agg_preagg off.

#### sift.agg_dead_delta

`sift_col` · default · since 3.4 · default 15

- **What it does:** after churn most units of a table have a dead row (a VACUUM cleared its liveness bit) and were read by their rows although every page is all-visible: a unit with at most this percent of its rows dead is answered from its stored counts and sums minus its dead rows, one row at a time (counts, NULL counts, integer and numeric sums; a minimum, a maximum, count(DISTINCT), a float sum and the write buffer still scan); 3,000,000 rows, 20% of the rows changed a month: 84 to 721 of 721 units from their pre-aggregates
- **Gain:** 60 days of the churn simulator (bench/churn), both arms on one build (integration at 452cc9fb and the change, PostgreSQL 18.6, x86-64): grouped sum, dirty state: 300,000 rows 5.26 to 4.84 ms; 3,000,000 rows, 20% a month 21.6 to 15.0 ms (336 to 160 million instructions); 10,000,000 rows, 5% a month 69.2 to 42.8 ms (1,078 to 439 million); 10,000,000 rows, 20% a month 104.1 to 80.2 ms (1,214 to 821 million); a window over the newest days 3.73 to 3.35 ms (3,000,000 rows) and 4.60 to 3.70 ms (10,000,000, 5%); the same rows after a forced merge and VACUUM answer in 0.05 to 0.13 ms
- **Impact:** index size: none: the setting is read only, the index bytes are those the same state has with it off; build, merge and writes: none: 0 bytes of WAL written by the statements with it on (300,000 rows state, pg_current_wal_lsn before and after); other: no stored byte and no write; a unit with more dead rows than the limit, or a page that is not all-visible, is scanned as before; a dead row costs about 500 instructions against about 100 for a scanned row, so a unit over about 17% dead is cheaper to scan; the attempt is dropped for a statement whose first 8 units read for their dead rows were scanned anyway (a statement the setting cannot help: key lookup +1.3% instructions at 300,000 rows, +0.8% at 3,000,000, float sum +0%)
- **When:** A sift_col index with pre-aggregates (sift.agg_preagg), a unit with a dead row whose heap pages are all-visible and no more dead rows than the limit, for count, sum and avg of integer and numeric columns (grouped, ranged and by calendar cell). Not a minimum, a maximum, a distinct count, a float sum, a unit with exceptions, a superunit, a sift_table or the write buffer.
- **Default:** 15 — It passes the 3.3 rule on the class it is for (the grouped sum of a churned table, 8% to 38% faster in time and 24% to 59% in instructions, and the window over the newest days 7% to 20%), grows no index (0%) and writes nothing (0 bytes of WAL, under the 5%); the statements it cannot help run the same instructions or +1.3%
- **Turn off:** SET sift.agg_dead_delta = 0 (or ALTER SYSTEM / ALTER DATABASE): a dirty unit is read by its rows, as 3.3 does.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 15. Raise it only for a table whose dead rows are few and spread evenly; above about 17% dead a unit is cheaper to scan. 0 scans every dirty unit, as 3.3 does. A lone segment above dead_rewrite_pct (10) dead is rewritten by VACUUM before it gets there.

#### sift.agg_super

`sift_col`, `sift_table` · default · since 3.3 · default on

- **What it does:** units whose rows are all visible are answered from the pre-aggregates stored with them, the exact count/sum/min/max, and groups of units from their superunit summary
- **Gain:** GROUP BY a 20-value key, count and sum, 400,000 rows (391 units, groups of 64): 3.09 ms from the units to 0.22 ms from the superunit summaries
- **Impact:** index size: +1.0% of the index at the default superunit = 16 (200,000 rows, 53 units); +0.8% at 64, +1.3% to +1.5% at 4 to 8; build, merge and writes: insert, promotion, merge, UPDATE and VACUUM CPU inside the noise (reloption superunit); other: none on the index; off scans every unit
- **When:** Groups of exact units that have a superunit summary, which only units written with reloption superunit > 0 (default 16 since 3.3) have; older units and groups cut by a predicate read each unit.
- **Default:** on — On: 400,000 rows grouped by a 20-value key 3.09 to 0.22 ms at +1.0% of the index (under 3%) and insert, merge and VACUUM CPU inside the noise: the 3.3 rule.
- **Turn off:** SET sift.agg_super = off (session, role, database or postgresql.conf) reads each unit's own pre-aggregate. ALTER INDEX .. SET (superunit = 0) stops writing summaries; a merge or REINDEX applies it to older units.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off reads each unit's own pre-aggregate. Summaries exist only for units written with the reloption superunit > 0 (default 16 since 3.3); REINDEX or a merge writes them for older units.

#### sift.enable_topn

`sift_col`, `sift_table` · default · since 3.0 · default on

- **What it does:** ORDER BY .. LIMIT n from the units, bounded by their zones and sorted runs
- **Gain:** ORDER BY .. LIMIT 10 with a search, 1M / 10M rows: 7.1 to 2.4 ms / 191 to 19.5 ms; a page of rows with totals, 1,000,000 rows: 85.1 to 4.4 ms
- **Impact:** index size: -; build, merge and writes: -; other: none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns
- **When:** ORDER BY .. LIMIT n (with a search, or the page of rows with totals) from sift_col units; needs sift.enable_agg. The planner compares it with an ordered b-tree by cost, so the b-tree stays where it is cheaper.
- **Default:** on — On: top-10 with a search 7.1 to 2.4 ms (1M rows) and 191 to 19.5 ms (10M); page with totals 85.1 to 4.4 ms; the cost comparison leaves other cases to the normal plan.
- **Turn off:** SET sift.enable_topn = off (session, role, database or postgresql.conf): ORDER BY .. LIMIT n gets the normal plan, aggregates keep SiftAgg. sift.enable_agg = off stops both. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; the planner compares it with an ordered btree by cost. Off gives the normal plan for ORDER BY .. LIMIT n. Needs sift.enable_agg.

#### sift.topn_walk

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** ORDER BY .. LIMIT n from the units, bounded by their zones and sorted runs
- **Gain:** ORDER BY key LIMIT 10 or 1000, key in blocks of 3,000 to 12,000 rows, 3,996,000 rows: 2.3 to 6.4x faster; a random key LIMIT 10: 3.1 to 3.4x faster
- **Impact:** index size: as `sift.enable_topn`; build, merge and writes: as `sift.enable_topn`; other: none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns
- **When:** Top-N without a search whose sort column does not follow the table (blocks of values in no order, random). A column that follows the table (clustered, serial id) is not walked by zones, it stops early on them.
- **Default:** on — On: 2.3x to 6.4x on keys in blocks of 3,000-12,000 rows (3,996,000 rows) and 3.1x to 3.4x on a random key; a key that follows the table or has many ties reads the same units, so nothing is lost.
- **Turn off:** SET sift.topn_walk = off (session, role, database or postgresql.conf): the units are visited in table order. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. A key that follows the table, or has many ties and NULLs, reads the same units either way. Off visits the units in table order (comparison).

#### sift.topn_zone_keys

`sift_col`, `sift_table` · default · since 3.3 · default on

- **What it does:** ORDER BY .. LIMIT n from the units, bounded by their zones and sorted runs
- **Gain:** ORDER BY g DESC, id DESC LIMIT 10, 25,000 to 200,000 rows: units read 20 to 157 down to 1 to 2; 1,000,000 rows, 5 statuses: 5.7 ms vs 5.5 ms off (no time gain)
- **Impact:** index size: as `sift.enable_topn`; build, merge and writes: as `sift.enable_topn`; other: none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns
- **When:** Top-N walk with several sort keys where the first key ties between units (few distinct values, ORDER BY status, id LIMIT 10), decided by the next integer key's zone; a first key that separates units is unaffected.
- **Default:** on — On: units read fall from 20-157 to 1-2 (25,000-200,000 rows) with no index or write cost; at 1,000,000 rows with 5 statuses there is no time gain (5.7 against 5.5 ms off).
- **Turn off:** SET sift.topn_zone_keys = off (session, role, database or postgresql.conf): only the first key bounds the units, every unit that ties with the n-th row is read. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. It decides a tie of the first sort key between units by the zones of the later keys (a few distinct values in the first key). Off to compare.

#### sift.topn_runs

`sift_col`, `sift_table` · default · since 3.3 · default on

- **What it does:** ORDER BY .. LIMIT n from the units, bounded by their zones and sorted runs
- **Gain:** ORDER BY k LIMIT 100 with order_columns, 1,000,000 rows: 3.5 to 1.4 ms (runs off, on); 2x to 4x up to n of a few thousand, a btree 5x to 14x faster at n = 1000
- **Impact:** index size: +50.8% of a ten-column index with order_columns on every column (1,000,000 rows); heads +0.6% (three columns, 800,000 rows); build, merge and writes: heads vs runs only: serial build 1.61 vs 1.63 s (800,000 rows), merge of 500,000 rows 0.74 vs 0.66 s (+-10% noise); other: none; off gives the normal plan, topn_walk and topn_zone_keys narrow the walk, topn_runs needs order_columns
- **When:** ORDER BY k LIMIT n where k is in the order_columns reloption, on units written after it was set; n up to a few thousand (b-tree 5x-14x faster at 1000). A removing WHERE and write buffers are read as before.
- **Default:** on — On, and costs nothing without the option: it acts only on an index with order_columns (default none; the opt-in costs the bytes). With it, LIMIT 100 on 1,000,000 rows 3.5 to 1.4 ms.
- **Turn off:** SET sift.topn_runs = off (session, role, database or postgresql.conf): whole units are read, the stored runs stay. ALTER INDEX .. RESET (order_columns) plus REINDEX drops them.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; it acts only on an index with the reloption order_columns (default none). Name only columns sorted with LIMIT: an unordered column costs 2 bytes a row. Off reads whole units.

#### sift.agg_lookup_pushdown

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** the table of a lookup join loads only the rows its own conditions can match, and keeps its rows between queries (backend cache, shared memory)
- **Gain:** semi join of a group of 1,000 keys, 3,000,000 rows: generic lookup join 70.8 to 22.9 ms, byte table 24.8 to 5.5 ms; keyed by a dependent lookup (a CTE of ids joined to the table of their rows, 90,000 rows, 1,500,000 events): 1 key 83.6 to 1.1 ms, 1,000 keys 101.8 to 30.5 ms, 12,000 keys unchanged
- **Impact:** index size: -; build, merge and writes: -; other: memory: up to 256 MB a backend, and shared memory of 5% of shared_buffers by default; the memory limits stop a larger table from being loaded
- **When:** Joined lookup tables (IN (SELECT ..), attribute tables) with own column = value conditions served by a b-tree prefix or a selective condition; a set that is most of the table gains nothing (30.4 vs 31.3 ms).
- **Default:** on — On: group of 1,000 keys over 3,000,000 rows, generic lookup join 70.8 to 22.9 ms and byte table 24.8 to 5.5 ms; the worst case is a tie (100,000 keys 30.4 against 31.3 ms).
- **Turn off:** SET sift.agg_lookup_pushdown = off (session, role, database or postgresql.conf): the whole joined table is loaded. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off loads the whole joined table. Gains need a btree on the table's equality columns or a selective condition; a set that is most of the table gains nothing (100,000 keys: 30.4 vs 31.3 ms).

#### sift.agg_lookup_columns

`sift_col`, `sift_table` · default · since 3.4 · default on

- **What it does:** the rows of a lookup table are read from the leaves of a sift_col index of it that holds the columns the join reads, not from the heap
- **Gain:** a table of 200,000 rows loaded by the query (1,000,000 orders): 33.5 to 23.3 ms; the load of the fact-first `tpch_shipping_priority` case at 400,000 lineitem rows: 102 M to 41 M instructions
- **Impact:** index size: as `sift.agg_lookup_pushdown`; build, merge and writes: as `sift.agg_lookup_pushdown`; other: none: the same memory as the load from the heap
- **When:** A joined table with a sift_col index that holds its join columns (integer, date, timestamp, text, numeric of up to 18 digits) and no value its leaves cannot store; the first query after a write to the table, or any query when the lookup cache is off. A table cached from an earlier query is not loaded again.
- **Default:** on — On: the heap is read a tuple at a time (about 160 ns a row read); the leaves are decoded a unit at a time, and the conditions `column <op> constant` of the table are tested on them before a tuple is made. The result is the same rows.
- **Turn off:** SET sift.agg_lookup_columns = off (session, role, database or postgresql.conf): the heap is read. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Turn it off to load every lookup table from the heap.

#### sift.verdict_cache_kb

`sift_col`, `sift_table` · default · since 3.4 · default 256 kB

- **What it does:** a new backend plans the form of a statement that won (a shared CTE or its inline plan, the flattened form or the one as written) from the choice another backend made, not all of them
- **Gain:** first statement of a new backend, a 27 KB dashboard statement (21 CTEs, 16 lateral sub-selects) over 3,000,000 rows: median 128 ms planning to 11 ms (the normal plan 10 ms) once another backend has planned the statement; the first backend pays the same as without
- **Impact:** index size: -; build, merge and writes: -; other: one table for the server, whatever the number of backends: 56 bytes an entry and up to three entries a statement, 256 kB by default (1 MB at most); where it cannot be allocated the backend logs once and plans in full; the size is taken when the first backend allocates it (a restart to change it); a plan in full at every eighth read of an entry, every 16th, 32nd and 64th once the plans in full have confirmed it
- **When:** Statements that the auto(shared-cte-choice) choice helps, planned by a backend that has not seen the shape but another backend of the database has: a pool that resets its sessions, a new connection. The first backend after a start of the server plans in full; a table that was changed since (ANALYZE, a rewrite, a REINDEX) moves the key and is planned in full once.
- **Default:** 256 kB — On: 256 kB of shared memory, allocated by the first backend that needs it; a wrong verdict is a plan of the other variant of the same query, never other rows, and the plan of the variant must cost within a factor of 4 of the one the verdict was made from. The 3.3 rule needs no more (no index growth, no write cost).
- **Turn off:** SET sift.verdict_cache_kb = 0 (superuser; postgresql.conf for all backends): each backend then plans every statement in full once.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at the default. 0 turns the sharing off (each backend then plans every statement in full once); more room helps only a server with more than about 2,000 statements that read a shared CTE.

#### sift.agg_share_lookups

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** the table of a lookup join loads only the rows its own conditions can match, and keeps its rows between queries (backend cache, shared memory)
- **Gain:** as `sift.agg_lookup_pushdown`
- **Impact:** index size: as `sift.agg_lookup_pushdown`; build, merge and writes: as `sift.agg_lookup_pushdown`; other: memory: up to 256 MB a backend, and shared memory of 5% of shared_buffers by default; the memory limits stop a larger table from being loaded
- **When:** UNION ALL arms of one statement that join the same table with the same columns and conditions; arms with other tables or conditions load their own, and nothing is kept between statements (that is agg_lookup_cache_kb).
- **Default:** on — On: the table loads once instead of once per arm, and nothing outlives the statement; the switch alone has no timing recorded (case cte_series_arms).
- **Turn off:** SET sift.agg_share_lookups = off (session, role, database or postgresql.conf): each arm loads the table itself. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. UNION ALL arms of one statement that join the same table with the same columns and conditions load it once; nothing is kept between statements. Off loads it per arm.

#### sift.agg_semi_lut

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer or date leaf is answered from a byte table over the keys of the joined table
- **Gain:** IN or EXISTS over a group of 1,000 keys, 3,000,000 rows: 66.0 ms normal plan, 27.0 generic lookup join, 6.0 byte table; NOT EXISTS 85.2 to 10.4 ms
- **Impact:** index size: -; build, merge and writes: -; other: memory of that table, 4096 kB by default per scan and per worker; keys that span more keep the generic join
- **When:** Semi and anti joins (IN, EXISTS, NOT EXISTS) on an integer or date leaf whose key range fits sift.agg_semi_lut_size; NOT IN (SELECT ..) stays on the hashed subplan, other key types use the generic lookup join.
- **Default:** on — On: 1,000 keys over 3,000,000 rows 6.0 ms against 27.0 (generic lookup join) and 66.0 (normal plan); NOT EXISTS 85.2 to 10.4 ms; no index growth or write cost.
- **Turn off:** SET sift.agg_semi_lut = off (session, role, database or postgresql.conf): the generic lookup join runs, 3.5x-4.5x slower for 100-5,000 keys. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off uses the generic lookup join, 3.5x to 4.5x slower for groups of 100 to 5,000 keys. It applies to integer or date join keys whose range fits sift.agg_semi_lut_size.

#### sift.agg_param_set_size

`sift_col`, `sift_table` · default · since 3.2 · default 1000

- **What it does:** the planner costs a set filter (col = ANY($1)) in a generic plan against the aggregate scan with this many elements
- **Gain:** col = ANY($1) in a generic plan, 3,000,000 rows, 1,000 / 10,000 / 100,000 keys: 4.8 / 5.6 / 10.4 ms against 10.3 / 32.4 / 100.5 ms with the planner's guess
- **Impact:** index size: -; build, merge and writes: -; other: none; planning only (0 keeps the planner's guess of 10)
- **When:** Planning of col = ANY($1) with an array parameter in a generic plan, for a query that can run as SiftAgg; custom plans (they see the array), literal lists and other queries are costed as before.
- **Default:** 1000 — 1000 lies above the crossover of 100 to 1,000 members: at 1,000-5,000 the generic plan is 2.1x-5.5x faster; at 100 members it is 2.0 ms slower (5.0 against 3.0 ms).
- **Turn off:** SET sift.agg_param_set_size = 0 gives back the planner's own guess of 10 elements (the bitmap scan is kept); 10 suits sets of about 100 keys. Session, role, database or postgresql.conf.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 1000. A set of about 100 keys is 2.3 ms slower on the scan: use a custom plan or SET sift.agg_param_set_size = 10. 0 gives the planner's own guess (10) back.

#### sift.agg_par_min_rows

`sift_col`, `sift_table` · default · since 3.5 · default 80000

- **What it does:** a scan whose workers the planner priced for the rows it reads starts them only when the units it keeps (after the zones and the keys of its lookups) hold this many rows
- **Gain:** lookup joins PostgreSQL counts at a handful of rows: 353,000 rows (171 units kept): 793 ms without workers, 262 ms with 4; 101,000 rows of unnested documents (49 units kept): 990 ms without workers, 870 with 4; 300,000 rows of 900 bytes, 60 units kept (65,000 rows): 36.7 ms with the threshold, 58.1 ms with 4 workers (1.5 ms against 7.1 for the smallest scan); a 300,000-row lab with the statistics of the large catalog, medians of 3 with the floor off and at the default: 1 unit kept (1,100 rows) 17.5 ms with workers, 2.7 serial; 3 units 77 and 52; 10 units 120 and 99; 95 units (114,000 rows) 693 and 671; the 12,000 rows of the bench planned at 8 times their size (3 interleaved rounds of 15 runs, medians): 1.2 ms and 11.7 million user instructions of all processes at the default, 4.3 ms and 23.8 million with the floor off
- **Impact:** index size: -; build, merge and writes: -; other: none; one process for a scan of fewer rows (0 starts the workers whatever it keeps)
- **When:** Only a scan whose path with workers was priced for the rows it reads (the lookups are a CTE or sub-select of ids PostgreSQL has no statistics for, and the price of the scan with its join rows is a few milliseconds); the workers of every other scan start as they did.
- **Default:** 80000 — 65,000 rows (60 units kept) the workers were 1.5x slower (40 against 58 ms), 101,000 rows of unnested documents 1.15x faster (990 against 870 ms), 300,000 rows (276 units) 1.5x faster (493 against 328 ms).
- **Turn off:** SET sift.agg_par_min_rows = 0 starts the workers of those scans for any number of rows. Session, role, database or postgresql.conf.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 80,000 rows. 0 starts the workers for any scan the plan asks them for.

#### sift.agg_key_skew

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** the probes of a lookup join are priced by the rows of the most common keys, not the mean
- **Gain:** counted lookup join of 5 heavy keys that hold a third of 200,000 rows: normal plan 34 ms, scan 8.6 ms; a rare list takes the scan and is 5 to 8 ms slower
- **Impact:** index size: -; build, merge and writes: -; other: none; planning only
- **When:** Normal-plan pricing of lookup joins PostgreSQL can count (IN (SELECT ..), join on a list table) when the column is more than twice as uneven as a flat one; flat columns are priced by the mean. Planning only.
- **Default:** on — On: 5 heavy keys holding a third of 200,000 rows run 8.6 ms on the scan against 34 ms; the measured cost is a rare list that takes the scan and is 5 to 8 ms slower.
- **Turn off:** SET sift.agg_key_skew = off (session, role, database or postgresql.conf): probes are priced by the mean rows of a key. Planning only, no REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. It prices the normal plan by the most common keys when the column is more than twice as uneven as a flat one. Off prices by the mean rows of a key.

#### sift.agg_rollup_cost

`sift_col`, `sift_table` · default · since 3.2 · default 1

- **What it does:** a unit is answered from its rollup only when that costs less than scanning its elements
- **Gain:** GROUP BY kind with count, 13,800 elements in 1,440 rollup tuples: 14.6M instructions from the rollup, 7.0M from the scan (100,000 documents)
- **Impact:** index size: -; build, merge and writes: -; other: none; planning only (0 always, large never)
- **When:** Units that have a rollup (GROUP BY an element or map key): per unit, the rollup is read only when its tuples cost less than scanning the elements; units without a rollup are unaffected.
- **Default:** 1 — 1 prices a rollup tuple as one element scan: rollups of a tenth of the elements were slower than the scan (14.6M against 7.0M instructions for GROUP BY kind), so always-from-rollup was dropped.
- **Turn off:** SET sift.agg_rollup_cost = 0 answers every unit from its rollup (the older behavior, used by tests); a large value never does. Session, role, database or postgresql.conf; no REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 1 (the cost of a rollup tuple against an element scan). 0 always answers from the rollup (tests), large values never do. EXPLAIN ANALYZE shows Units Scanned By Cost.

#### sift.agg_dense

`sift_col`, `sift_table` · default · since 3.1 · default on

- **What it does:** one switch for each execution shortcut of the aggregate scan: dense chunks, one scan for aggregates over per-row aggregates, UNION ALL arms in one pass, key filters on lookup joins, exact count(DISTINCT) without a sort
- **Gain:** count(*) FILTER per flag over 387,593 elements: 34.0M to 10.9M instructions; GROUP BY kind with count and avg: 41.1M to 26.9M
- **Impact:** index size: -; build, merge and writes: -; other: none on the index; off gives the generic path with the same rows
- **When:** count, sum and avg (with or without FILTER, ungrouped or grouped by text columns) of integer or float columns or of array-of-object elements; other aggregates and shapes take the tuple path.
- **Default:** on — On: 34.0M to 10.9M instructions (count FILTER per flag, 387,593 elements) and 41.1M to 26.9M (count and avg); sums add in the same order, so rows do not change.
- **Turn off:** SET sift.agg_dense = off (session, role, database or postgresql.conf): the tuple path runs with the same rows. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; the sums add in the same order, so the rows do not change. Off runs the tuple path (to compare, or to isolate a wrong result). EXPLAIN ANALYZE shows Dense Chunks.

#### sift.agg_two_level

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** one switch for each execution shortcut of the aggregate scan: dense chunks, one scan for aggregates over per-row aggregates, UNION ALL arms in one pass, key filters on lookup joins, exact count(DISTINCT) without a sort
- **Gain:** UNION ALL arms grouped by a row id, 1,000,000 rows: 428 ms normal plan, 90 ms with the scan (case two_level_spend; the switch alone is not isolated)
- **Impact:** index size: as `sift.agg_dense`; build, merge and writes: as `sift.agg_dense`; other: none on the index; off gives the generic path with the same rows
- **When:** An aggregate over per-row aggregates (UNION ALL arms grouped by a row id), also with joined VALUES lists, sub-selects or LIMIT 1 lookups; other statements are unaffected.
- **Default:** on — On: 1,000,000 rows 428 ms normal plan to 90 ms with the scan (case two_level_spend); the switch alone is not isolated.
- **Turn off:** SET sift.agg_two_level = off (session, role, database or postgresql.conf): each arm keeps its own scan and the outer aggregate runs above them. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The arms and the outer aggregate run in one scan of the index. Off keeps each arm's own scan (to compare, or to isolate a wrong result).

#### sift.agg_dist_stream

`sift_col`, `sift_table` · default · since 3.3 · default on

- **What it does:** one switch for each execution shortcut of the aggregate scan: dense chunks, one scan for aggregates over per-row aggregates, UNION ALL arms in one pass, key filters on lookup joins, exact count(DISTINCT) without a sort
- **Gain:** as `sift.agg_dense`
- **Impact:** index size: as `sift.agg_dense`; build, merge and writes: as `sift.agg_dense`; other: none on the index; off gives the generic path with the same rows
- **When:** count(DISTINCT x) of an integer column whose superblocks hold little-overlapping ranges of x, on a source that gives zones; otherwise, or when the group table would spill, the plain scan runs.
- **Default:** on — On: the answer is the same, the sets stay within hash_mem where the plain scan would spill or sort, and a scan that does not pay costs one pass over the zone arrays; no timing recorded.
- **Turn off:** SET sift.agg_dist_stream = off (session, role, database or postgresql.conf) restores the plain scan. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; the scan decides at run time from the zones, else it is the plain scan after one pass over the zone arrays. Off restores the plain scan (comparison, or a way out).

#### sift.agg_fuse

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** one switch for each execution shortcut of the aggregate scan: dense chunks, one scan for aggregates over per-row aggregates, UNION ALL arms in one pass, key filters on lookup joins, exact count(DISTINCT) without a sort
- **Gain:** three UNION ALL arms over a lookup join, 100,000 rows: 18.7 ms arm by arm, 8.6 ms in one pass
- **Impact:** index size: as `sift.agg_dense`; build, merge and writes: as `sift.agg_dense`; other: none on the index; off gives the generic path with the same rows
- **When:** UNION ALL arms that read the same rows and join the same lookup table, also with different WHERE or aggregates; arms that join no lookup, or that the pre-aggregates answer, stay apart.
- **Default:** on — On: three arms over a lookup join, 100,000 rows, 18.7 ms arm by arm against 8.6 ms in one pass (2.2x); results are the same.
- **Turn off:** SET sift.agg_fuse = off (session, role, database or postgresql.conf): each arm runs alone. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Arms that read the same rows run in one pass; arms that join no lookup, or that the pre-aggregates answer, stay apart. Off runs each arm alone.

#### sift.agg_join_filter

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** one switch for each execution shortcut of the aggregate scan: dense chunks, one scan for aggregates over per-row aggregates, UNION ALL arms in one pass, key filters on lookup joins, exact count(DISTINCT) without a sort
- **Gain:** as `sift.agg_dense`
- **Impact:** index size: as `sift.agg_dense`; build, merge and writes: as `sift.agg_dense`; other: none on the index; off gives the generic path with the same rows
- **When:** Inner and semi lookup joins on an integer or date key leaf; other key types are not tested against the loaded keys before tuples are made.
- **Default:** on — On: units outside the keys' range are not read and the other rows are tested as a set, not through the combo cache (case keys_join_500k); no figure recorded for the switch.
- **Turn off:** SET sift.agg_join_filter = off (session, role, database or postgresql.conf): the join resolves through the combo cache (no Join Key Filters in EXPLAIN ANALYZE). No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. An inner or semi lookup join on an integer or date key tests the key against the loaded table's keys before tuples are made (zone pruning). Off to compare.

#### sift.agg_rows

`sift_col`, `sift_table` · default · since 3.2 · default on

- **What it does:** one switch for each execution shortcut of the aggregate scan: dense chunks, one scan for aggregates over per-row aggregates, UNION ALL arms in one pass, key filters on lookup joins, exact count(DISTINCT) without a sort
- **Gain:** count(DISTINCT id) over per-row aggregates joined to a spine, 109,000 rows, work_mem 4MB: 753 ms (sort and GroupAggregate) to 444 ms
- **Impact:** index size: as `sift.agg_dense`; build, merge and writes: as `sift.agg_dense`; other: none on the index; off gives the generic path with the same rows
- **When:** A grouped plan that reads SiftAgg nodes with an exact count(DISTINCT) of integers (outer count(DISTINCT id) over per-row aggregates joined to a spine); no GROUP BY, DISTINCT ON or aggregates: not used.
- **Default:** on — On: 109,000 rows at work_mem 4MB 753 to 444 ms; the set spills to a sort of narrow tuples past hash_mem. Float sum and avg add in arrival order.
- **Turn off:** SET sift.agg_rows = off (session, role, database or postgresql.conf) restores PostgreSQL's sort and GroupAggregate. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. It counts distinct integers of a SiftAgg plan's rows in a node of its own, spilling to a sort of narrow tuples past hash_mem. Off restores the sort; float sums add in arrival order.

#### sift.table_write_buffer_limit

`sift_table` · default · since 3.0 · default 4096 kB

- **What it does:** size of the row-format write buffer before it is closed and promoted into columns
- **Gain:** none: query 1.24 ms at 2 MB and at 8 MB (100,000 rows)
- **Impact:** index size: table 3.52 MB at 2 MB, 2.05 MB at 8 MB (-41.9%): fewer, larger segments (100,000 rows loaded in 20 statements); build, merge and writes: load in 20 statements 354 ms at 2 MB, 281 ms at 8 MB (-20.8%); other: rounded down to 2 MB; a row must fit one unless table_wide_rows is on
- **When:** Writes of a sift_table: size of the row-format write buffer before it is closed and promoted; whole 2 MB superblocks, range 2048 to 8192 kB. A row must fit one unless table_wide_rows is on. Segments written afterwards.
- **Default:** 4096 kB — 4096 kB: the middle of the range and the write_buffer_limit of a sift index; 100,000 rows loaded in 20 statements: 8 MB against 2 MB was 41.9% smaller and 20.8% faster to load, queries equal (1.24 ms).
- **Turn off:** n/a: a size, not a switch. RESET sift.table_write_buffer_limit returns 4096; 2048 kB is the smallest. Buffers already written keep their size.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 4096 kB (2048 to 8192, rounded down to 2 MB). A row has to fit one buffer unless table_wide_rows is on; a larger buffer means fewer, larger promotions. Applies to segments written afterwards.

#### sift.table_wide_rows

`sift_table` · default · since 3.3 · default on

- **What it does:** a row that does not fit a write buffer is written (up to 1 GB) instead of refused
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: 5 / 50 / 200 MB rows insert in 45 / 403 / 1376 ms; other: none for rows that fit; the buffer that takes a wide row is directory format 11, which a build that does not know it cannot write to
- **When:** Rows of a sift_table larger than one write buffer (up to 1 GB). Rows that fit keep the old layout and are not affected; the buffer taking a wide row is directory format 11.
- **Default:** on — On, new in 3.3: a row up to 1 GB is accepted instead of refused with 'row of sift_table is too large'; 5 / 50 / 200 MB rows insert in 45 / 403 / 1376 ms. Format 11 appears only in tables that take one.
- **Turn off:** SET sift.table_wide_rows = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf): such rows are refused again. Buffers already marked format 11 stay, and older builds still refuse to write to them.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: a row above the write buffer (up to 1 GB) is accepted. Rows that fit keep the old layout; the buffer that takes a wide row is format 11, which older builds refuse. Off refuses such rows.

#### sift.table_inline_promote

`sift_table` · default · since 3.0 · default on

- **What it does:** the writer promotes closed write buffers into columns while rows are inserted
- **Gain:** none: query 1.29 ms off, 1.42 ms on (within noise; 100,000 rows)
- **Impact:** index size: 0.0% (2.10 MB both; 100,000 rows); build, merge and writes: the writer that closes a buffer pays a 25-31 ms promotion (single-row INSERTs, 100,000 rows, 4 MB buffers); other: off leaves the promotion to VACUUM and autovacuum
- **When:** Writers of a sift_table: the session that closes a write buffer promotes it into columns while inserting. VACUUM and autovacuum always promote. Not affected: reads of already promoted segments.
- **Default:** on — On since 3.0 so closed buffers become columns as rows arrive; this run measured no query gain (1.29 off, 1.42 ms on, noise) and a 25-31 ms promotion in the statement that closes a buffer.
- **Turn off:** SET sift.table_inline_promote = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf): promotion is left to VACUUM and autovacuum. Autovacuum reads the server or database setting, not the writer's.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on so the writer promotes closed buffers as it inserts. Off leaves them to VACUUM and autovacuum; table_async_promote moves the work to a worker instead.

#### sift.table_dict_lookup

`sift_table` · default · since 3.3 · default on

- **What it does:** a test for a string reads a dictionary of 1,024 strings or more in place, without loading it whole (column format minor 14), where the orders of a segment take a quarter of it at most
- **Gain:** first statement of a new backend on a text column of 300,000 distinct 33-character strings (instructions of the backend): prefix test 154.6 M to 7.4 M, equality 137.6 M to 3.4 M. Steady state: the backend loads the dictionary whole and keeps it after about 250 statements (one load of 5 M), so the equality costs 0 M and the prefix 4 M in either form; during those statements the equality costs about 1 M, the compact form 0.4 M
- **Formula:** S 20.90x saving 147153k instructions, G 22.9%, W 3.0%, R 1: B 62.3 against C 8.2, automatic
- **Impact:** index size: +22.9% of the table (10,502,144 to 12,910,592 bytes, 300,000 rows, a uuid and a text column of distinct strings); nothing for a dictionary under 1,024 strings; the orders of a segment stop at 25% of it; build, merge and writes: load and VACUUM +3.0% instructions (11,233 M to 11,593 M, 300,000 rows); the order is sorted by the radix of the sort of ; other: the order is 4 bytes a string, +22.9% of a sift_table with a uuid and a text column of 300,000 distinct strings (the cap is 25% of the segment; a column of short repeated strings keeps the compact form); load and VACUUM +3.0% instructions
- **When:** Column segments of a sift_table written from now on, for a text or uuid column with a dictionary of 1,024 strings or more: string tests (equality, IN, prefix, range) read it in place. Column format minor 14.
- **Default:** on — Automatic where the orders of a segment (4 bytes a string) take 25% of it at most, which holds the growth under the ceiling of the default formula: S 20.9 on the first statement of a backend (every backend of a pool or a dashboard pays it), G 22.9%, W 3.0%, B 62 against C 8.2. At steady state the form costs the same as the compact one.
- **Turn off:** SET sift.table_dict_lookup = off (session): new segments return to the compact form; segments already written keep the order until a merge or VACUUM FULL rewrites them. No NOTICE: a sift_table writes its segments in promotion and VACUUM, not in a statement that could report them; sift.table_segments(table) gives their bytes and format.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Set off for a table whose segments are written far more than their strings are tested, or to keep them at 4 bytes a string less; new segments only.

#### sift.table_key_table

`sift_table` · default · since 3.5 · default on

- **What it does:** count, sum, min and max WHERE a key = x, IN, a range or GROUP BY the key (a foreign key or *_id column with more than 256 values in a unit) read one record of each segment, not its units: 0.068 ms at 900,000 rows against 0.577 ms (300 keys)
- **Gain:** count, sum, min, max WHERE key = x over 900,000 rows 0.577 to 0.068 ms, count(*) 0.395 to 0.064 ms, IN (3 keys) 0.835 to 0.069 ms, BETWEEN 1.201 to 0.069 ms, GROUP BY key 0.922 to 0.066 ms (300 keys, 1 session, warm); 300,000 rows 0.205 to 0.066 ms; user instructions of a statement at 900,000 rows (perf stat, warm, the statement's own, plan included): count(*) 4.42 to 0.54 million, count, sum, min, max 5.67 to 0.66 million, IN (3 keys) 9.33 to 0.57 million
- **Impact:** index size: +2.7% of the segments' bytes at 100,000 rows, +0.34% at 300,000, +0.27% at 900,000 (300 keys, two measures); a segment whose table would pass 3% has none or fewer measures; build, merge and writes: load and promotion of 100,000 rows +0.42% (200 and 3,000 keys), 20,000 rows inserted and promoted +0.41%, delete and VACUUM +0.05% and +0.08%, maintenance and VACUUM FULL +1.28% and +1.49% instructions; other: a table of at most 3% of the segment (+0.27% of the segments' bytes at 900,000 rows, +2.7% at 100,000); +0.4% instructions on a load and promotion, +1.5% on a rewrite
- **When:** WHERE a foreign key or *_id column (int2, int4, date) = x, IN, a range or GROUP BY, nothing else tested, count/sum/avg/min/max of the numeric, time and integer leaves the table keeps; every superblock of the segment plain (no tombstone, no write buffer row, no uncommitted row) and no dead row. Else the units are read.
- **Default:** on — The rule of 3.3: over 5% on the relevant class (0.58 to 0.07 ms at 900,000 rows), growth at most 3% of a segment by construction, write cost under 5% on every path measured; a table that does not fit is not written.
- **Turn off:** SET sift.table_key_table = off (or in postgresql.conf), then VACUUM FULL or the next merge; segments written before keep theirs and are still read.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Set off for a table that is written far more than it is read by key, or to keep the segments readable by 3.4. The setting applies to the segments written afterwards.

#### sift.table_bulk_groups

`sift_table` · default · since 3.3 · default on

- **What it does:** COPY, CREATE TABLE AS and REFRESH MATERIALIZED VIEW write straight into column groups: COPY of 200,000 rows CPU 0.52 to 0.39 s, WAL 44.2 to 1.1 MB
- **Gain:** COPY of 200,000 rows (13 columns): CPU 0.52 to 0.39 s; 20 loads of 2,000 rows: WAL 8.8 to 0.3 MB
- **Impact:** index size: relation 1.2 against 1.2 MB (200,000 rows); 20 loads of 2,000 rows 6.1 to 2.5 MB; build, merge and writes: COPY of 200,000 rows: WAL 44.2 to 1.1 MB; other: a table with a unique or exclusion index, a trigger or foreign key, or a published table keeps the write buffers
- **When:** COPY, CREATE TABLE AS, REFRESH MATERIALIZED VIEW and ALTER TABLE .. SET ACCESS METHOD into a sift_table. A table with a unique or exclusion index, trigger or foreign key, or a published table keeps write buffers.
- **Default:** on — On since 3.3: COPY of 200,000 rows CPU 0.52 to 0.39 s and WAL 44.2 to 1.1 MB, relation size equal (>= 5% gain, no growth); no format change, but a 3.0 library refuses tables with unfrozen groups.
- **Turn off:** SET sift.table_bulk_groups = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf): loads go through write buffers again. Groups already written stay.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. COPY, CREATE TABLE AS and REFRESH MATERIALIZED VIEW write groups directly; a table with a trigger, foreign key, unique or exclusion index, or publication keeps write buffers. Off keeps them.

#### sift.table_compression

`sift_table` · default · since 3.0 · default ''

- **What it does:** chunk compression of the column segments the table writes
- **Gain:** -
- **Impact:** index size: zstd:3 -4.7% of the table against auto (2.10 to 2.00 MB, 100,000 rows); none 30 MiB against 13 MiB with auto (1,000,000 rows); build, merge and writes: load in 20 statements 318 ms (auto), 263 ms (zstd:3): within noise; query equal (1.15 ms); other: the sift_col trade-off: zstd:3 -4.7% of the table, scans no slower in a small run (100,000 rows)
- **When:** Column segments a sift_table writes from now on (promotion, merge, bulk load): auto, none, lz4[:N], zstd[:N], encodings; per column 'auto; col=zstd:9'. Autovacuum uses the server setting. Not sift_col.
- **Default:** '' — Unset (auto): encodings plus lz4 or zstd only where the cost rule says the bytes repay the decode; none was 2x the bytes (30 against 13 MiB) and zstd:3 4.7% smaller with equal load time and query (100,000 rows).
- **Turn off:** SET sift.table_compression = 'none' for no compression (scans fastest at 2x bytes); RESET returns auto. Segments already written keep their codec until a merge or rewrite.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave unset (auto: encodings, plus lz4 or zstd only where the cost rule says the bytes repay the decode time). none scans fastest at 2x the bytes. Per column: 'auto; col=zstd:9'. New segments only.

#### sift.table_dead_rewrite_pct

`sift_table` · default · since 3.0 · default 30

- **What it does:** VACUUM rewrites a column segment alone above this percent of dead rows
- **Gain:** -
- **Impact:** index size: file near 1.4x the live rows instead of up to 2x at 30 against 50 (scattered deletes and updates); build, merge and writes: VACUUM about 15-20% more time at 30 against 50 (steady delete/update/insert); other: 100 never rewrites
- **When:** VACUUM, table_maintain and the auto defrag pass of a sift_table: a column segment alone is rewritten above n percent dead rows. A sift_col index has its own dead_rewrite_pct (10).
- **Default:** 30 — 30: the file stays near 1.4x the live rows instead of up to 2x (30 against 50, scattered deletes and updates) for about 15-20% more VACUUM time; a table keeps its segments until a third is dead.
- **Turn off:** SET sift.table_dead_rewrite_pct = 100 never rewrites a lone segment (session, ALTER ROLE / DATABASE .. SET, postgresql.conf); merges still drop dead rows they rewrite. RESET returns 30.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 30. Lower rewrites segments sooner (smaller file, more VACUUM work); 100 never rewrites. The auto defrag pass rewrites segments above this percent too.

#### sift.table_auto_defrag

`sift_table` · default · since 3.2 · default on

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** update/insert/delete mix, 4 clients, 200,000 rows: 7,536 against 5,092 tx/s, range read p99 16.8 against 34.2 ms
- **Impact:** index size: file 10.7 against 18.1 MB (200,000 rows); 19.2 to 8.2 MB after 12 cycles of 4% churn; build, merge and writes: WAL +60% on the mix (237 against 142-154 MB, 200,000 rows); append-only table starts no pass, same WAL per transaction; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** A sift_table that deletes, updates or reads past table_auto_defrag_dead_pct tombstones, or holds table_auto_defrag_free_pct free blocks; needs sift.async_maintenance. An append-only table starts no pass.
- **Default:** on — On since 3.3 (off in 3.2 as an opt-in until run on real workloads): 200,000 rows 7,536 against 5,092 tx/s, read p99 16.8 against 34.2 ms, file 10.7 against 18.1 MB, for about 60% more WAL on that mix.
- **Turn off:** SET sift.table_auto_defrag = off (server or database; postgresql.conf, ALTER DATABASE .. SET) restores the 3.2 VACUUM-only behaviour.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on (needs sift.async_maintenance). A worker clears dead rows and returns free space without VACUUM; off restores VACUUM-only (3.2). Bound it with budget, delay, wal_rate and max_active.

#### sift.table_auto_defrag_indexes

`sift_table` · default · since 3.2 · default on

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** as `sift.table_auto_defrag`
- **Impact:** index size: as `sift.table_auto_defrag`; build, merge and writes: as `sift.table_auto_defrag`; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** The background pass of a sift_table asked for by dead rows: a whole table VACUUM with index cleanup. A free-space request runs table_maintain, which clears no index entry.
- **Default:** on — On: without the index cleanup the tombstone log (which every scan reads and which holds the file above the live rows) cannot shrink; the log stays until a VACUUM when it is off.
- **Turn off:** SET sift.table_auto_defrag_indexes = off (server or database): the pass is table_maintain - dead rows cleared, space returned, tombstones and index entries wait for a VACUUM.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: the pass is a whole VACUUM, index cleanup included, so the tombstone log shrinks. Off: it only clears dead rows and returns space; tombstones and index entries wait for a VACUUM.

#### sift.table_auto_defrag_free_pct

`sift_table` · default · since 3.2 · default 25

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** as `sift.table_auto_defrag`
- **Impact:** index size: as `sift.table_auto_defrag`; build, merge and writes: as `sift.table_auto_defrag`; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** A sift_table with a file whose free blocks are n percent or more and that VACUUM's lock rule accepts (1000 blocks or a 16th of the file); the pass then runs table_maintain. Skipped while the table is being written.
- **Default:** 25 — 25: a quarter of the file, the 3.2 value; it is the second trigger beside dead_pct, tested only as a part of the pass (test 017_tbl_autodefrag), no other value measured.
- **Turn off:** n/a: a threshold (1 to 100). table_auto_defrag = off stops the whole pass; RESET returns 25.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 25. The pass also starts when this percent of the file is free blocks (and VACUUM's lock rule holds: 1000 blocks or a 16th of the file). Lower returns space sooner.

#### sift.table_auto_defrag_dead_pct

`sift_table` · default · since 3.3 · default 5

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** read after 16% churn 35.8 ms at 20, 7.6 ms at 5, 6.7 ms at 2 (200,000 rows; 0.5 ms fresh)
- **Impact:** index size: as `sift.table_auto_defrag`; build, merge and writes: mix at 20 / 5: 3,206 / 3,452 tx/s, WAL 164 / 157 MB (200,000 rows, 4 clients); churn WAL doubles 11.8 to 24 MB at 5; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** Backends of a sift_table asking for a pass when n percent of its rows are tombstones older than every snapshot (a long transaction makes it wait). The pass rewrites segments above table_dead_rewrite_pct.
- **Default:** 5 — 5, was 20 in 3.2 (autovacuum's factor): a read after 16% churn took 35.8 ms at 20, 7.6 at 5 and 6.7 at 2 (0.5 fresh); mix 3,206 against 3,452 tx/s and WAL 164 against 157 MB, churn WAL doubled (11.8 to 24 MB).
- **Turn off:** SET sift.table_auto_defrag_dead_pct = 100 never asks for dead rows (the free-space trigger remains); table_auto_defrag = off stops the pass. RESET returns 5. Server or database level.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 5. The pass is asked for when this percent of rows are tombstones older than every snapshot; 100 never asks. Lower starts sooner (more WAL); higher leaves reads slow until VACUUM.

#### sift.table_auto_defrag_coalesce

`sift_table` · default · since 3.3 · default on

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** after 6 cycles of churn: 2 groups against 4, aggregate 20-21 against 28-32 ms, range scan 9.5-14 against 14-18 ms; none after 12 cycles (200,000 rows)
- **Impact:** index size: file 8.7 against 10.1 MB after 6 cycles (200,000 rows); build, merge and writes: WAL 50.3 against 49.3 MB (6 cycles, 200,000 rows); other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** The pass of a sift_table: the small column groups next to a rewritten one are merged into one group. The gain fades after about 12 cycles of churn.
- **Default:** on — On: after 6 cycles of churn 2 groups against 4, aggregate 20-21 against 28-32 ms, file 8.7 against 10.1 MB, for WAL 50.3 against 49.3 MB (200,000 rows).
- **Turn off:** SET sift.table_auto_defrag_coalesce = off (server or database): the pass rewrites only the segments above the threshold and leaves their neighbours as they are.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The pass merges the small groups next to the one it rewrites, so fewer groups remain; the gain fades after about 12 cycles of churn.

#### sift.agg_scale_horizon

`sift_col`, `sift_table` · default · since 3.3 · default 100

- **What it does:** keeps an inline shared CTE that is a close call only if it also wins at the square root of this many times the rows and at this many times (sift.plan_scale)
- **Impact:** index size: -; build, merge and writes: -; other: none at 1; two more plans of the statement where the scans cost less than the shared work but not by the margin
- **When:** Only an inline shared CTE that is a close call (scans cost less than the shared work, not by the margin); it is planned again at the square root of the horizon and at the horizon times the rows.
- **Turn off:** SET sift.agg_scale_horizon = 1 (minimum; session, role, database or postgresql.conf) keeps the choice at the size the table has, with no further planning.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 100: a close call between an inline and a shared CTE must also hold at 10 and at 100 times the rows. 1 keeps the choice at the current size; values around the default change few plans.

#### auto(col-append-batch)

`sift_col` · default · since 3.6 · default automatic

- **What it does:** The rows a transaction inserts into the index after its first wait in the backend (up to a page of records for each index) and go to the write buffer as one WAL record for the run that fits the tail page, before the commit record; COPY and a statement of many rows write a record a page, not a record a row
- **Gain:** COPY of 50,000 rows into a table with a 6-column index, 1,000,000 rows (PostgreSQL 18.6, x86-64): WAL 245 to 186 bytes a row (-24%), backend 23,022 to 20,571 instructions a row (-10.6%), standby replay 6,205 to 3,390 (-45%); a statement of 100 rows 293 to 234 bytes (-20%), backend -2.4%, replay -31%
- **Impact:** index size: 0% (the same bytes in the write buffer); build, merge and writes: one row a transaction unchanged: 166,693 to 166,846 instructions (+0.1%), 353 bytes, 11,085 to 11,075 on the standby; the index adds 47 bytes a row of COPY WAL (106 before) over the table with only a primary key; other: No index size effect (the buffer holds the same bytes). The rows are written when the transaction commits, reads the index or starts a savepoint, so a statement that reads the index right after its inserts flushes first; the rows of a savepoint that rolls back are never written; an ON CONFLICT statement and a row of more than a page go in at once; a write buffer can pass write_buffer_limit by the rows of one page before it seals; at most 8 indexes of a transaction are buffered, the others go in at once; the first row of a transaction goes in at once, so one row a transaction is unchanged
- **When:** Every transaction that inserts more than one row into a sift_col index (COPY, INSERT with many rows or SELECT, many statements in a transaction). Not a transaction of one row, an ON CONFLICT statement, or a row of more than a page.
- **Default:** automatic — The WAL of the index is the largest cost of a bulk load on a server with a standby: the standby replays one record a page, not one a row (-45% of its replay instructions for COPY), with the rows of a transaction visible to its own reads and a savepoint that rolls back writing nothing; measured at 1,000,000 rows (the PR).
- **Turn off:** n/a: no setting; the rows of a one-row transaction, an ON CONFLICT statement and a row of more than a page are written at once, the others wait for the commit, a read of the index or a savepoint.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave as is; there is nothing to set. Load with COPY or statements of many rows to write the fewest records; one row a transaction writes what it did.

#### auto(lateral-decorrelation)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** A LATERAL or scalar sub-select that aggregates one table tied to the outer query by equalities (also one that groups, and the comma form FROM x, LATERAL) is planned as that table grouped once and joined to the outer query, not as an index lookup for each outer row
- **Gain:** 1,000,000 orders, 2,000 outer rows: lateral count and sum 470 to 13.3 ms (35x), lateral that groups 598 to 29.8 ms (20x); scalar sub-select, 8,000 outer rows, 2,161 to 19.7 ms ()
- **Impact:** index size: -; build, merge and writes: -; other: No index, write or WAL effect. A statement of the shape is planned twice (grouped and as written), about 2-3 ms more on star queries (4.0 against 1.2 ms for a selective one); a backend keeps the choice (auto(shared-cte-choice)). Kept only when the grouped query runs in the scan.
- **When:** A LATERAL or scalar sub-select of count, sum, avg, min, max, array_agg, bool_and, bool_or or string_agg (optional GROUP BY and HAVING) over one table with a column index, tied to the outer query by equalities of the same type, with many outer rows. Not with LIMIT, ORDER BY, grouping sets, joins inside, inequalities, a HAVING without GROUP BY or an outer column in a group: the sub-select stays as written.
- **Default:** automatic — Cost-based: the grouped form is kept only when it runs in the scan and the planner costs it below the lookups of the plain plan, so a few outer rows keep the index lookups. Same rows as the sub-select (fuzz family decor: 0 divergent).
- **Turn off:** SET sift.enable_agg = off (the whole aggregate scan; the query is planned as written). There is no setting for this rewrite alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. With few outer rows (19 in one test) the lookups stay. sift.explain_why shows the rewrite as the arm used; write the tie as equalities of one column on each side.

#### auto(key-cut-second-key)

`sift_col` · default · since 3.6 · default automatic

- **What it does:** When key_cut is chosen by default and the index has a date or timestamp leaf, the rows of a key are sorted by that leaf too: a key with more rows than a unit is split by date, a window of dates on the key skips the units outside it, and the sorted runs and addresses of the rows of a unit encode in fewer bytes
- **Gain:** 3,000,000 rows, 6,000 keys, dates scattered over 15 years, one segment: the wildcard aggregate of a jsonb map over a 2-year window 760.6 to 722.9 M instructions (-5.0%), over 90 days 536.3 to 498.2 M (-7.1%), count and sum over 2 years -8.0%, key and month aggregate -3.7%; 125,000 rows, 20 keys: a key and a 2-month window reads 2 units against 9
- **Formula:** S 1.08x saving 38100k instructions, G -20.9%, W 11.0%, R 1: B 1.5 against C -4.8, automatic
- **Impact:** index size: -20.9% (3,000,000 rows: 76.9 to 60.8 MB; 1,000,000 rows -21%): the dates of a unit span a narrower range; build, merge and writes: build 12.3 to 13.7 s (+11%) at 3,000,000 rows (-7% at 2,000,000), merge 8.1 to 8.3 s, no WAL or insert change; other: CREATE INDEX or REINDEX sorts the rows by two leaves, not one (build +11% at 3,000,000 rows, -7% at 2,000,000; merge 8.1 to 8.3 s); the sample prices one more layout (a few ms); no structure is added
- **When:** A CREATE INDEX or REINDEX where key_cut is chosen (or advised) for a leaf that is not a time leaf, on an index that has a date or timestamp leaf, when the sample predicts the index no more than 1% larger than the cut by the key alone. Not with a time leaf as the key, key_cut = 'off' or a list, order_columns or point_tid_columns.
- **Default:** automatic — The sorted order costs nothing the 3.3 rule counts: the index shrinks and the build pays 11% (W 11 gives C = -20.9/3 + 11/5 = -4.8 against B = 1.4). Float8 sums add a key's units in another order, as a parallel plan adds rows; integers and numeric are exact.
- **Turn off:** ALTER INDEX .. SET (key_cut = 'key') and REINDEX, or CREATE INDEX .. WITH (key_cut = 'key'); the NOTICE names the choice.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The choice is written as key_cut = 'auto: key, date' and the NOTICE names both; key_cut = 'off', or a list of your own (say 'key, date' for an index whose cut you set before), is never changed. sift.key_cut_decision says what a REINDEX would decide.

#### auto(key-cut-range-lane)

`sift_col` · default · since 3.6 · default automatic

- **What it does:** When key_cut is chosen by default, the start of the set of a range column and the node of an inet or cidr column (their first lane, r#1 or ip#1) are candidates like a date: the units are cut by the lane, so @>, <@, &&, <<, >>, -|- of a range and <<=, >>= of a network read the few units whose lane meets the constant
- **Gain:** 3,000,000 random int4range rows beside an address and a 128-character note, sift_col (r, ip), one table with and without the cut: r @> x 27.9 M instructions without the cut and 0.67 M with it; r && range 28.3 M and 0.87 M; ip <<= net 25.6 M and 23.8 M; ip >>= addr 95.7 M and 93.9 M; 1,000,000 rows r @> x 9.7 M and 0.42 M (a GiST 0.82 M, 0.138 ms); int4range @> x, 56-byte rows, without the cut and with it (GiST): 500,000 rows (23 units) 4.4 M and 1.0 M instructions (0.4 M), 1,000,000 rows 8.4 M and 1.4 M (0.4 M), 3,000,000 rows 24.7 M and 0.6 M (0.5 M), the index 55.1 to 48.8 MB (GiST 132.8 MB); the formula cuts at every size, there is no minimum of units
- **Formula:** S 42.00x saving 27200k instructions, G -6.0%, W 58.8%, R 0.5: B 38.3 against C 9.8, automatic
- **Impact:** index size: -6.0% (3,000,000 rows: 66.5 to 62.5 MB after the merge and VACUUM; 1,000,000 rows 21.6 to 20.2 MB, -6.5%); the sample predicts +2.9%; build, merge and writes: CREATE INDEX 20.4 to 32.6 G instructions (+59.6%, 3,000,000 rows, one process); inserts, promotion and merge of the cut as for any key_cut; other: The cut lists the TIDs of its rows (the cost every key_cut has) but the lanes of a unit sort and encode in fewer bytes: the index is 6.0% smaller at 3,000,000 rows (66.5 to 62.5 MB); CREATE INDEX +58.8% instructions (20.4 to 32.4 G, one process) for the sample and the sort; the other leaves of the index keep their zones (the cut is not chosen when one would lose them), and an address column with no zones reads 1% to 9% fewer instructions
- **When:** A CREATE INDEX or REINDEX on a table of 64 units or more, whose range or inet lane is scattered over the units (a range of it reads 8 times the units its rows fill), whose other leaves are scattered too (their zones keep all the units for an equality: a leaf that follows the table, or whose zones skip units, blocks the cut), when the sample predicts the index no more than 3% larger. Not with key_cut = 'off' or a list, order_columns or point_tid_columns.
- **Default:** automatic — B = R*log2(S)/log2(1.05) = 0.5*log2(42)/log2(1.05) = 38.3 against C = G/3 + W/5 = -2.0 + 11.8 = 9.8: automatic; G is under the 25% ceiling and the saving is 27.2 M instructions a query. Rare columns (R 0.5).
- **Turn off:** ALTER INDEX .. SET (key_cut = 'off') and REINDEX, or CREATE INDEX .. WITH (key_cut = 'off'); the NOTICE names the choice.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The choice is written as key_cut = 'auto: r#1' and the NOTICE names it; key_cut = 'off', or a list of your own, is never changed. sift.key_cut_decision says what a REINDEX would decide. A range column in the order of the table, or another leaf that follows it (a sequential id) or whose zones skip units (a date that drifts over the table), blocks the cut: the units stay by superblock.

#### auto(key-cut-date-growth)

`sift_col` · default · since 3.6 · default automatic

- **What it does:** When key_cut is chosen by default for a date or timestamp column (or a lane), the benefit and the cost of the default formula decide the cut, not a ceiling on the growth of the index (it was 25%, 3% for a key): the rows of a day sit in few units whatever the order of the heap, so a window of dates, an equality on a date and a group by day read the units of their range, on a table of any number of units
- **Gain:** 3,000,000 rows scattered over 1,000 days, sift_col (d), warm instructions of one statement, without the cut and with it (b-tree): 56-byte rows: count(*) of a day 25.4 M and 0.7 M (1.6 M), of a week 25.6 M and 0.9 M (10.4 M), group by day 88.7 M and 3.8 M (1,885 M); 140-byte rows: a day 53.7 M and 0.7 M, a week 54.0 M and 0.9 M, group by day 91.7 M and 3.7 M; 190-byte rows in the order of a scraper: a day 22.0 M and 0.7 M (0.9 M), group by day 94.2 M and 7.1 M (1,889 M); 1,000,000 rows of 56 bytes (45 units) a day 8.8 M and 0.4 M (0.6 M), a week 8.9 M and 0.5 M (3.5 M); 500,000 rows of 56 bytes (23 units) a day 4.5 M and 0.3 M (0.4 M), a group by day 16.3 M and 2.2 M (315.7 M)
- **Formula:** S 34.00x saving 24700k instructions, G 44.5%, W 67.0%, R 1: B 72.3 against C 28.2, automatic
- **Impact:** index size: +46.6% (56-byte rows), +55.0% (140), +1.9% (190) measured, 3,000,000 rows, parallel build; the sample predicts +44.5%, +44.7% and -1.3%; an index of several columns grows less; build, merge and writes: CREATE INDEX at 3,000,000 rows, instructions a row and seconds (one process; the default parallel build in brackets), without the cut, the cut alone and the default (cut and the sample that decides it): 56-byte rows 1,776 and 0.8 s (1,769 and 0.5 s), 2,965 and 2.5 s (2,848 and 1.4 s), 3,485 and 2.9 s (3,371 and 1.7 s); 190-byte rows 1,919 and 1.2 s (1,913 and 0.7 s), 2,877 and 2.2 s (2,819 and 1.3 s), 3,154 and 2.2 s (3,100 and 1.4 s). The cut is the sort of the rows by the date and the writing of the addresses (+1,189 and +958 instructions a row), the decision the first look of 8 superblocks and the sample of 32 (+520 and +277 a row); a table under 32 superblocks is sampled whole and costs less. key_cut = 'off' keeps the build of a table without the cut. Inserts, promotion and merge as for any key_cut; other: The cut lists the TIDs of its rows, but the date sorts and encodes in fewer bytes: 3,000,000 rows of 56 bytes 4.87 to 7.14 MB (+46.6%; the sample predicts +44.5%; a b-tree 21.2 MB), of 140 bytes 5.41 to 8.38 MB (+55.0%, predicted +44.7%), of 190 bytes 5.27 to 5.37 MB (+1.9%, predicted -1.3%)
- **When:** A CREATE INDEX or REINDEX whose date, timestamp or lane is scattered over the units (a range of it reads more units than its rows fill, and the cut saves 100,000 instructions or more a query), whose other leaves are scattered too, when the benefit of the cut is at least its cost. Not with key_cut = 'off' or a list, order_columns or point_tid_columns.
- **Default:** automatic — B = R*log2(S)/log2(1.05) = 1*log2(34)/log2(1.05) = 72 against C = G/3 + W/5 = 14.8 + 13.4 = 28: automatic (3,000,000 rows of 56 bytes, G predicted +44.5%); the saving is 24.7 M instructions a query.
- **Turn off:** ALTER INDEX .. SET (key_cut = 'off') and REINDEX, or CREATE INDEX .. WITH (key_cut = 'off'); the NOTICE names the choice.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The choice is written as key_cut = 'auto: d' and the NOTICE names it; key_cut = 'off' keeps the units by superblock. sift.key_cut_decision(index) says what a REINDEX would decide. A date that follows the table, another leaf that follows it, order_columns or point_tid_columns keep the cut off.

#### auto(point-directory-columns)

`sift_col` · default · since 3.6 · default automatic

- **What it does:** When point_columns and point_tid_columns are not set, CREATE INDEX and REINDEX write to it (point_tid_columns), as 'auto: col', the integer columns of nearly unique values (distinct values at least half the rows) that do not follow the order of the table (correlation under 0.9), when the benefit of the directory is at least its cost: an equality on such a column reads every unit, because the keys of a unit span the whole range of the column, and the directory of heap addresses answers it in a flat 0.2 M instructions, an IN list of 16 keys in 0.4 M (a b-tree 0.5 M; the directory of rows, point_columns, 0.9 M)
- **Gain:** an equality on a random unique int8 of a 7-column index, 400,000 rows: 1.9 M instructions without the directory and 0.2 M with it (a b-tree 0.2 M); one int8 column, 1,000,000 rows: 5.17 M and 0.20 M (b-tree 0.14 M), 3,000,000 rows about 15 M and 0.2 M; an IN list of 16 keys: 95.6 M, 0.74 M (b-tree 0.50 M) at 1,000,000 rows
- **Formula:** S 9.50x saving 1700k instructions, G 96.0%, W 21.0%, R 1: B 46.1 against C 36.2, automatic
- **Impact:** index size: +96% measured (6.50 to 12.74 MB; predicted +105%), 400,000 rows, 7 columns of 62, 10 and 10 bits; 16.5 bytes a row more for a column of 1,000,000 rows (8.3 to 25.4 MB for one int8 column, a b-tree 22.5 MB); build, merge and writes: CREATE INDEX +21% instructions (1,756 to 2,126 a row, one int8 column, 1,000,000 rows; 1,719 to 2,092 for an int4); the default parallel build +21%; a table without statistics gets no choice until a REINDEX after ANALYZE; other: 16.5 bytes a row for each column chosen (6.50 to 12.74 MB, +96%, for a 7-column index of 400,000 rows; the sample-free estimate said +105%); CREATE INDEX +21% instructions (1,756 to 2,126 a row for one int8 column, 1,000,000 rows)
- **When:** A CREATE INDEX or REINDEX of an analyzed table whose integer column has nearly unique values, is not in heap order and whose benefit is at least its cost, when point_columns and point_tid_columns are unset (or 'auto'). Not with sift.col_format_minor under 15.
- **Default:** automatic — B = R*log2(S)/log2(1.05) = 1*log2(9.5)/log2(1.05) = 46 against C = G/3 + W/5 = 32 + 4.2 = 36: automatic; the saving is 1.7 M instructions a lookup. S is (0.17 M + 5 instructions a row) over the 0.2 M of the directory; G is 16.5 bytes a row over the bytes a row of the index without it, estimated from the statistics.
- **Turn off:** ALTER INDEX .. SET (point_tid_columns = 'off') and REINDEX; the NOTICE names the choice.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The choice is written as point_tid_columns = 'auto: rid' and the NOTICE names it with the benefit and the cost; point_tid_columns = 'off' keeps the index without a directory; a list you set is never changed. A column that follows the table, or has few values, or a table that is small for it, gets none (1,000,000 rows of one int8 column: B 66 against C 71, so a one-column index of that size stays without; 3,000,000 rows: B 88 against C 71, chosen).

#### auto(top-n-per-group)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** The first N rows of each group asked with row_number() OVER (PARTITION BY .. ORDER BY ..) cut at rn <= N (N > 1, over a sub-select or a CTE read once) are planned as one grouped scan: an ordered array_agg per column cut at N elements and unnested with the ordinal, not a sort of every row
- **Gain:** 3 columns, N = 3, one month: 321 to 176 ms and 213 to 171 ms; a year, scan forced: 4,578 to 960 ms. Bench window_first_rows (1,000,000 rows) keeps the window: 51.4 ms, the forced scan 58.0 ms
- **Impact:** index size: -; build, merge and writes: -; other: No index, write or WAL effect. Planned twice (window and arrays), the cheaper kept. The arrays hold every row of a group before the cut, so memory is that of the groups: the window stays when they do not fit hash memory (work_mem x hash_mem_multiplier).
- **When:** row_number() OVER (PARTITION BY .. ORDER BY ..) as the only window function, kept by rn <= N, rn < N + 1 or N >= rn with N > 1, over a table with a column index (no sublinks, array or row columns, at least one non-key column). Groups that do not fit hash memory keep the window. N = 1 is the DISTINCT ON rewrite.
- **Default:** automatic — Cost-based: both plans are built and the cheaper kept (the window when the scan is dearer or a small work_mem). Same rows as the window, ties arbitrary as in row_number() (fuzz families latest and decor: 0 divergent).
- **Turn off:** SET sift.enable_agg = off (the whole aggregate scan). There is no setting for this rewrite alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; the window is kept when the scan costs more. The cost model rates the scan too dear for a year of rows (4-5x faster in a test): SET sift.agg_force = on shows it. sift.explain_why names window_first for a window left as written.

#### auto(ordered-groups)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** The groups of an aggregate scan come out in the order the query's ORDER BY or window on their keys asks for (also over a sub-select or inlined CTE that the window reads), so no Sort is put above the scan: the distinct values of each key are sorted once with the pathkey's comparator and the groups by their ranks
- **Gain:** 40,586 groups, window over a CTE of the scan, 1,000,000 rows: 645 to 514 ms with 4 workers, 800 to 705 ms in one process; bench group_order_window
- **Impact:** index size: -; build, merge and writes: -; other: No index, write or WAL effect. A second path is planned for each order the query asks (about 0.1 ms); the rank arrays take 4 bytes for each distinct value of a key and 4 bytes for each group and key, freed after the sort. The groups stay in memory: the path is offered only when the group table fits hash memory.
- **When:** An ORDER BY or window whose leading keys are group keys, a sub-select or inlined CTE read by a window or ORDER BY (constant columns and COALESCE(key, constant) allowed). Not grouping sets, DISTINCT ON, ORDER BY an aggregate LIMIT, or a group table over hash memory.
- **Default:** automatic — Cost-based: the ordered path competes with the unordered one plus the Sort (or Incremental Sort) and wins when cheaper.
- **Turn off:** SET sift.enable_agg = off (the whole aggregate scan). There is no setting for the order alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The node costs the sort by the groups (n log n integer compares), below the Sort it replaces; the unordered path is still planned, so a statement that wants no order keeps its plan.

#### auto(shared-cte-choice)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** Chooses between keeping a CTE that several aggregates read and inlining it so that each aggregate is a scan, prices the lookup tables and the heap check as PostgreSQL's own reads, and keeps the verdict per backend (4,096 statements) and in shared memory (sift.verdict_cache_kb) so a statement seen again, by this backend or by one that connected later, plans only the winning variant
- **Gain:** Planning -23% over a 624-statement replay (7,684 to 5,942 ms; -29% where the scans win), -38% on a 21-CTE family (891 to 550 Minstr); cte_two_readers 239 to 9.5 ms when the scans win
- **Impact:** index size: -; build, merge and writes: -; other: Memory under 1 MB a backend (4,096 entries of about 200 bytes). Every 8th hit plans in full; an entry drops on DDL, an index change, ANALYZE or VACUUM of a table it reads. A new backend takes the verdicts the others made from shared memory (256 kB; sift.verdict_cache_kb); the first plan after a start of the server and after a change of a table is in full. A stale verdict is a plan of the other variant, never other rows. No index, write or WAL effect.
- **When:** Statements with a CTE that two or more aggregates read, or flattenable laterals, over a table with a column index, planned again by a backend that has seen the shape, or by a new one when another backend saw it (other constants are the same shape). A first plan, a CTE read once (auto(cte-preinline)) and statements without a shared CTE are not helped. The pricing of lookup tables read whole and of the heap check holds for every scan.
- **Default:** automatic — Both variants are plans of the same query, so a wrong verdict costs plan quality at worst, never rows. It removes up to four planner runs for a repeated statement; the 3.3 rule needs no more (no index growth, no write cost, planning -23%).
- **Turn off:** SET sift.enable_agg = off (no CTE or lateral planning by the hook). sift.verdict_cache_kb = 0 turns the shared table off. The cache of a backend has no setting: it drops its entries when a planner setting, work_mem, hash_mem_multiplier, max_parallel_workers_per_gather or a sift.* setting changes.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. sift.explain_why shows a verdict_cached row for a statement planned from the cache and sift.stats() counts verdict_cache_hits, verdict_cache_misses and verdict_cache_flips.

#### auto(cte-preinline)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** A CTE read once is inlined a single time on the tree all the plans of a statement are copied from (PostgreSQL's own conditions: not recursive, SELECT only, nothing writing, no volatile function, not MATERIALIZED), so the up to four plans of a statement do not repeat the walk
- **Gain:** Planning instructions -23.0% (10,772 to 8,294 Minstr, 156 replay statements, steady backend), -21.8% for a backend's first planning, -23.3% on 237 statements (about 3 ms a statement)
- **Impact:** index size: -; build, merge and writes: -; other: Nothing stored, no index, write or WAL effect. Saves planning time; the plan and its costs are equal (0 differences of 786 plans, extension on and off).
- **When:** A statement planned through the hook (sift.enable_agg on) that reads a table with a column index and has a CTE read once. A MATERIALIZED, volatile, writing or recursive CTE stays as written.
- **Default:** automatic — The same query goes to every plan, so plans are the same (text and costs equal on 786 plans) and planning takes 23% fewer instructions: nothing to choose.
- **Turn off:** SET sift.enable_agg = off (the hook plans nothing). There is no setting for it alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; it changes no plan or row. Only statements with a CTE read once gain.

#### auto(json-build-object)

`sift_col`, `sift_table` · default · since 3.6 · default automatic

- **What it does:** A statement that reads a table with a column index calls sift.json_build_object where it names json_build_object: the same text, with the type lookups of each argument made once for the call site, not for every argument of every row
- **Gain:** Instructions -4.9% to -16.3% on five grouped statements over 300,000 rows with 10,000 to 60,000 groups and an object built above the scan (a grouped object of six arguments: 1,836 to 1,558 Minstr; a nested object over lag(): 2,130 to 1,766 Minstr)
- **Impact:** index size: -; build, merge and writes: -; other: Nothing stored, no index, write or WAL effect. The plan shows sift.json_build_object where it showed json_build_object; the text is the same (test json_build_object compares both byte for byte).
- **When:** A statement planned through the hook (sift.enable_agg on) that reads a table with a column index and calls json_build_object with at least one argument, for a role that may execute sift.json_build_object. jsonb_build_object and a call with no argument stay.
- **Default:** automatic — The same bytes are written either way and the call site looks its types up once: nothing to choose.
- **Turn off:** SET sift.enable_agg = off (the hook plans nothing). There is no setting for it alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; the text is the core function's. Only statements that build objects gain.

#### auto(point-directory-aggregate)

`sift_col` · default · since 3.4 · default automatic

- **What it does:** The aggregate scan asks the point directory (point_columns, point_tid_columns) for the rows of a short IN list, an equality, a short range or the keys of an inner or semi lookup join and reads only the units that hold them, each as the listed rows; it skips the directory for keys with many rows each
- **Gain:** 51-key IN over 16,384 units 23.7 to 3.6 ms (slope 0.80 to 0.20), 51-row lookup join 28.6 to 2.7 ms; point_keys_agg (1,000,000 rows) 80.9 to 2.85 ms; dense keys: worst cell 1.2x the zones, was 3.9x
- **Impact:** index size: -; build, merge and writes: -; other: No index growth or write cost of its own (the directory is the one the options write). The directory is asked once per segment and source and kept with the unit cache; the planner prices the units it leaves. Keys whose rows lie in most units are answered from zones and summaries.
- **When:** An index with point_columns or point_tid_columns and an IN list of up to 65,536 keys, an equality, a short range or lookup-join keys on that column. The directory is read only while keys x rows listed / distinct keys of the segment is at most 4,096 or a 64th of its rows.
- **Default:** automatic — Costed: the planner prices the units the directory leaves, and the guard keeps it off where it costs more than the zones. No format change, no index growth, no write cost: the 3.3 rule is met (3x or more on the few-key class).
- **Turn off:** No setting for the guard. ALTER INDEX .. RESET (point_columns) and RESET (point_tid_columns) with a REINDEX drop the directory; SET sift.enable_agg = off turns off the aggregate scan.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. EXPLAIN ANALYZE shows Units From Point Directory and Rows From Point Directory. It needs point_columns or point_tid_columns on the key; a key with many rows each (a status, a tenant) does not use it.

#### auto(run-heads)

`sift_col` · default · since 3.4 · default automatic

- **What it does:** The first and last keys of a sorted run (up to 32 at each end, with the heap addresses of their rows) answer ORDER BY .. LIMIT n (n <= 32, one sort key, no condition, join or aggregate) for a clean, all-visible unit without reading the unit
- **Gain:** ORDER BY k LIMIT 10 0.128 to 0.051 ms (800,000 rows; b-tree 0.010), executor 645k to 245k instructions; LIMIT above 32 or a filtered LIMIT gain nothing
- **Formula:** S 2.63x saving 400k instructions, G 0.6%, W 0.0%, R 0.5: B 9.9 against C 0.2, automatic
- **Impact:** index size: +0.60% (94,044,160 against 93,478,912 bytes, three order columns, 800,000 rows); +2.4% (narrow index, 353,029 rows); build, merge and writes: Build 1.61 against 1.63 s (800,000 rows), promotion of 100,000 rows 0.052 against 0.052 s, merge of 500,000 rows 0.74 against 0.66 s (noise +-10%); other: format minor 18; Index +0.6% to +2.4% (0.24 B a row at units of 4,096 rows or more); build, promotion and merge within noise. Needs order_columns; none for a unit under 128 rows, a leaf with NULL or overflow, or a key_cut segment. A 3.3 library cannot read minor 18.
- **When:** An index with order_columns, ORDER BY one sort key LIMIT n of at most min(32, rows / 128) of a unit, no filter, join or aggregate, a unit alone in its superblock with every page all-visible and clean, no write buffer rows. Not for LIMIT above 32, a filtered LIMIT or a leaf with NULL.
- **Default:** automatic — Meets the 3.3 rule: 2-3x on unfiltered ORDER BY .. LIMIT <= 32 (limit 5%), growth 0.6% to 2.4% (limit 3%), write cost inside noise (limit 5%). It follows order_columns, so no second opt-in.
- **Turn off:** SET sift.col_format_minor = 14 (runs are written without heads; units already written keep them until rewritten), SET sift.topn_runs = off (runs and heads are not read) or SET sift.enable_topn = off (no top-N scan).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on for an index with order_columns; EXPLAIN shows Units Answered By Run Head. A b-tree is still 3-5x faster for LIMIT 10, and heads do not apply to LIMIT above 32 or to a filter.

#### auto(table-cube-minmax)

`sift_table` · default · since 3.5 · default automatic

- **What it does:** GROUP BY with min or max of an int2, int4, int8 or numeric(p,s) column, by a key the cube has a dim for, over whole units, reads no row (EXPLAIN ANALYZE shows Units From Pre-aggregates).
- **Gain:** GROUP BY g with min and max of an integer column, 500,000 rows, 7 columns, 50 groups, 7 units: 2.860 to 0.140 ms, 55.46M to 1.31M instructions
- **Formula:** S 42.30x saving 54150k instructions, G 3.1%, W 0.2%, R 0.5: B 38.4 against C 1.1, automatic
- **Impact:** index size: +3.1% of the column groups of a 4-integer-column table (532,480 to 548,864 bytes, 500,000 rows) and +0.7% of an 8-column one (2,457,600 to 2,473,984 bytes); build, merge and writes: User instructions of the writer (perf stat, 500,000 rows, 3 runs, 3.5 library with and without the change): CTAS and VACUUM FREEZE of 4 integer columns 3,427M to 3,426M (0.0%), of 8 columns 4,898M to 4,907M (+0.2%); other: format minor 7; The cube of every unit holds, for each integer measure of a dim, the smallest and largest value of each group (zigzag keys after the sums): within the cube's byte budget, none for the calendar cells of a date or timestamp column (their bytes fit the calendar budget only without them), none for a float measure. A 3.2 library cannot read minor 7.
- **When:** A sift_table with a low-cardinality key (a store, a status, a kind) and integer or numeric measures, units clean and all-visible. Not a float measure, a calendar cell or a unit with a dead row.
- **Default:** automatic — Meets the 3.3 rule: 20x at 50 groups (limit 5%), growth 0.7% to 3.1% (limit 3%; the 4-column table is a narrow one with nothing else in the unit), write cost 0.0% to 0.2% of the writer's instructions (limit 5%). It is the default of cube_minmax for an index since 3.3.
- **Turn off:** No per-table switch: SET sift.col_format_minor = 6 writes no min or max (groups already written keep theirs until rewritten), and it also stops every later feature of the writer (calendar cells, key lists and run heads of a table, column format minors 8 to 18; it is the 3.2 format); SET sift.agg_preagg = off reads none of the pre-aggregates.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; EXPLAIN ANALYZE shows Units From Pre-aggregates. A min or max of a float column, of a month of a timestamp, or by a column that is not a dim of the cube reads the rows.

#### auto(table-rollup)

`sift_table` · default · since 3.5 · default automatic

- **What it does:** GROUP BY of the unnested elements of a declared jsonb list (sift.table_set_paths, doc.items[]{k int4, p float8}) by its low-cardinality leaves, with count, sum, min and max of the measures, over whole units, reads no element (EXPLAIN ANALYZE shows Units From Pre-aggregates, Units Without The Rollup 0).
- **Gain:** GROUP BY an element key with count and sum of an element float, 500,000 rows, 2 elements a row, 10 units: 22.977 to 0.121 ms, 97.73M to 0.46M instructions
- **Impact:** index size: none: the bytes of the groups are those the 3.5 tip writes; build, merge and writes: none: the writers are unchanged; other: Nothing new is written: the units of a table already carry the element rollups (the 2% byte budget a list of the index default); the source reads them with the code of the sift_col source (unit_rollup, unit_rollup_tuples, unit_rollup_miss).
- **When:** A sift_table with a jsonb column whose list paths are declared (sift.table_set_paths), GROUP BY over jsonb_to_recordset / unnest of it by element leaves of few values, units clean and all-visible. Not a grouping by a leaf of many values, a filter on the row or a unit with a dead row.
- **Default:** automatic — Meets the 3.3 rule: 140x at 500,000 rows (limit 5%), growth 0% (limit 3%), write cost 0% (limit 5%): the rollups were written already and nothing read them.
- **Turn off:** SET sift.agg_rollup_cost = 1000 (no unit is answered from a rollup).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; EXPLAIN ANALYZE shows Units From Pre-aggregates. A grouping by a leaf the rollup does not hold, a filter on a column of the row, or a unit with a dead or buffered row reads the elements.

#### auto(table-key-lists)

`sift_table` · default · since 3.5 · default automatic

- **What it does:** count(DISTINCT col) of a sift_table column that is an int2, int4, int8 or date foreign key or *_id and not unique reads the key list or bitmap of each clean, all-visible unit, not its rows
- **Gain:** count(DISTINCT fk) of 500,000 rows, 7 columns, 21 units: 2.10 to 0.053 ms, executor 36.2M to 0.69M instructions; with a date filter 2.10 to 0.106 ms, 36.5M to 1.30M
- **Formula:** S 52.50x saving 35510k instructions, G 0.6%, W 1.2%, R 0.5: B 40.6 against C 0.4, automatic
- **Impact:** index size: +0.00% to +0.55% of the column groups (8 KB page granularity, 500,000 rows, 7 columns: 1,490,944 to 1,499,136 bytes after COPY with the lists, none visible after INSERT and VACUUM); build, merge and writes: User instructions of the writer: INSERT 3,393,416,045 to 3,393,024,718 (-0.01%), VACUUM FREEZE 5,732,666,520 to 5,800,252,851 (+1.18%), COPY 6,453,099,597 to 6,486,689,588 (+0.52%), 100 batches of 1,000 rows 29,270,531,273 to 29,273,684,061 (+0.01%), UPDATE of 10% 2,006,963,472 to 2,006,942,673 (0.00%), VACUUM after it +0.54%, merge +1.58%; lists and run heads together; other: format minor 15; A list or bitmap per unit and column, written only where the lists of all the columns of the unit take at most 3% of its bytes (a column of random keys keeps none). A dirty unit (a tombstone or a write buffer row) is read as before; groups written earlier have none until VACUUM or a merge rewrites them.
- **When:** A sift_table with an int2, int4, int8 or date column that is a foreign key or ends in _id and is not unique, a layout with no jsonb path, units clean and all-visible, count(DISTINCT col) over whole units.
- **Default:** automatic — Meets the 3.3 rule: 40x in instructions on count(DISTINCT fk) (limit 5%), growth under 0.6% (limit 3%), write cost 0.0% to 1.6% of the writer's instructions (limit 5%). The columns are the default of key_columns for an index, so there is no second choice to make.
- **Turn off:** SET sift.col_format_minor = 14 (no list is written; groups already written keep theirs until rewritten) or SET sift.agg_preagg = off (no unit is answered from pre-aggregates).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; EXPLAIN ANALYZE shows Units From Pre-aggregates. A column that is not a foreign key or named *_id has no list; a sift_col index takes key_columns for the others.

#### auto(table-run-heads)

`sift_table` · default · since 3.5 · default automatic

- **What it does:** ORDER BY ts LIMIT n (n <= 32, one sort key, no condition, join or aggregate) of a timestamp or timestamptz column the rows follow reads the first and last keys of each clean, all-visible unit, not the unit
- **Gain:** ORDER BY ts DESC LIMIT 10 of 500,000 rows, 7 columns, 19 units: 0.140 to 0.038 ms, executor 1.74M to 0.40M instructions; ASC LIMIT 10 0.80M to 0.44M; LIMIT 32 0.82M to 0.58M (ASC), 2.26M to 0.67M (DESC); LIMIT 33 to 10,000 within 1%
- **Formula:** S 4.35x saving 1340k instructions, G 1.4%, W 1.2%, R 0.5: B 15.1 against C 0.7, automatic
- **Impact:** index size: +1.42% of the column groups with one timestamp column (1,736,704 to 1,761,280 bytes, 500,000 rows, 7 columns, 19 units); +0.55% after COPY; build, merge and writes: User instructions of the writer: INSERT 3,393,416,045 to 3,393,024,718 (-0.01%), VACUUM FREEZE 5,732,666,520 to 5,800,252,851 (+1.18%), COPY 6,453,099,597 to 6,486,689,588 (+0.52%), 100 batches of 1,000 rows 29,270,531,273 to 29,273,684,061 (+0.01%), UPDATE of 10% 2,006,963,472 to 2,006,942,673 (0.00%), VACUUM after it +0.54%, merge +1.58%; lists and run heads together; other: format minor 18; A run header and its head (up to 32 keys at each end with the heap addresses of their rows: 16 B a key) per unit, written only where the run takes at most 3% of the unit's bytes, which is a column the rows follow (a run of any other takes 2 B a row and is not written); none for a unit with NULL or under 128 rows. A source does not walk a run that is the rows in their own order (LIMIT above 32 reads the unit as before). A 3.3 library cannot read minor 18.
- **When:** A sift_table timestamp or timestamptz column loaded in order (created_at), ORDER BY it LIMIT up to 32 with no filter, join or aggregate, units clean and all-visible with no NULL in the column. Not a column the rows do not follow, a filtered LIMIT or LIMIT above 32.
- **Default:** automatic — Meets the 3.3 rule: 3.7x at LIMIT 10 DESC (limit 5%), growth 0.55% to 1.42% (limit 3%), write cost 0.0% to 1.6% of the writer's instructions (limit 5%). The column is the one whose run fits 3% of a unit, so there is no second choice to make.
- **Turn off:** SET sift.col_format_minor = 12 (no run is written; groups already written keep theirs until rewritten), SET sift.topn_runs = off (runs and heads are not read) or SET sift.enable_topn = off.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; EXPLAIN ANALYZE shows Units Answered By Run Head. A b-tree on the column is still faster for LIMIT 10; heads do not apply to LIMIT above 32 or to a filtered LIMIT.

#### auto(top-n-zone-probe)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** ORDER BY .. LIMIT over a sift_col index tells a sort column that follows the table from one that does not by reading the zones of six superblocks from the segment headers, not by opening the unit of each
- **Gain:** ORDER BY rnd LIMIT 10, 100,000 rows with order_columns: 617,524 to 524,572 instructions a statement (executor 288k to 192k; b-tree 353,686), callgrind, one segment
- **Impact:** index size: -; build, merge and writes: -; other: Nothing stored, no index growth or write cost. Saves about 94k instructions a statement; the begin of each superblock still loads its write buffers.
- **When:** Every ORDER BY .. LIMIT scan over a sift_col index whose walk visits superblocks, before the walk; the saving is largest for a small LIMIT, where the probe is most of the statement.
- **Default:** automatic — The same zones read from the headers: 15% fewer instructions with the same plan and rows, so there is nothing to choose.
- **Turn off:** SET sift.enable_topn = off turns off the top-N scan; the probe has no setting of its own.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; plan and rows are unchanged (Units Answered By Run Head: 7, Superblocks Skipped By Top-N: 16 in the test).

#### auto(parallel-aggregate-scan)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** The aggregate scan plans workers for shapes that kept one process: a preserved join (a calendar LEFT JOIN the facts), count(DISTINCT) of a column of a joined table, and lookup joins that read a CTE, a sub-select or a function
- **Gain:** 1,600,000 rows, calendar LEFT JOIN facts: 522 to 79 ms with 8 workers; count(DISTINCT) of a joined column, 800,000 rows: 29.9 to 17.7 ms (4); CTE lookup request: 80 to 42 ms (4)
- **Impact:** index size: -; build, merge and writes: -; other: Each process keeps its own tables and the leader merges them; lookup tables are written to shared memory once and mapped by the workers. Worker start is priced at max(2 x parallel_setup_cost, (3 + 0.35 x workers) ms). No index, write or WAL effect; throughput is not raised when cores are busy.
- **When:** Workers as PostgreSQL gives a parallel seq scan of the table (compute_parallel_worker: table size, the parallel_workers reloption, max_parallel_workers_per_gather), when the query and the functions the scan evaluates are parallel safe and the priced scan is cheaper. Ordered aggregates, hypothetical sets, min, max and numeric sums of the nullable side stay serial.
- **Default:** automatic — Cost-based: worker start and the pairs the leader adds are priced, so a small scan stays serial; rows are equal at every worker count (float sums add in lane order).
- **Turn off:** SET max_parallel_workers_per_gather = 0 (PostgreSQL's setting; sift.parallel_scan governs the BM25 scans only).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. A small scan stays in one process (100,000 rows with 8 workers took 16.7 against 14.7 ms). Workers cut the latency of heavy requests when cores are spare; Workers Planned in EXPLAIN shows them.

#### auto(preagg-exceptions)

`sift_col` · default · since 3.4 · default automatic

- **What it does:** A unit in which one integer leaf is NULL in at least 99% of the rows (a soft-delete stamp) also keeps the pre-aggregates of the rows where it is not NULL, so WHERE stamp IS NULL is the unit's aggregate minus them (IS NOT NULL is them alone), not a scan of the unit
- **Gain:** count and avg by a key WHERE stamp IS NULL, 353,029 rows: 6.1 to 2.5 ms, 171 of 171 units from pre-aggregates (5 before); soft_delete_exceptions, 1,000,000 rows: 158 ms normal plan to 0.69 ms; 600,000 rows with 1,127 stamped: 39.9 M to 9.7 M instructions a query (214 of 214 units from pre-aggregates)
- **Formula:** S 4.12x saving 30241k instructions, G 1.4%, W 0.9%, R 1: B 29.0 against C 0.7, automatic
- **Impact:** index size: +1.41% (6,381,568 to 6,471,680 bytes; 353,029 rows, 655 stamped, 166 units with exceptions); build, merge and writes: CREATE INDEX +0.94% instructions (2,238 M to 2,259 M; 600,000 rows, 1,127 stamped, index of an id, a key, the stamp and one integer measure, REINDEX at column format minor 15 against 14); +0.55% bytes (4,980,344 to 5,007,560); other: format minor 19; Index +1.4% on a table with 0.2% of the rows stamped. Written by a merge, VACUUM's rewrite and REINDEX; min and max cannot be subtracted (the unit is scanned for that measure) and a float sum is guarded against cancellation. A sift_table writes none; a dirty or not all-visible unit is scanned as before.
- **When:** A sift_col unit with exactly one top-level integer leaf, no overflowed value and at most 1 row in 100 not NULL (the fewest, the first of equals), a filter leaf IS NULL or IS NOT NULL beside conjuncts the pre-aggregates decide, and units clean and all-visible. A date range with the filter decides only the units it covers.
- **Default:** automatic — No option: the unit's own data decides (one integer leaf NULL in 99% of its rows). Growth 1.4% and a write cost under 1% meet the default formula at 4.1x on the class.
- **Turn off:** SET sift.agg_preagg = off (no unit is answered from pre-aggregates) or SET sift.col_format_minor = 14 (no exceptions are written; units already written keep them until rewritten).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on; EXPLAIN ANALYZE shows Units Net Of Exceptions. A leaf with 1% or more non-NULL rows has no exceptions and the filter scans the unit.

#### auto(index-pick)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** A table with several sift_col indexes is planned through each index that gives a plan and the cheapest is kept, not the newest: a key lookup reads the index cut by that key (key_cut), a range of dates the index cut by block range, whatever the creation order
- **Gain:** 400,000 rows, a page-cut and a key_cut index: month count 0.86 to 0.036 ms; index_pick, 1,000,000 rows: 23.8 ms normal plan to 1.05 ms
- **Impact:** index size: -; build, merge and writes: -; other: Planning: about one single-index plan (0.05 to 0.1 ms) more for each extra index, growing with the index directory (geometric mean x1.4-1.5 with 3 indexes, x1.7-2.0 with 5; one index unchanged). No index, write or WAL effect.
- **When:** A table with two or more sift_col indexes that both give a plan (a key_cut next to a page-cut one, a full next to a partial one). A table with one index plans as before; on a clustered copy both tie within noise.
- **Default:** automatic — Cost-based: add_path keeps the cheapest of the priced paths, and the price of a pre-aggregate scan follows the units the zones keep. Planning costs 0.05 to 0.1 ms per extra index.
- **Turn off:** No setting for the pick. Dropping the extra index leaves one index, planned as before; SET sift.enable_agg = off turns off the aggregate scan.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Keep one sift_col index per table unless two layouts serve different queries; each extra index adds 0.05-0.1 ms to planning.

#### auto(distinct-bigint)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** count(DISTINCT x) of a bigint column is kept as the exact distinct values of a decimal expression (the path count(DISTINCT x + 1) took) instead of a combination of a generic aggregate: the number is the same and the aggregate runs in the workers of a parallel scan
- **Gain:** 1,000,000 rows: a 100,000-valued column 85.7 to 20.6 ms, a unique id 499 to 70 ms, 100 groups 448 to 27 ms; count_distinct_unique 932 ms normal plan to 3.95 ms
- **Impact:** index size: -; build, merge and writes: -; other: No index, write or WAL effect. The exact set lives in the scan's group table, bounded by hash memory (work_mem) as for an int4 column, which is still about 3.5x faster than a bigint (an id map, not a bitmap).
- **When:** count(DISTINCT x) of a bigint column (NULLs, FILTER, int8 extremes and expressions covered) in an aggregate scan over a table with a column index. With 2 workers the time did not drop at these sizes (worker start costs as much); the gain is the aggregate now runs in the workers.
- **Default:** automatic — Exact and identical to the normal plan (agg_distinct_parallel at 0, 2, 4 workers), 4-7x faster, no storage change: the 3.3 rule.
- **Turn off:** SET sift.enable_agg = off. There is no setting for this path alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. count(DISTINCT) stays exact whatever the settings say; the approximate sift.approx_count_distinct is a separate opt-in.

#### auto(histogram-aggregates)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** percentile_cont and percentile_disc find the ranks of a group by a quickselect over its histogram, not a sort of its slots (percentile_disc also of a decimal up to 18 digits, timestamp, timestamptz or bigint on 64-bit keys); mode() is the most frequent entry of the same histogram
- **Gain:** 40 groups, 1,000,000 float8 rows: median 193 to 76 ms; percentile_select 606 to 121 ms; decimal median, 400,000 rows, 168 to 70 ms; mode of a decimal 175 to 26 ms; mode_histogram 468 to 83.6 ms
- **Impact:** index size: -; build, merge and writes: -; other: No index, write or WAL effect. The histogram of (group, value, count) is held in memory; one that spills goes through the tuplesort. Aggregates on one argument share it. mode() of a text column keeps the generic path.
- **When:** percentile_cont and percentile_disc over float, integer or date; percentile_disc also over a decimal of up to 18 digits, timestamp, timestamptz or bigint; mode() over float, integer, date, timestamp or decimal of up to 18 digits, in an aggregate scan. Other types keep the generic path.
- **Default:** automatic — Same answers as the normal plan (agg_ordered_set, 0 rows differ) and 2.4x to 5.6x faster on the class, no index growth, no write cost: the 3.3 rule.
- **Turn off:** SET sift.enable_agg = off. There is no setting for these aggregates alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The answers are the final functions' (ties, NULLs, NaN, DESC, spilled histograms, workers); EXPLAIN shows the aggregates among Pushed Aggregates.

#### auto(order-by-aggregate-limit)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** ORDER BY <aggregate> LIMIT n [OFFSET m] over a plain GROUP BY ranks the groups by the raw value of the aggregate and emits only those that tie with or beat the (n + m)-th, before they become tuples; the sort and limit above stay and rank the ties
- **Gain:** GROUP BY id ORDER BY sum(a * b) DESC LIMIT 10: 400,000 groups 96.6 to 14.6 ms (normal plan 334); group_by_id_top 1,000,000 groups 174 to 61 ms; group_topn 581 ms normal plan to 72.8 ms
- **Impact:** index size: -; build, merge and writes: -; other: No index, write or WAL effect. A bounded heap over the accumulators; the node's row estimate is lowered to about 2n. A NaN decimal sum or a float sum that overflows turns the drop off for the statement; a spilled or partitioned scan keeps all its groups.
- **When:** The first sort key is count, sum of an integer, decimal or float8, or avg of a float8 column, over a plain GROUP BY; no HAVING, window, DISTINCT, grouping sets or WITH TIES. Gains grow with the number of groups (100,000 groups: 23.2 to 4.4 ms).
- **Default:** automatic — Same rows as the normal plan (agg_group_topn: ASC and DESC, NULLS, OFFSET, NaN, ties, workers) and 1.4x to 6.6x faster on the class; no index growth, no write cost: the 3.3 rule.
- **Turn off:** SET sift.enable_agg = off. There is no setting for it alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. EXPLAIN shows Groups Dropped By Top-N. HAVING, a group key as first sort key, an expression of aggregates and ROLLUP keep the old path.

#### auto(exists-decorrelation)

`sift_col`, `sift_table` · default · since 3.4 · default automatic

- **What it does:** EXISTS and NOT EXISTS (SELECT .. FROM f WHERE f.k = x.k AND ..) where PostgreSQL leaves a sub-plan for each outer row (select list, under OR, in CASE) become f grouped by k and left joined to x, EXISTS being a group that is there
- **Gain:** 1,000,000 orders, 2,000 outer rows: exists_decor 259 to 11.7 ms (22x); 6,000 outer rows over 200,000: 12.8-33.7 to 2.6-3.0 ms in the select list, 14.0-14.9 to 1.7-2.3 ms under OR NOT EXISTS
- **Impact:** index size: -; build, merge and writes: -; other: No index, write or WAL effect. A statement of the shape is planned twice (grouped and as written), the cheaper kept; planning time of this form not measured separately.
- **When:** An equality tie of the table's column and an outer one of the same type, a table with a column index, many outer rows. Not for IN or NOT IN, inequalities, LIMIT, a join or sub-select inside, or an outer query that groups: those stay as written.
- **Default:** automatic — Cost-based: kept only when the planner costs it below the lookups; EXISTS is never NULL in either form (fuzz family exists: 960 queries, 0 divergent).
- **Turn off:** SET sift.enable_agg = off. There is no setting for this rewrite alone.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. An EXISTS that the WHERE ANDs stays the semi join (NOT EXISTS the anti join) PostgreSQL makes of it. sift.explain_why shows the arm used.

#### auto(top-n-run-probes)

`sift_col` · default · since 3.4 · default automatic

- **What it does:** A top-N over a table with order_columns (the sorted run of each unit) reads the sort key of a unit's rows only for the rows it keeps and sizes its candidate buffers by the unit, not by the table's largest
- **Gain:** 40 scans ORDER BY a key column LIMIT 10..49, 1,000,000 orders: 48 to 24.8 ms; ORDER BY rnd LIMIT 10, 800,000 rows: 113 to 82 us a scan; LIMIT 1000: 1.23 to 1.05 ms
- **Impact:** index size: -; build, merge and writes: -; other: Nothing stored: the segment format does not change, no index growth or write cost.
- **When:** A top-N scan whose first key is a leaf with a run (an index with order_columns) and a unit of at least 2 x (limit + 1) rows passing.
- **Default:** automatic — Same rows and units read, less work per scan; no format change, no growth, no write cost.
- **Turn off:** SET sift.topn_runs = off (units are tested row by row) or SET sift.enable_topn = off.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. A b-tree is still faster on the same queries (15 us and 140 us against 82 us and 1.05 ms).

#### sift.col_leaf_cache_kb

`sift_col` · default · since 3.5 · default 4096 kB

- **What it does:** a new backend plans over a sift_col index without reading the zones of all its units: the first backend that plans folds them into the statistics of the planner and keeps them for the others
- **Gain:** planning of the first statement of a new backend over an index of 10,000 units and 6 leaves (2,000,000 rows, key_cut): 9.4 ms and 2,494 buffers to 0.9 ms and 213 once another backend has planned (17 ms and 2,284 reads for the very first after a start); the work grows with the units and the leaves, a new connection to an index of 63,000 units and some forty leaves paid 100 to 800 ms
- **Impact:** index size: -; build, merge and writes: -; other: one table for the server, whatever the number of backends: the statistics of an index are about 33 kB for each integer leaf and 0.4 kB for the others, 4 MB by default (256 MB at most); where it cannot be allocated the backend logs once and folds them itself; the size is taken when the first backend allocates it (a restart to change it); the record of an index is read again from the zones when its segments or options change
- **When:** Servers whose clients connect often (a pool that resets its sessions, an application that opens a connection for each request) and read large sift_col indexes: the planning of the first statement of each connection. The first backend after a start of the server, and after a merge, a rewrite or a REINDEX of the index, folds the statistics as before.
- **Default:** 4096 kB — On: 4 MB of shared memory, allocated by the first backend that needs it, and a copy of a record (0.2 ms for an index of six leaves) in place of the fold; the statistics are the same numbers, so the plans do not change.
- **Turn off:** SET sift.col_leaf_cache_kb = 0 (superuser; postgresql.conf for all backends): each backend then folds the statistics of every index it plans over once.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at the default. 0 turns the sharing off (each backend then folds the statistics of an index once, as before); more room helps only a server with indexes of many integer leaves (a record is 33 kB for each).

#### sift.col_dict_shared_cache_kb

`sift_col` · default · since 3.6 · default 65536 kB

- **What it does:** a new backend, a pooled connection or a parallel worker reads the parsed dictionary of a column that another backend parsed, in place, instead of inflating and indexing all of it again
- **Gain:** first statement of a new backend, `ip = x/51` over 1,000,000 rows of IPv4 and IPv6 networks, millions of instructions of one backend: 286 (cut index) and 332 (uncut) to 2.5 and 2.3 once another backend has kept the dictionary, 877 and 516 to 2.5 and 2.4 over 2,000,000 rows (the dictionary no longer fits the 64 MB of a private cache); 1,000,000 rows take 66 MB of the ring, 2,000,000 132 MB and 3,000,000 213 MB
- **Impact:** index size: -; build, merge and writes: -; other: one ring of records for the server, whatever the number of backends, taken whole from dynamic shared memory when the first backend keeps a dictionary (65,536 kB by default, 4,194,304 kB at most, so that the setting cannot ask for a size a server is not likely to have): this memory is in addition to shared_buffers, is not reclaimable like page cache (with dynamic_shared_memory_type = posix it counts against /dev/shm and the shared memory of a container, and is swap-backed), and gets no huge pages; with posix at most a quarter of /dev/shm is used and the log says so; where the ring cannot be had the backend logs once and parses its own; a dictionary under 128 kB or over half of the ring is not kept; the context is superuser, as the other shared caches: the value of the backend that creates the ring wins (a superuser who sets a size in a session before the ring exists decides the size for the server), so to set the size for the server set it in postgresql.conf and restart (a later change of the setting only turns the sharing on and off, 0 for off, and never resizes or frees the ring)
- **When:** Servers whose clients connect often (a pool that resets its sessions, an application that opens a connection for each request) or run parallel queries over large sift_col indexes with text columns of many distinct values: the first statement of each connection parses the dictionaries of the columns it reads.
- **Default:** 65536 kB — On: 64 MB of shared memory, allocated by the first backend that keeps a dictionary of 128 kB or more, and a read in place (a few instructions) in place of the parse; the rows are those of the segment, so nothing else changes. 64 MB holds the dictionaries of about 1,000,000 distinct values with their hash tables; more room is a choice of the DBA, not the default.
- **Turn off:** SET sift.col_dict_shared_cache_kb = 0 (superuser; postgresql.conf for all backends): no segment is made or attached by the backend, and each backend parses and keeps its own dictionaries.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at the default on a server with a few hundred MB of shared memory to spare. 0 turns the sharing off (each backend then parses and keeps its own dictionaries, up to the dict_cache_kb of the index); raise it for dictionaries of more than about 1,000,000 distinct values with their hash tables (a ring of 256 MB held the dictionaries of 1,000,000, 2,000,000 and 3,000,000 rows of the measurement); lower it on a small server or a container, whose /dev/shm is 64 MB.

#### auto(dict-tsig)

`sift_col` · default · since 3.6 · default automatic

- **What it does:** A substring test (LIKE '%part%') of 3 bytes or more on a text leaf whose dictionary no longer fits the cache of parsed dictionaries reads only the blocks of 64 strings whose trigram filter holds the part, and does not inflate the dictionary again for each query.
- **Gain:** LIKE '%part%' over 3,000,000 URLs, a part in 42 rows 1,336 to 944 M instructions (S 1.42); a part in 3.5% of the rows 1,341 to 1,256 M; random hex, rare part 986 to 127 (S 7.8)
- **Formula:** S 1.42x saving 392000k instructions, G 9.4%, W 3.9%, R 1: B 7.2 against C 3.9, automatic
- **Impact:** index size: +0.33% (448,200,704 against 446,734,336 bytes, 3,000,000 rows, 6 columns); +9.4% (URL column alone, 79,740,928 against 72,884,224); +27% (random hex, 3,000,000 rows); build, merge and writes: Build 73,218 against 72,611 M instructions (+0.8%); URL alone 13,847 against 13,332 M (+3.9%); other: format minor 29; Index +0.33% (6 columns), +9.4% (the URL column alone) on real text; +27% on random hex strings. Build +0.8% and +3.9%. Only a dictionary over half of dict_cache_kb (or whose scaled size is over it) and 1 MiB of strings of 5 bytes or more is signed. A 3.5 library cannot read minor 29.
- **When:** A text leaf with substring tests (LIKE '%part%', ILIKE, regular expressions with a literal of 3 bytes or more) whose segment dictionary is over half of dict_cache_kb, or whose dictionaries of the whole index together exceed it. Not for a leaf that fits (it is parsed once and kept), a part under 3 bytes or a part in most blocks.
- **Default:** automatic — Weighted formula: B 3.6 (S 1.42, R 0.5) to 7.2 against C 0.3 (6 columns) to 3.9 (the URL column alone, the worst real case); the size of the dictionary against the cache is the signal that the filter is needed, and growth follows the entropy of the strings, so no selectivity test.
- **Turn off:** SET sift.col_format_minor = 14 (leaves are written without the signature; the index stays readable by a 3.5 library)
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. The leaves that fit the cache are not signed. To opt out set sift.col_format_minor to 14 before building or rebuilding the index.

#### auto(text-lz4)

`sift_col` · default · since 3.6 · default automatic

- **What it does:** The strings of a text column stored inline (strings over 255 bytes, or more distinct text than the segment dictionary holds) are wrapped with LZ4 instead of zstd where the default formula says so: a substring, equality or GROUP BY on the column reads 1.1 to 3.4 times fewer instructions a row. The same for the segment dictionary of a column whose parsed dictionary does not fit dict_cache_kb (blocks of LZ4): ip = x on mixed IPv4 and IPv6 networks at 3,000,000 rows 1,181 -> 266 M instructions.
- **Gain:** Substring of a sentence 2,371 -> 683 instructions a row (3,000,000 rows, heap scan 4,286); equality 2,305 -> 617; GROUP BY 10,381 -> 8,693; referer equality 623 -> 327
- **Formula:** S 3.45x saving 5060000k instructions, G 19.3%, W 0.2%, R 1: B 25.4 against C 6.5, automatic
- **Impact:** index size: +23.7% (554.2 against 448.2 MB, 3,000,000 rows, 6 columns); +54% (252.6 against 164.3 MB, sentences alone); +9.2% (309.9 against 283.8 MB, unique URLs and referers); build, merge and writes: Build 73,335 against 73,213 M instructions (+0.2%); 32,327 against 32,196 (+0.4%); other: Index +23.7% (6 columns, one of long prose), +54% (the sentences alone), +9.2% (unique URLs and referers); build within +0.4% of the instructions. Nothing for text that repeats (dictionary) or that LZ4 does not shrink (hex). Dictionary blocks: +12.7% to +18.9% (cut network index), +4.9% uncut. Format minor 29 for a segment with LZ4 blocks.
- **When:** A text leaf with inline units whose strings compress to 0.3 to 0.6 of their bytes under zstd (prose, long text); not a leaf kept in the dictionary, not text that LZ4 cannot shrink (hex), not text that compresses so well that the extra bytes cost more than the unwrap saves.
- **Default:** automatic — Weighted formula per chunk: B = log2(S) / log2(1.05) with S from 1.8 + the unwrap (zstd 45 instructions a compressed byte, LZ4 0.5 a byte + 5.5 a compressed byte), against C = G / 3 for the growth of the strings: sentences B 25.2 against C 18.0 take LZ4, titles B 9.6 against 32 keep what they have.
- **Turn off:** compression = 'auto; col=zstd' (the NOTICE at CREATE INDEX gives the line), then REINDEX; sift.col_format_minor = 14 writes no LZ4 dictionary blocks
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. To keep the footprint of a column: compression = 'auto; col=zstd' and REINDEX; the NOTICE at CREATE INDEX lists the columns that took LZ4 (and the ones that came close) with their numbers.

### opt-in

#### sift(analyzer)

`BM25` · opt-in · since 1.1 · default text

- **What it does:** keyword indexes each whole value as one term, for facets and exact keys
- **Gain:** keyword facet counts of broad queries on 26 values 1.9-4.5x faster than GROUP BY; selective queries or thousands of values lose
- **Impact:** index size: -27.3% (450,560 to 327,680 bytes; 100,000 short keyword values of 2,000 distinct, keyword against text); build, merge and writes: build -3.1%, insert of 20,000 rows +1.0%: within noise (100,000 rows); other: REINDEX to change
- **When:** A sift index on a text key used for exact values and sift.facets / sift.facet_counts. Leave text for searched prose; keyword indexes the whole value, so word queries on it do not match inside a value.
- **Default:** text — text: a keyword index answers whole values only. Opt-in for result: broad facet counts 1.9-4.5x faster than GROUP BY, selective queries or thousands of distinct values lose.
- **Why not the default:** changes results or semantics
- **Turn off:** Leave the default text: CREATE INDEX without analyzer. On an existing index ALTER INDEX .. RESET (analyzer) makes it unusable until REINDEX; build a text index with CREATE INDEX CONCURRENTLY and drop the keyword one.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at text. Set keyword on a separate index of a column used for exact keys and facet counts (sift.facets): it indexes each whole value as one term. Needs REINDEX.

#### sift(stemmer)

`BM25` · opt-in · since 1.0 · default ''

- **What it does:** Snowball English stemming: word forms match
- **Gain:** -
- **Impact:** index size: -3.1% (8.78 to 8.51 MB; 100,000 documents of 30 words, en against none); build, merge and writes: build +22.3% (497 to 609 ms), insert of 20,000 documents +74.7% (95 to 165 ms); other: REINDEX to change
- **When:** Text indexes of English prose, so running matches runs. English only (en), no stopword list; it changes what matches and the same analyzer applies to every query on the index. Not for other languages.
- **Default:** '' — Unset (no stemming): it changes results, and en costs insert +74.7% (95 to 165 ms) and build +22.3% for -3.1% size (100,000 documents).
- **Why not the default:** changes results or semantics
- **Turn off:** Leave unset or stemmer = 'none' at CREATE INDEX. On an existing index ALTER INDEX .. RESET (stemmer) then REINDEX; until then queries and inserts fail. Prefer CREATE INDEX CONCURRENTLY and swap.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave unset. Set en to match English word forms (running finds runs); it changes what matches, so build it with CREATE INDEX CONCURRENTLY and swap. There is no stopword list.

#### sift(prefix_index)

`BM25` · opt-in · since 1.0 · default 0

- **What it does:** prefix queries of up to n characters need no term expansion (autocomplete)
- **Gain:** prefix queries of up to n characters, 200,000 short rows (instructions of one backend): the first call of a backend for a 1-letter prefix 112.1 M to 0.9 M, 3-letter 111.3 M to 0.2 M, warm 0.5 M to 0.2 M; a prefix of 4 or more characters is expanded as before. 1-letter prefix on a million short rows: 10-15 ms first call in a backend (under 1 ms cached) without, under 1 ms with
- **Formula:** S 124.00x saving 111200k instructions, G 100.0%, W 92.6%, R 0.5: B 49.4 against C 51.9, opt-in (fails: cost)
- **Impact:** index size: a million rows: 17 MB, 34 MB at n=3, 73 MB at n=15 (2 to 4x); 300,000 rows 8.4 to 20 MB; build, merge and writes: n = 3: CREATE INDEX +92.6% instructions (1,321 M to 2,544 M, 200,000 rows); build 428 to 854 ms, insert of 20,000 documents 72 to 266 ms; n = 15: build 385 to 1,544 ms, insert 67 to 549 ms (100,000 documents); other: a larger index (more terms, growing with n); REINDEX to change
- **When:** Prefix queries (ab*) of up to n characters; longer prefixes, wildcard, regex and fuzzy leaves still expand terms. n up to 15. Pairs with fields for autocomplete.
- **Default:** 0 — 0 (off): grows the index 2 to 4x (17 to 34 to 73 MB on a million rows), over the 25% ceiling, build 2-4x and insert 3.7-8x; broad prefixes already work from the per-backend cache (sift.prefix_cache_size).
- **Why not the default:** the index grows over 25% (retired: growth is a term of C, the cost), the benefit B does not reach the cost C
- **Turn off:** Default 0. ALTER INDEX .. RESET (prefix_index) or SET (prefix_index = 0) then REINDEX; queries and inserts fail until then, so prefer CREATE INDEX CONCURRENTLY without it and drop the old index.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 0: broad prefixes work without it (cached per backend). Set n to the longest prefix that must be sub-millisecond on first use; it grows the index. Needs REINDEX.

#### sift(fields)

`BM25` · opt-in · since 1.0 · default ''

- **What it does:** weights and queries over named sections of the indexed text
- **Gain:** -
- **Impact:** index size: two sections 15.6 MB against 8.3 MB for the first alone (1,000,000 rows); build, merge and writes: build 489 to 973 ms, insert of 20,000 documents 107 to 340 ms with three fields declared against none (100,000 documents); other: REINDEX to change
- **When:** Text built with sift.sections() whose leading sections are named; queries name:word, sift.field_query. At most 14 names. Phrases, regex and NOT are not scoped to a field; one analyzer for all.
- **Default:** '' — Unset: named sections add 88% to the index (15.6 against 8.3 MB on a million rows) and insert 107 to 340 ms with three fields, and a colon is plain text without it.
- **Why not the default:** changes results or semantics, the index grows over 25% (retired: growth is a term of C, the cost)
- **Turn off:** Default '' (no sections). ALTER INDEX .. RESET (fields) then REINDEX; queries and inserts fail until then. Prefer CREATE INDEX CONCURRENTLY without fields and drop the old index.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave unset. Set it with sift.sections() to weight or restrict a query to named leading sections (name:word, name^3); pair with prefix_index for autocomplete. Needs REINDEX.

#### sift_col(point_columns)

`sift_col` · opt-in · since 3.3 · default ''

- **What it does:** an equality, IN list or short range on a column of many distinct keys reads only the units that hold the rows, which the heap then checks again: integer equality 0.119 to 0.020 ms (200,000 rows; a b-tree 0.006)
- **Gain:** integer equality 0.119 to 0.020 ms, IN of 5 keys 0.193 to 0.028 ms (200,000 rows; a b-tree 0.006 and 0.009)
- **Formula:** S 2.03x saving 804k instructions, G 58.0%, W 3.4%, R 1: B 14.5 against C 20.0, opt-in (fails: cost)
- **Impact:** index size: index 29.1 to 93.7 MB (+222%) for five leaves at 1,000,000 rows, 170 MB for six b-trees; 4 bytes a row for repeating keys, 4.2 MB for a unique key (200,000 rows); build, merge and writes: build 66 to 94 ms (+42.7%); insert of 20,000 rows 24 to 23 ms and update of 10% of the rows 53 to 52 ms: within noise (100,000 rows); other: format minor 14; 4 bytes a row for keys that repeat (0.8 MB for 200,000 rows), 4.2 MB for a unique key; a leaf with overflow values or inline strings gets no table
- **When:** Equality, IN list (up to 65,536 keys; over 64 only under a sixteenth of the rows) or range of up to 64 keys on integer, text, uuid columns, in access-method and aggregate scans. Overflow values: no directory.
- **Default:** '' — Opt-in: an equality on a unique id reads 1.58 M instructions with the key filter and 0.78 M with the directory (2.03x), for +58% of the index and +3.4% of the build: the benefit (14.5) is under the cost (20.0) and the growth is over 25%. A lookup stays 3x slower than a b-tree (0.020 against 0.006 ms).
- **Why not the default:** the index grows over 25% (retired: growth is a term of C, the cost), the benefit B does not reach the cost C
- **Turn off:** RESET (point_columns): new segments get no directory; older ones keep theirs until a merge or REINDEX. sift.col_format_minor <= 13 writes none. No directory, no format change to undo.
- **Upgrade from 3.6:** unchanged
- **Advice:** Opt-in: set it only where the b-tree on the column can go and a lookup 3x slower is fine. Prefer point_tid_columns, which reads no unit. Larger index; REINDEX to apply.

#### sift_col(point_tid_columns)

`sift_col` · opt-in · since 3.3 · default ''

- **What it does:** an equality or IN list on a column of many distinct keys returns the addresses of its rows from the directory with no unit read, and the heap checks them again: integer equality 0.013 ms at 26,700 to 1,000,000 rows, flat (point_columns 0.015 to 0.022; a btree 0.006); a unit with a dead row is read instead
- **Gain:** integer equality 0.013 ms flat from 26,700 to 1,000,000 rows (point_columns 0.015-0.022, b-tree 0.006); 100-id IN 45.7 to 0.25 ms (1,000,000 rows)
- **Formula:** S 2.51x saving 1002k instructions, G 57.6%, W 3.6%, R 1: B 18.9 against C 19.9, opt-in (fails: cost)
- **Impact:** index size: within 3% of point_columns: 91.4 against 93.7 MB, 29.1 MB without a directory, 170 MB for six b-trees (5 leaves, 1,000,000 rows); build, merge and writes: build 1.32 against 0.80 s (+65%), promotion of 100,000 rows 92 against 53 ms (+5% of insert plus promotion), inserts unchanged; other: format minor 16; the size of point_columns within 3% (a key of many rows takes 1 to 3 bytes a row, not 4); the build 65% longer, a promotion 5% longer, inserts unchanged; not for an index-only scan, which reads the units the addresses name
- **When:** Leaves of point_columns: an equality or IN list of up to 64 keys returns heap addresses with no unit read. A unit with a dead row is read; an index-only scan reads units; a key_cut segment keeps the rows form.
- **Default:** '' — Opt-in: an equality on a unique id reads 1.67 M instructions with the key filter and 0.66 M with the addresses (2.51x), for +57.6% of the index and +3.6% of the build: the benefit (18.8) is under the cost (19.9) and the growth is over 25%; minor 16 is read by 3.3 and later only.
- **Why not the default:** the index grows over 25% (retired: growth is a term of C, the cost), the benefit B does not reach the cost C
- **Turn off:** RESET (point_tid_columns); a leaf kept in point_columns then has the rows form. New segments only; REINDEX rewrites older ones; sift.col_format_minor <= 14 writes no addresses.
- **Upgrade from 3.6:** unchanged
- **Advice:** Opt-in where the b-tree on an id or key can go: lookups of up to 64 keys read no unit. Not for index-only scans; a unit with a dead row is read. Larger index; REINDEX to apply.

#### sift_col(order_columns)

`sift_col` · opt-in · since 3.3 · default ''

- **What it does:** ORDER BY col LIMIT n reads the first rows of each unit only, and a LIMIT of up to 32 over units that are all visible and clean reads no unit
- **Gain:** ORDER BY an integer LIMIT 100 37.9 ms normal plan, 3.5 without runs, 1.4 with (1,000,000 rows); LIMIT 10 0.128 to 0.051 ms with heads (800,000)
- **Formula:** S 1.08x saving 1383k instructions, G 8.3%, W 2.5%, R 1: B 1.6 against C 3.3, opt-in (fails: cost)
- **Impact:** index size: 2 bytes a row per unordered leaf (+50.8% of a 10-column index with 8 unordered leaves, 1,000,000 rows); heads add 0.6% to 2.4% (three order columns); build, merge and writes: with heads (not the runs themselves): build 1.61 against 1.63 s, promotion of 100,000 rows 0.052 s, merge of 500,000 rows 0.74 against 0.66 s; other: format minor 18; the row ordinals per column: 1 B a row (units up to 256 rows), 2 B (to 65,536), else 4 B; 16 B a unit for a column loaded in order; the head of the run (the best 32 keys at each end with their heap addresses) adds 0.24 B a row for a column with no NULL (units of 4,096 rows or more, less for smaller units)
- **When:** ORDER BY col LIMIT n on one sort key (integer, date, timestamp, numeric or float columns) reads the first rows of each unit; LIMIT up to 32 with no condition, join or aggregate over clean units reads none.
- **Default:** '' — Opt-in: an ORDER BY a date LIMIT 100 reads 18.3 M instructions without the runs and 16.9 M with them (1.08x), for +8.3% of the index and +2.5% of the build: the benefit (1.6) is under the cost (3.3); an integer column with few distinct values gains 1.04x. ORDER BY an integer LIMIT 100: 3.5 to 1.4 ms (37.9 ms normal plan).
- **Why not the default:** the benefit B does not reach the cost C
- **Turn off:** RESET (order_columns): new units get no run; older ones keep theirs until a merge or REINDEX. sift.col_format_minor <= 12 writes no runs; 13 to 14 writes runs without heads.
- **Upgrade from 3.6:** unchanged
- **Advice:** Opt-in for ORDER BY .. LIMIT n over a column spread over every unit; keep a b-tree for LIMIT of over 1000. Grows the index by 1 to 4 bytes a row per column. Applies to units written afterwards.

#### sift_col(hll_columns)

`sift_col` · opt-in · since 3.4 · default ''

- **What it does:** sift.approx_count_distinct(col) of the whole table or of the units a test passes in full is the merge of the stored sketches of its units, not a hash of every row: 5.4 ms at 1,600,000 rows against 63 ms for the same aggregate by the normal plan; the number is the same in every plan, count(DISTINCT) stays exact
- **Gain:** 5.4 ms against 63 ms for the same aggregate by the normal plan (2 leaves, 1,600,000 rows); bench case approx_distinct_sketch 109 ms against 6.4 ms (1,000,000 rows)
- **Impact:** index size: +23% of the index for four columns of a million rows (169 B to 12,307 B a unit and column, by distinct values); build, merge and writes: build +70% for four columns (about +15% a leaf); other: format minor 20; a sketch of 8 B + 3 B a register to 12,296 B a unit and column (169 B for 50 values, 8,209 B for 2,700, 12,307 B above 4,096): the index 23% larger for four columns of a million rows, the build 70% longer; segments are minor 20, which 3.3 and older refuse
- **When:** sift.approx_count_distinct(col) of the table or of units a filter passes in full; count(DISTINCT) stays exact. Refused: char(n), name, float, numeric, boolean, nondeterministic collation, jsonb-path, key_columns leaves.
- **Default:** '' — Opt-in: +23% of the index for four columns of a million rows and build +70% (169 B to 12.3 kB a unit and column), over the 3% and 5% limits; 5.4 against 63 ms; minor 20 is refused by 3.3 and older.
- **Why not the default:** changes results or semantics
- **Turn off:** RESET (hll_columns): new units get no sketch; older units keep theirs until a merge or REINDEX, and approx_count_distinct reads rows of units without one. sift.col_format_minor <= 14 writes none.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave unset unless a dashboard needs an approximate distinct count of a high-cardinality column: sift.approx_count_distinct(col) is the same number in every plan and count(DISTINCT) stays exact; list only the columns that ask for it, each costs up to 12 kB a unit.

#### sift_col(return_columns)

`sift_col` · opt-in · since 3.3 · default ''

- **What it does:** an index-only scan returns these columns from the dictionary of the segment, not the heap
- **Gain:** slower on a table that fits in memory: SELECT id of 100 rows 0.066 to 0.158 ms, 10,000 rows 1.49 to 1.81 ms (300,000 rows); on-disk tables not measured
- **Impact:** index size: no extra storage (reads the segment dictionary); dict_lookup auto adds the string order, +10% to +70% of a one-column index; build, merge and writes: -; other: no extra storage: the strings are the dictionary the index keeps
- **When:** text, varchar, uuid columns an index-only scan returns from the segment dictionary (tuple scans, sift.enable_col_scan on). Integer, date, timestamp, boolean, float, numeric columns need no option; char(n) never.
- **Default:** '' — Opt-in: with the heap in memory it is slower (SELECT id of 100 rows 0.066 to 0.158 ms, 300,000 rows) because the executor asks for every returnable column; meant for tables much larger than memory, not measured on disk.
- **Why not the default:** the speedup S is under 1.05, or saves under 100k instructions a query, not run on real workloads
- **Turn off:** RESET (return_columns) or 'off': no format change, no REINDEX; a query that needs one of these columns reads the heap again.
- **Upgrade from 3.6:** unchanged
- **Advice:** Opt-in: an index-only scan returns text, varchar and uuid columns from the index. Use it for tables much larger than memory, where heap fetches are disk reads; otherwise the heap is faster.

#### sift.parallel_scan

`BM25` · opt-in · since 1.1 · default off

- **What it does:** parallel SiftScan and SiftCount plans
- **Gain:** unordered scan with a heavy residual qual 1.8x (218,058 documents); other shapes lose to the Gather cost of milliseconds
- **Impact:** index size: -; build, merge and writes: -; other: off because a Gather costs milliseconds: only an unordered scan with a heavy residual qual gained (1.8x on 218,058 articles)
- **When:** Planning with a Gather over a parallel SiftScan or SiftCount; a ranked one needs a LIMIT and one table, never with a pushed-down filter or partitions. Wins only on large heap-bound scans or expensive quals.
- **Default:** off — off: a Gather costs milliseconds before any work; on 218,058 articles only an unordered scan with a heavy residual qual gained (1.8x), other shapes lost, so the speedup is under 1.05 on most relevant classes.
- **Why not the default:** the speedup S is under 1.05, or saves under 100k instructions a query
- **Turn off:** Already off. SET sift.parallel_scan = on to try it; RESET (or off) returns the serial plans; read at planning time.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave off. Try on much larger tables or expensive quals: a ranked scan needs a LIMIT and one table, never with a pushed-down filter or partitions. Rows, scores and ties equal the serial plan.

#### sift.rls_scores

`BM25` · opt-in · since 3.3 · default off

- **What it does:** a ranked scan for a role under row-level security
- **Gain:** ranked top-10, 100,000 documents, 50% / 5% / 0.5% visible: 0.15 to 0.9 ms; the plan the role had before took 5 to 31 ms, unranked
- **Impact:** index size: -; build, merge and writes: -; other: scores use the statistics of the whole index, hidden rows included
- **When:** A role under a row-level security policy (or a security barrier view) on a table with a sift index: it gets ranked scans. The table owner already does. Superuser setting, set per role.
- **Default:** off — off for result: the scores use document frequencies and lengths of the whole index, hidden rows included, so a score depends on rows the role cannot see. On, the ranked top-10 took 0.15 to 0.9 ms against 5 to 31 ms.
- **Why not the default:** changes results or semantics
- **Turn off:** Already off: the role gets no ranked scan and sift.score() raises an error. Turn on with ALTER ROLE app SET sift.rls_scores = on (superuser); RESET returns off; a prepared plan keeps its choice.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave off. Set per role (superuser) only when a role under row-level security needs ranked scans; scores then use whole-index statistics, hidden rows included, so they depend on unseen rows.

#### sift.col_auto_merge_partition_kb

`sift_col` · opt-in · since 3.3 · default 0

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** 1,000,000 rows, limit 8 MB: units answerable 42% to 100% (uniform churn, grouped sum 7.4 to 0.10 ms) and 71% to 100% (8.4 to 0.16 ms)
- **Impact:** index size: -; build, merge and writes: VACUUM plus worker WAL equals the whole-index rewrite (15.3 against 15.05 MB uniform, 12.8 against 12.7 MB scraper a round); one step holds the lock for one partition; other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** Size of the segments the rewrite writes, so it rewrites only the ones churn touched, one step each under the lock. An index under half of it is rewritten whole; a segment under half is an overlay.
- **Default:** 0 — Opt-in (gain): same WAL as the whole-index rewrite (15.3 against 15.05 MB uniform) and an index over sift.vacuum_rewrite_limit is already partitioned; it pays for churn in one place (a hot tail) of a large index.
- **Why not the default:** the speedup S is under 1.05, or saves under 100k instructions a query
- **Turn off:** 0 (the default) rewrites the index whole, in partitions of the vacuum_rewrite_limit size above it or of a third of maintenance_work_mem where that is less (at least 4 MB, at most 48 for an index): SET sift.col_auto_merge_partition_kb = 0 or RESET at server or database level, and a size above the index puts back the one segment. Segments already cut stay.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 0 rewrites the index whole (partitions of the vacuum_rewrite_limit size above it, or of a third of maintenance_work_mem where that is less). Set a size to keep the index as segments of that size and rewrite only the ones churn touched.

#### sift.gpu

`sift_col`, `sift_table` · opt-in · since 3.1 · default off

- **What it does:** the integer aggregates of a scan run on a Vulkan device
- **Gain:** device vs one CPU core, chunks resident, 6 integer count/sum queries: 0.77x to 1.07x at 1,000,000 rows, 0.99x to 1.00x at 10,000,000 (3.2.0 pass, RTX 3090)
- **Impact:** index size: -; build, merge and writes: -; other: needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says
- **When:** sift_col on a VULKAN=1 build: integer count/sum/avg of clean all-visible units; auto takes grouped ones above sift.gpu_min_rows, on also ungrouped. Not floats, min/max, text filters, sift_table.
- **Default:** off — Off: the 3.2.0 pass found 0.77x-1.07x of one CPU core at 1M rows and 0.99x-1.00x at 10M (speedup under 1.05), at 0.2 s and about 100 MB per backend context; packages are built without Vulkan.
- **Why not the default:** the speedup S is under 1.05, or saves under 100k instructions a query, needs hardware, not run on real workloads
- **Turn off:** Default off. SET sift.gpu = off (superuser setting; ALTER ROLE/DATABASE .. SET or postgresql.conf). Without a VULKAN=1 build the CPU runs whatever it says; a failing device is dropped for the session.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave off. Needs make VULKAN=1 and a Vulkan device, and persistent connections (0.2 s and about 100 MB of context per backend). Measure first: the 3.2.0 pass shows no gain over the CPU path.

#### sift.gpu_min_rows

`sift_col`, `sift_table` · opt-in · since 3.1 · default 5000000

- **What it does:** the integer aggregates of a scan run on a Vulkan device
- **Gain:** as `sift.gpu`
- **Impact:** index size: as `sift.gpu`; build, merge and writes: as `sift.gpu`; other: needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says
- **When:** Only with sift.gpu = auto: a grouped aggregate runs on the device when the index has more rows than this; an ungrouped one only with sift.gpu = on. No effect with off or on.
- **Default:** 5000000 — 5,000,000: where auto starts; the 3.2.0 pass at 1M and 10M rows found no gain at either, so no value is shown to pay and the device stays off by default. Why exactly 5M is not argued.
- **Why not the default:** needs hardware, not run on real workloads
- **Turn off:** n/a: a threshold (0 to INT_MAX); sift.gpu = off is the switch. 0 lets auto take every eligible grouped aggregate.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 5,000,000: the index rows above which sift.gpu = auto uses the device for a grouped aggregate; an ungrouped one runs there only with sift.gpu = on.

#### sift.gpu_device

`sift_col`, `sift_table` · opt-in · since 3.1 · default ''

- **What it does:** the integer aggregates of a scan run on a Vulkan device
- **Gain:** as `sift.gpu`
- **Impact:** index size: as `sift.gpu`; build, merge and writes: as `sift.gpu`; other: needs a build with VULKAN=1 and a device; without them the CPU runs whatever this says
- **When:** Chooses the Vulkan device when sift.gpu is auto or on on a VULKAN=1 build; empty picks the best (discrete before integrated before virtual before software); a software device is taken only with on.
- **Default:** '' — Empty: the best device is picked by that rank, so no configuration is needed; set an index or part of a name only to choose another.
- **Why not the default:** needs hardware, not run on real workloads
- **Turn off:** n/a: it chooses a device. RESET sift.gpu_device (superuser) gives the best-device rule back; sift.gpu = off stops any use.
- **Upgrade from 3.6:** unchanged
- **Advice:** Empty picks the best device (discrete before integrated before virtual before software); set an index or part of a name to choose one. Superuser.

#### sift.table_async_promote

`sift_table` · opt-in · since 3.3 · default off

- **What it does:** the writer asks a worker to promote a closed buffer, taking the 25 ms pass out of its statement
- **Gain:** statements that close a buffer 25-31 ms to at most 2.8 ms; median 65 us, p99 190 us, p99.9 0.6 ms unchanged (single-row INSERTs, 100,000 rows)
- **Impact:** index size: -; build, merge and writes: 4 clients of single-row INSERT: tps and p99 equal within noise; other: off by default: 4 clients of single-row INSERT, tps and p99 equal within noise
- **When:** Writers of a sift_table that close a write buffer: a worker promotes it instead of the statement. Needs sift.async_maintenance and table_inline_promote; from half of the directory on the writer promotes itself.
- **Default:** off — off: the statements that close a buffer drop from 25-31 ms to at most 2.8 ms, but the median, p99 and tps are equal within noise (single-row INSERTs, 4 clients): the speedup of the relevant class is under 1.05.
- **Why not the default:** the speedup S is under 1.05, or saves under 100k instructions a query
- **Turn off:** Already off. SET sift.table_async_promote = on to use it; off or RESET returns the writer promoting itself. Session or role setting; pair with the worker settings.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave off. Set on when statements that close a write buffer must not stall for the promotion; needs sift.async_maintenance and table_inline_promote; past half the directory the writer promotes.

#### sift.table_backpressure

`sift_table` · opt-in · since 3.3 · default 0

- **What it does:** a writer that needs a new write buffer first waits for the maintenance lock and merges, when the directory passes this percent
- **Gain:** n/a: it acts only when the directory is nearly full, which a focused run reaches only with the test setting sift.table_max_segments
- **Impact:** index size: -; build, merge and writes: a writer past the percent waits for the maintenance lock and merges up to 16 steps instead of 4; other: the wait; 0 waits only when the directory is nearly full
- **When:** Writers of a sift_table needing a new write buffer while the 128-entry directory is past n percent: they wait for the maintenance lock and merge (16 steps, not 4). n is 0 or above 50. Heavy steady writers.
- **Default:** 0 — 0: opt-in for risk - it only adds a wait, and it acts only when the directory is nearly full, which a focused run reaches only with the test setting sift.table_max_segments; no gain figure exists.
- **Why not the default:** not run on real workloads
- **Turn off:** Already 0 (a writer waits only when the directory is nearly full). SET sift.table_backpressure = 0 or RESET returns it; session or role setting.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 0 (a writer waits only when the directory is nearly full). Set a percent above 50 for steady heavy writers so they merge before the directory fills, at the cost of the wait.

### diagnostic

#### sift(build_segments)

`BM25` · diagnostic · since 1.0 · default 0

- **What it does:** forces N segments at build and turns the parallel build off
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: serial build (parallel workers off); other: a serial build
- **When:** CREATE INDEX and REINDEX only: N forced segments and a serial build; 0 writes one per build participant. Up to 64. Used by tests of VACUUM and merges.
- **Default:** 0 — 0: a diagnostic switch; N > 0 turns the parallel build off (a serial build is its only effect), so it is never a default.
- **Turn off:** Leave 0 or ALTER INDEX .. RESET (build_segments); it only acts at the next CREATE INDEX or REINDEX, which then builds in parallel again.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 0. A test and measurement switch: CREATE INDEX writes N forced segments (a many-segment index for VACUUM and merge tests) and runs serially. Needs REINDEX to change.

#### sift_col(cluster_advice)

`sift_col` · diagnostic · since 3.3 · default true

- **What it does:** sift.cluster_advice() may recommend a physical order or a key_cut for the table, and CREATE INDEX may suggest key_cut
- **Gain:** advised sort: a key equality reads 196 units in a random heap, 18 sorted by date, 1 sorted by the key (200,000 rows)
- **Impact:** index size: -; build, merge and writes: -; other: none; maintenance only, not part of the format
- **When:** sift.cluster_advice() and the CREATE INDEX / REINDEX notice for this index. Advice only: it names a repack order or key_cut and performs nothing; a small table (under 16 units) or an ordered one gets not recommended.
- **Default:** true — On: it stores nothing (maintenance only, not part of the format), so there is no growth or write cost to weigh and no behavior changes; it is a diagnostic.
- **Turn off:** ALTER INDEX .. SET (cluster_advice = off) silences it for this index; nothing stored changes, no REINDEX. SET sift.cluster_advice = off does it for a session, role or database.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: it only lets sift.cluster_advice() and the CREATE INDEX notice recommend a physical order or key_cut. Turn it off per index to silence the advice; nothing stored changes.

#### sift.enable_scan

`BM25` · diagnostic · since 1.0 · default on

- **What it does:** switches of the ranked scan, the count(*) pushdown, the bitmap of the other quals and the read-ahead
- **Gain:** no consistent difference at 200,000 rows (sub-millisecond: first 10 ids of a match 0.32 ms off, 0.42 on for rare terms; 0.92 off, 0.83 on for frequent ones); sift.score() exists only inside the scan
- **Impact:** index size: n/a: a planner switch; it stores nothing; build, merge and writes: n/a: a planner switch; it adds no write work; other: none; off gives the plan PostgreSQL has without them
- **When:** Planning of a statement on a table with a sift index: the ranked and unordered sift scan. Off, the plan is PostgreSQL's own (bitmap or sequential scan); sift.score() needs the scan. Not sift_col scans.
- **Default:** on — On: it is the point of the index. The setting is a diagnostic to compare against the plan PostgreSQL has without sift, and the regression tests use it for the sequential reference.
- **Turn off:** SET sift.enable_scan = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf); RESET returns on. Read when a statement is planned, so a prepared plan keeps its choice.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. enable_scan, enable_count, filter_pushdown and prefetch switch the ranked scan, the count(*) pushdown, the bitmap of other quals and the read-ahead; off compares with PostgreSQL's own plan.

#### sift.enable_count

`BM25` · diagnostic · since 1.0 · default on

- **What it does:** switches of the ranked scan, the count(*) pushdown, the bitmap of the other quals and the read-ahead
- **Gain:** count(*) of a match 0.37 to 0.29 ms (rare terms) and 0.40 to 0.33 ms (frequent terms), 200,000 documents: about 20% faster, sub-millisecond
- **Impact:** index size: as `sift.enable_scan`; build, merge and writes: as `sift.enable_scan`; other: none; off gives the plan PostgreSQL has without them
- **When:** SELECT count(*) of a sift-matched table: pushed into the index. Off, count(*) is planned without the pushdown; ranked and unordered scans are not affected.
- **Default:** on — On: the count answers from the index; the setting is a diagnostic to compare plans.
- **Turn off:** SET sift.enable_count = off (session, ALTER ROLE / DATABASE .. SET, postgresql.conf); RESET returns on.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Off plans count(*) without the index pushdown, to compare plans; see sift.enable_scan.

#### sift.log_min_duration

`all` · diagnostic · since 2.0 · default 1000 ms

- **What it does:** logs promotions, merges and worker runs of at least that long
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: none; a log line
- **When:** Promotions, merges and maintenance worker runs of any index kind that last at least n ms; each is one log line. Superuser setting; -1 logs none, 0 logs all.
- **Default:** 1000 ms — 1000 ms: only steps of a second or more are logged; a diagnostic with no gain, growth or write cost.
- **Turn off:** SET sift.log_min_duration = -1 (superuser: postgresql.conf, ALTER SYSTEM, ALTER DATABASE .. SET); RESET returns 1000.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 1000 ms. Logs promotions, merges and worker runs that take at least that long; lower it to see short ones, -1 logs none.

#### sift.simd

`all` · diagnostic · since 1.0 · default avx512

- **What it does:** caps the SIMD level of every kernel; every level returns the same results bit for bit
- **Gain:** column kernels against scalar, 4096-row vectors (x86-64): reductions 4x to 27x, 2x to 10x under a 50% mask
- **Impact:** index size: -; build, merge and writes: -; other: none; a lower level only slows the kernels (auto is the best level of the CPU, shown as avx512 on x86-64 and as neon on AArch64, whose levels are scalar and neon; the level names of the other architecture cap nothing, so one configuration file serves a mixed fleet)
- **When:** Every kernel (BM25 scoring, column filters, aggregation, codec) on x86-64 (scalar, avx2, avx512) and AArch64 (scalar, neon); a level of the other architecture caps nothing. Results are bit for bit equal at every level.
- **Default:** avx512 — auto (the best level of the CPU, shown as avx512 or neon): column reduction kernels 4x to 27x faster than scalar (x86-64, 4096-row vectors); a lower level only slows them. Written data never depends on the level.
- **Turn off:** SET sift.simd = scalar to run the specification (session, ALTER ROLE / DATABASE .. SET, postgresql.conf); avx2 caps AVX-512 hosts; auto restores the best level.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave the default (the best level of the CPU; every level returns the same results and written data never depends on it). Lower it only to compare levels; scalar is the specification.

#### sift.enable_col

`sift_col` · diagnostic · since 3.0 · default on

- **What it does:** the column scans over sift_col indexes
- **Gain:** column scans against the normal plan, 1,000,000 rows: count/sum of the table 60.6 to 0.11 ms, GROUP BY month 320 to 0.74 ms, count(DISTINCT) 108 to 0.38 ms
- **Impact:** index size: -; build, merge and writes: -; other: none; off gives the plan PostgreSQL has without them
- **When:** Column scans (SiftAgg) over sift_col indexes in the session; off gives the plan PostgreSQL has without them. It changes nothing stored.
- **Default:** on — On: against the normal plan (1,000,000 rows) count/sum 60.6 to 0.11 ms, GROUP BY month 320 to 0.74 ms, count(DISTINCT) 108 to 0.38 ms; it has no storage or write cost of its own.
- **Turn off:** SET sift.enable_col = off for a session (ALTER ROLE/DATABASE or postgresql.conf for more); used to compare plans and answers. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. SET it off for one statement to see the plan PostgreSQL has without sift_col indexes, or to compare answers and times.

#### sift.cluster_advice

`sift_col` · diagnostic · since 3.3 · default on

- **What it does:** sift.cluster_advice() may recommend a physical order or a key_cut for the tables, and CREATE INDEX may suggest key_cut
- **Gain:** n/a: it reports advice and changes no plan; the gain is that of the order or key_cut it recommends (see sift_col(key_cut))
- **Impact:** index size: -; build, merge and writes: -; other: none; maintenance only
- **When:** sift.cluster_advice() and the CREATE INDEX / REINDEX notice for every sift_col index in the session. Advice only: nothing is rewritten. The index option cluster_advice switches one index.
- **Default:** on — On: it stores nothing (maintenance only), so there is no growth or write cost to weigh and no behavior changes; it is a diagnostic.
- **Turn off:** SET sift.cluster_advice = off (session, ALTER ROLE/DATABASE, postgresql.conf): the function answers not recommended and CREATE INDEX does not suggest key_cut. Nothing stored changes.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on: sift.cluster_advice() recommends a physical order or key_cut and CREATE INDEX may print the advice. Off answers not recommended and prints nothing; the index option switches one index.

#### sift.agg_verdict_cache

`sift_col`, `sift_table` · diagnostic · since 3.5 · default on

- **What it does:** a plan that does not depend on the statements planned before it: the choice between a shared CTE and its inline plan, and between the flattened form of a statement and the form as written, is made in full for every statement (support, benchmarks, comparing two plans)
- **Gain:** plan-gate tables, 30,000 rows, one statement of three scans over a shared CTE planned 64 times with other constants, 3 rounds on and off in turn, instructions of the backend (perf stat): 4.66 million a statement on, 4.61 million off
- **Impact:** index size: -; build, merge and writes: -; other: every such statement is planned in full: the shared and the inline plan, and the larger sizes of sift.agg_scale_horizon for a close call; on three scans over the 30,000 rows of the plan-gate tables 4.61 million user instructions and 0.7 ms of planning off against 4.66 million and 0.7 ms on (no measurable difference at that size); a 27 KB dashboard statement (21 CTEs, 16 lateral sub-selects) over 3,000,000 rows takes 128 ms to plan in full against 11 ms from the verdict
- **When:** Statements that read a shared CTE several times or a lateral aggregate that can be flattened; no other statement consults the cache.
- **Default:** on — the planning of the choice is 4 to 6 times cheaper from a verdict (verdict.c) and an entry is planned in full only at its eighth read and then every 16th, 32nd and 64th; the plan is the same except for a statement whose two variants cost about the same, and the rows are the same either way
- **Turn off:** SET sift.agg_verdict_cache = off (session, role, database or postgresql.conf).
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. Turn it off to get the same plan for a statement whatever ran before it (a support case, a benchmark, a comparison of two plans); it costs the planning of the choice for every statement.

#### sift.explain_refusals

`sift_col`, `sift_table` · diagnostic · since 3.3 · default off

- **What it does:** a notice says why a query kept the normal plan instead of SiftAgg
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: none when off; the plans are the same on or off
- **When:** A statement with a candidate SiftAgg path that kept the normal plan: one notice with the slug and the first reason each path gave up for; a statement that used SiftAgg gives none.
- **Default:** off — Off: a diagnostic that sends a NOTICE to the client; with it off there is no plan change and no cost.
- **Turn off:** Default off. SET sift.explain_refusals = off or RESET (session, role, database or postgresql.conf); sift.explain_why(query) asks without running. sift.count_refusals counts on its own.
- **Upgrade from 3.6:** unchanged
- **Advice:** Set it in a session to see why a query kept the normal plan (a notice with the slug and reason); the plans are the same on or off. sift.explain_why(query) asks without running it.

#### sift.count_refusals

`sift_col`, `sift_table` · diagnostic · since 3.3 · default off

- **What it does:** a notice says why a query kept the normal plan instead of SiftAgg
- **Gain:** as `sift.explain_refusals`
- **Impact:** index size: as `sift.explain_refusals`; build, merge and writes: as `sift.explain_refusals`; other: a few microseconds a query that reaches a candidate path
- **When:** Queries that reach a candidate SiftAgg path and keep the normal plan: counted per session in sift.stats() as refused.<slug> and limit.<id>, without the notice.
- **Default:** off — Off: a diagnostic that costs a few microseconds a query that reaches a candidate path, so it is kept for those who read sift.stats().
- **Turn off:** Default off. SET sift.count_refusals = off or RESET (session, role, database or postgresql.conf); the notice is sift.explain_refusals.
- **Upgrade from 3.6:** unchanged
- **Advice:** Set it to count why queries kept the normal plan in sift.stats() (refused.<slug>, limit.<id>) without the notice; leave off otherwise.

#### sift.plan_scale

`sift_col`, `sift_table` · diagnostic · since 3.3 · default 1

- **What it does:** plan as if the tables with a sift_col index had this many times the pages and rows, to see the planner's choice at a larger size (on every supported PostgreSQL)
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: none at 1; a diagnostic
- **When:** Planning only, tables with a sift_col index: pages and rows are multiplied; other tables and column statistics stay. Compare EXPLAIN or sift.explain_why at 10 and 100. It does not change the data a query reads.
- **Default:** 1 — 1 plans the table as it is: a diagnostic that nothing sets for you, so a plan never changes unless you ask. Range 1 to 1,000,000.
- **Turn off:** SET sift.plan_scale = 1 (or RESET) in the session; planning only, nothing to rebuild.
- **Upgrade from 3.6:** unchanged
- **Advice:** Set 10 or 100 in a session and compare EXPLAIN or sift.explain_why to see the planner's choice at a larger size without the data. 1 plans the table as it is.

#### sift.agg_force

`sift_col`, `sift_table` · diagnostic · since 3.4 · default off

- **What it does:** takes the aggregate scan wherever one is possible, whatever the plans cost, to time the plan the cost model refused
- **Gain:** 27 nested-list statements, 31.7M rows, 32 keys, work_mem 4GB: 136.8 s plain, 117.4 s planner's choice, 6.3 s forced; statements of 15 to 120 ms get slower
- **Impact:** index size: -; build, merge and writes: -; other: none; off; a diagnostic: the scan is priced as free and an inline shared CTE with a scan is kept
- **When:** Statements where a scan can be built but the cost model keeps the normal plan (refusals cost, cte_cost); an arm refused for its shape stays refused; requests of 15-120 ms get slower.
- **Default:** off — Off: 27 nested-list statements, 31.7M rows: 136.8 s plain, 117.4 s planner's choice, 6.3 s forced, but small requests get slower, so it is a diagnostic; SET LOCAL per statement.
- **Turn off:** Default off. SET sift.agg_force = off or RESET; ALTER ROLE .. RESET sift.agg_force undoes a role setting. The cost-based choice returns at the next plan.
- **Upgrade from 3.6:** unchanged
- **Advice:** A diagnostic first: it takes the scan whatever the cost model says, to time a refused plan. Set it for a role only if its requests are large; SET LOCAL it per statement. Small requests get slower.

### safety

#### sift.max_expansions

`BM25` · safety · since 1.0 · default 512

- **What it does:** bounds the terms a wildcard, regex, fuzzy or prefix leaf expands to
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: above it a prefix matches all its terms but scores as one term
- **When:** Wildcard, regex, fuzzy and prefix leaves of a sift index: above n terms the leaf still matches every word but scores as one term. Below it a prefix scores as the best of its terms. Superuser setting.
- **Default:** 512 — 512: a safety bound on terms scored one by one; above it a leaf matches all words and scores as one term instead of failing (*ing on 100,000 documents: 90-170 ms). No other value is measured.
- **Turn off:** n/a: a limit (1 to 1000000). Raise it (postgresql.conf, ALTER SYSTEM, ALTER DATABASE / ROLE .. SET; superuser) to score more terms individually; RESET returns 512.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 512. Terms a wildcard, regex, fuzzy or prefix leaf scores one by one; above it the leaf still matches every word but scores as one term (*ing on 100,000 documents: 90-170 ms). Superuser.

#### sift.standby_conflicts

`all` · safety · since 2.0 · default on

- **What it does:** a standby without hot_standby_feedback waits for or cancels the queries that may read reused blocks, instead of failing them
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: replay can wait up to max_standby_streaming_delay for a standby query; other: replay can wait up to max_standby_streaming_delay
- **When:** Primary side, any index kind, when a merge's freed blocks are reused and a standby runs without hot_standby_feedback; with feedback or no standby nothing changes. Superuser setting.
- **Default:** on — On since 3.3 (off in 3.2 and earlier) so a standby query waits up to max_standby_streaming_delay, then is cancelled as for a heap, not failed at once with a retryable error; costs only replay waits.
- **Turn off:** SET sift.standby_conflicts = off (superuser: postgresql.conf, ALTER SYSTEM): a standby query reading reused blocks fails at once, retryable ('segment recycled during scan'). hot_standby_feedback = on avoids both.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave on. A standby without hot_standby_feedback waits (up to max_standby_streaming_delay) or cancels a query that may read reused blocks, as for a heap; off fails it at once with a retryable error.

#### sift.maintenance_cost_delay

`all` · safety · since 2.0 · default 0

- **What it does:** promotion and merges run in a background worker that the settings below throttle
- **Gain:** -
- **Impact:** index size: as `sift.async_maintenance`; build, merge and writes: worker merge of a 10 MB index: 0.27 s at 0, 5.3 s at 20 ms (at most 6.4 MB/s of I/O); other: a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O)
- **When:** Merges of the background worker (not its promotions, not VACUUM, sift.merge() or inserters' merges), per database; also the rewrites of sift_col partitions. Sleeps n ms per 16 pages (128 kB).
- **Default:** 0 — 0: no throttle, as before the setting existed; a merge of a 10 MB index took 0.27 s at 0 and 5.3 s at 20 ms. Opt in only where merges compete with queries for I/O.
- **Turn off:** Already 0 (no sleep). Set a few ms (superuser: ALTER DATABASE .. SET or postgresql.conf) to throttle; RESET returns 0.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 0 unless merges compete with queries for I/O; then set a few ms per 128 kB (per database). It slows only the worker, not VACUUM, sift.merge() or the merges inserters do for room.

#### sift.maintenance_wal_rate

`all` · safety · since 3.3 · default 0

- **What it does:** promotion and merges run in a background worker that the settings below throttle
- **Gain:** -
- **Impact:** index size: as `sift.async_maintenance`; build, merge and writes: as `sift.async_maintenance`; other: a merge of a 10 MB index took 0.27 s at cost delay 0 and 5.3 s at 20 ms (at most 6.4 MB/s of I/O)
- **When:** Merges of the background worker (not VACUUM, sift.merge() or inserters' merges): a merge waits before the page that would pass n kB/s of WAL; adds to maintenance_cost_delay. Superuser setting.
- **Default:** 0 — 0 (no limit) keeps the earlier behaviour; set it to bound the WAL so replicas can replay it. No gain, growth or write figure is measured.
- **Turn off:** Already 0 (no limit). Set kB/s (superuser: postgresql.conf, ALTER DATABASE .. SET) to cap; RESET returns 0.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 0 (no limit). Set kB/s to cap the WAL the worker's merges write so replicas can replay it; a merge waits before the page that would pass the rate, and it adds to maintenance_cost_delay.

#### sift.vacuum_rewrite_limit

`sift_col`, `sift_table` · safety · since 3.2 · default 128 MB

- **What it does:** bounds the megabytes a column VACUUM rewrites to merge, and moves from the end of the file
- **Gain:** -
- **Impact:** index size: with vacuum_truncate: table and indexes after VACUUM 200.3 to 154.5 MiB (1,000,000 rows after churn); build, merge and writes: default 128 MB of segments rewritten by one VACUUM, plus an equal allowance to move the end of the file in each of up to 64 rounds, the lock let go between them (a rewritten index of 105 MB live: file 210 to 115 MiB after VACUUM, in 12 MB rounds; one segment of 75.3 MB above 76.9 MB of free blocks: 152.2 to 75.35 MB in 6 rounds); 0 keeps the one merge step and moves nothing; the move holds an exclusive lock; other: the VACUUM that moves the end of the file holds the table or index under an exclusive lock
- **When:** VACUUM of sift_col and sift_table: merge work past the first step, and a separate allowance, for each of up to 64 rounds, to copy tail segments, or the highest runs of a segment larger than it, into free blocks before truncation. col_auto_merge rewrites an index over it in steps of that size.
- **Default:** 128 MB — 128 MB (about 6 million rows of index) bounds the rewrite and the exclusive-lock copy of one VACUUM; the first merge always runs; with truncation, table and indexes 200.3 to 154.5 MiB after churn (1,000,000 rows).
- **Turn off:** SET sift.vacuum_rewrite_limit = 0 (session, ALTER ROLE/DATABASE, postgresql.conf): one merge step and no file move; the free end is still truncated unless vacuum_truncate = false.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 128. Raise it for a faster return of space after heavy churn; set 0 to forbid the file move and long locks. An index over it is rewritten in steps of that size by col_auto_merge.

#### sift.col_auto_merge_wal_pct

`sift_col` · safety · since 3.3 · default 5

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: rewrites kept within 5% of the server WAL: total WAL per round equal to VACUUM only (299.0 / 304.7 MB with full-page images; 24.4 / 24.3 MB without, against 29.2 MB unbudgeted, 1,000,000 rows); other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** Gate of the whole-index rewrite: it waits until the server wrote 100 / pct times the bytes of the index since it was last written. Not applied to promoting the buffer or to sift.col_maintain(index). 0 to 100.
- **Default:** 5 — 5 bounds the rewrites at a twentieth of the server WAL: +0.9% to +1.9% total WAL with full-page images, +0.0% without, against +12% to +28% rewriting at every VACUUM (1,000,000 rows).
- **Turn off:** SET sift.col_auto_merge_wal_pct = 0 (ALTER SYSTEM or ALTER DATABASE) removes the wait: a rewrite at every VACUUM that finds the index due, +12% to +28% WAL. sift.col_maintain already ignores it.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at 5: the whole-index rewrite waits until the server wrote 100 / 5 times the bytes of the index since it was last written. Raise it for sooner rewrites; 0 does not wait. col_maintain ignores it.

#### sift.col_auto_merge_max_active

`sift_col` · safety · since 3.3 · default 0

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: as `sift.col_auto_merge`; other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** The rewrite worker waits (naptime, doubling, 8 tries) while more than N client backends of the database run a statement; the next VACUUM asks again. 0 to 10000.
- **Default:** 0 — 0: never waits, the behavior before. A bound for servers where rewrites must stay off busy moments; it has no measurement of its own.
- **Turn off:** 0 (the default) never waits: SET sift.col_auto_merge_max_active = 0 or RESET at server or database level (ALTER SYSTEM, ALTER DATABASE).
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 0 never waits. Set N to make the rewrite worker wait (with a backoff, 8 tries) while more than N client backends of the database run a statement, to keep rewrites off busy moments.

#### sift.col_auto_merge_max_workers

`sift_col` · safety · since 3.3 · default 0

- **What it does:** after churn a worker vacuums the table and merges the segments into one, so that scans stop checking rows in the heap and units answer from pre-aggregates again: 5% of 200k rows updated, a grouped sum 10.8 ms becomes 0.24 ms (2.6 ms after the VACUUM alone); 3% churn per round on 1M rows, the units answerable from the pre-aggregates 32% to 51% (wide rows) and 33% to 83% (a scraper mix), 100% right after a rewrite
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: as `sift.col_auto_merge`; other: background WAL and I/O: the table is vacuumed every five naptimes while it keeps changing, and the rewrites take at most sift.col_auto_merge_wal_pct (5) of the WAL the server writes: +0.9% to +1.9% total WAL at 1M rows with full-page images and a checkpoint per round, +0.0% without full-page images (it waits for the WAL), against +12% to +28% when it rewrites at every VACUUM; the file is up to twice its live size after a rewrite until later VACUUMs; no rewrite while a snapshot holds back the blocks the last one freed
- **When:** Caps the rewrite workers of sift_col indexes running at once in a database; a worker above the cap looks again after sift.maintenance_naptime, doubling, 8 times; the next VACUUM asks again. 0 to 64.
- **Default:** 0 — 0: no limit, the behavior before. A bound to cap the workers of a database that run at once; it has no measurement of its own.
- **Turn off:** 0 (the default) is no limit: SET sift.col_auto_merge_max_workers = 0 or RESET at server or database level (ALTER SYSTEM, ALTER DATABASE).
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 0 is no limit. Set N to cap the rewrite workers of a database that run at once; a worker above the cap looks again after the naptime, doubling, 8 times.

#### sift.agg_lookup_cache_kb

`sift_col`, `sift_table` · safety · since 3.2 · default 262144 kB

- **What it does:** the table of a lookup join loads only the rows its own conditions can match, and keeps its rows between queries (backend cache, shared memory)
- **Gain:** an unchanged 200,000-row joined table: 27 ms to load, 0.47 ms to check; 25,000 rows 3.3 ms and 0.09 ms
- **Impact:** index size: 12 MB per kept table of 500,000 rows with three text keys (76 MB before); at most 256 MB a backend; build, merge and writes: as `sift.agg_lookup_pushdown`; other: up to 256 MB a backend while the table is unchanged and fully vacuumed; a table that needs over half of it is loaded per query
- **When:** Lookup tables a SiftAgg scan loaded, while the table is unchanged and fully vacuumed (all pages visible, same LSNs); a written, rolled back or unvacuumed table is loaded again, as is one needing over half the limit.
- **Default:** 262144 kB — 256 MB a backend caps what a session keeps: an unchanged 200,000-row table costs 27 ms to load, 0.47 ms to check; 12 MB per 500,000-row table. The value 256 itself is not argued in a source.
- **Turn off:** SET sift.agg_lookup_cache_kb = 0 (session, role, database or postgresql.conf): nothing is kept between queries and, with sift.agg_lookup_shared_kb = -1, nothing is shared. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 256 MB a backend; 0 turns the per-backend cache off. Lower it on a server with many backends; a table that needs over half of it is loaded for every query.

#### sift.agg_lookup_shared_kb

`sift_col`, `sift_table` · safety · since 3.3 · default -1

- **What it does:** the table of a lookup join loads only the rows its own conditions can match, and keeps its rows between queries (backend cache, shared memory)
- **Gain:** first query of a fresh backend, 200,000 mapping rows, one integer key: 35.6 ms to load, 17.0 ms to read shared; 8 fresh connections at once 59.5 to 13.5 ms
- **Impact:** index size: 32 sessions joining a 200,000-row table: 254 MB private, 88 MB shared in all; build, merge and writes: the backend that writes the entry pays +16 ms once (200,000 rows); other: dynamic shared memory taken as entries are kept (by default the smaller of the per-backend cache and 5% of shared_buffers); with posix shared memory keep it below what /dev/shm holds
- **When:** Several backends joining the same unchanged table (pools of short connections, concurrent sessions); same-query backends wait up to 30 s for the loader; with no room in shared memory a backend loads its own table.
- **Default:** -1 — -1 (the smaller of agg_lookup_cache_kb and 5% of shared_buffers): 32 sessions on a 200,000-row table use 254 MB private against 88 MB shared; a fresh backend 35.6 to 17.0 ms; the writer pays +16 ms once.
- **Turn off:** SET sift.agg_lookup_shared_kb = 0 (superuser; ALTER ROLE/DATABASE .. SET or postgresql.conf): each backend loads its own tables, no restart. Dynamic shared memory is returned as entries go.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default -1 (the smaller of agg_lookup_cache_kb and 5% of shared_buffers), 0 turns sharing off, superuser. With posix shared memory keep it below /dev/shm. Helps pools of short connections.

#### sift.agg_lookup_max_kb

`sift_col`, `sift_table` · safety · since 3.3 · default 262144 kB

- **What it does:** the table of a lookup join loads only the rows its own conditions can match, and keeps its rows between queries (backend cache, shared memory)
- **Gain:** spine of 1,000 / 10,000 / 100,000 rows over 20,000 facts: 60 / 425 / 693 ms (100,000 refused) to 10.6 / 17 / 130 ms (spine matched by a hash)
- **Impact:** index size: as `sift.agg_lookup_pushdown`; build, merge and writes: as `sift.agg_lookup_pushdown`; other: a larger sub-select, VALUES list or function keeps the normal join
- **When:** Rows of a sub-select, VALUES list, function or spine joined to the scan, and the keys of a semi or anti join, sized as rows times (width + 64 bytes) by the planner's estimate; a larger one keeps the normal join.
- **Default:** 262144 kB — 256 MB replaced a fixed limit of 10,000 spine rows: spines of 1,000 / 10,000 / 100,000 rows run 10.6 / 17 / 130 ms, where 100,000 rows was refused (693 ms).
- **Turn off:** n/a: a memory limit, not a switch; raise it to admit larger sets. A non-negative sift.agg_lookup_max_rows replaces it.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 256 MB (rows times width plus 64 bytes a row, as estimated). Raise it for a larger sub-select, VALUES list, function or spine; a larger one keeps the normal join.

#### sift.agg_semi_lut_size

`sift_col`, `sift_table` · safety · since 3.2 · default 4096 kB

- **What it does:** a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer or date leaf is answered from a byte table over the keys of the joined table
- **Gain:** as `sift.agg_semi_lut`
- **Impact:** index size: as `sift.agg_semi_lut`; build, merge and writes: as `sift.agg_semi_lut`; other: memory of that table, 4096 kB by default per scan and per worker; keys that span more keep the generic join
- **When:** The byte table (one byte per key value) of a semi or anti join on an integer or date leaf, per scan and per worker; a key range wider than size x 1024 values keeps the generic lookup join.
- **Default:** 4096 kB — 4096 kB covers key ranges up to 4,194,304 values per scan and worker and bounds the memory; wider keys fall back to the generic join. The choice of 4096 is not argued in a source.
- **Turn off:** n/a: a memory limit; lower it to cap memory. 0 sends every semi or anti join with a key to the generic join; sift.agg_semi_lut = off is the switch.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 4096 kB per scan and per worker (one byte per value of the key range). Raise it when keys span more (a wider range keeps the generic join); lower it to cap memory.

#### sift.agg_combo_cache

`sift_col`, `sift_table` · safety · since 3.0 · default 1048576

- **What it does:** the distinct value combinations of an expression PostgreSQL evaluates are cached across the rows of a scan
- **Gain:** -
- **Impact:** index size: n/a: a per-scan memory cache; nothing is stored in the index; build, merge and writes: n/a: a per-scan memory cache; it adds no write work; other: memory grows with the combinations kept before the cache starts again
- **When:** Expressions PostgreSQL evaluates for the aggregate scan, kept per distinct value combination (a volatile one is evaluated once per combination); combos with a lookup join or a dictionary leaf are never dropped.
- **Default:** 1048576 — 1,048,576 entries bounds the memory: past it the cache starts again and a value that comes back is evaluated again; the figure has no benchmark in a source.
- **Turn off:** n/a: a bound on the cache, not a switch (16 to 16,777,216); a small value only makes it restart often.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 1,048,576 entries (16 to 16,777,216). More keeps more value combinations of an expression before the cache restarts; memory grows with it. Lookup-join and dictionary combos are never dropped.

#### sift.gpu_cache_mb

`sift_col`, `sift_table` · safety · since 3.1 · default 256 MB

- **What it does:** the integer aggregates of a scan run on a Vulkan device
- **Gain:** as `sift.gpu`
- **Impact:** index size: as `sift.gpu`; build, merge and writes: as `sift.gpu`; other: device memory per backend for chunks that stay on the device
- **When:** Per backend, only when the device runs a query: the compressed column chunks kept on it between queries (a cold backend uploads again); many connections multiply it.
- **Default:** 256 MB — n/a: no source records why 256 MB; the pinned cache is sized for the GPU memory of the host
- **Why not the default:** the speedup S is under 1.05, or saves under 100k instructions a query, needs hardware, not run on real workloads
- **Turn off:** n/a: a memory cap (16 MB minimum, plus 32 MB staging); sift.gpu = off uses no device memory.
- **Upgrade from 3.6:** unchanged
- **Advice:** Default 256 MB of device memory per backend for the compressed chunks kept between queries (at least 16), plus 32 MB of staging. A cold backend uploads again. Superuser.

#### sift.table_auto_defrag_delay

`sift_table` · safety · since 3.2 · default 10 ms

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** as `sift.table_auto_defrag`
- **Impact:** index size: as `sift.table_auto_defrag`; build, merge and writes: as `sift.table_auto_defrag`; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** Every background pass of a sift_table: sleeps n ms (at most four times) when its page accesses add up to vacuum_cost_limit, and after each segment it rewrites. Range 0 to 100 ms.
- **Default:** 10 ms — 10 ms: a safety throttle on I/O, as an autovacuum worker sleeps; no throughput figure is measured. Bounded to 100, the most vacuum_cost_delay allows.
- **Turn off:** SET sift.table_auto_defrag_delay = 0: the pass never waits (full speed). RESET returns 10.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 10 ms. The pass sleeps this long when its page accesses add up to vacuum_cost_limit and after each segment, as an autovacuum worker does; 0 never waits, at most 100.

#### sift.table_auto_defrag_max_active

`sift_table` · safety · since 3.3 · default 0

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** no throughput gain beyond the run-to-run spread (10-30%) in 3 rounds (200,000 rows, 4 clients)
- **Impact:** index size: as `sift.table_auto_defrag`; build, merge and writes: as `sift.table_auto_defrag`; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** The pass of a sift_table (also between its merges): waits for a quieter moment when more than n client backends of the database run a statement; looks again after the naptime, 8 times, then a later request asks again.
- **Default:** 0 — 0 (never waits): 3 rounds (200,000 rows, 4 clients) showed no throughput gain beyond the run-to-run spread (10-30%), so no waiting is imposed; set n where the pass must yield to busy clients.
- **Turn off:** Already 0 (never waits). SET sift.table_auto_defrag_max_active = n to wait; RESET returns 0.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 0 (never waits). Set to the number of busy clients above which the pass waits for a quieter moment (it looks again after the naptime, 8 times, and between its merges).

#### sift.table_auto_defrag_wal_rate

`sift_table` · safety · since 3.3 · default 0

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** no throughput gain beyond the run-to-run spread (10-30%) in 3 rounds (200,000 rows, 4 clients)
- **Impact:** index size: as `sift.table_auto_defrag`; build, merge and writes: 5.2 MB pass: 368 ms unlimited, 1.6 s at 2 MB/s; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** The pass of a sift_table: after a step ahead of n kB/s of average WAL the worker sleeps; it also bounds how often readers reload dictionaries after a rewritten segment.
- **Default:** 0 — 0 (no limit): 3 rounds (200,000 rows, 4 clients) showed no gain beyond the run-to-run spread (10-30%); a 5.2 MB pass took 368 ms unlimited, 1.6 s at 2 MB/s, so a cap is the user's choice.
- **Turn off:** Already 0 (no limit). SET sift.table_auto_defrag_wal_rate = n kB/s to cap; RESET returns 0.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 0 (no limit). Set kB/s to cap the pass's average WAL; it also bounds how often readers reload dictionaries after a rewritten segment.

#### sift.table_auto_defrag_budget

`sift_table` · safety · since 3.3 · default 128 MB

- **What it does:** a worker clears dead rows, rewrites mostly dead segments and returns free space without VACUUM
- **Gain:** as `sift.table_auto_defrag`
- **Impact:** index size: as `sift.table_auto_defrag`; build, merge and writes: as `sift.table_auto_defrag`; other: background WAL and I/O, bounded by the budget, the cost delay, the WAL rate and the active-backend check below; at dead_pct 5 a read after 16% churn takes 7.6 ms, not 35.8 (0.5 fresh)
- **When:** Each pass of a sift_table: the merge steps rewrite at most n MB of segments (the most dead first); the worker passes again while the budget stopped the last one. 0 is no limit.
- **Default:** 128 MB — 128 MB: bounds the WAL and I/O of one pass; the table VACUUM has no such cap. The same value as sift.vacuum_rewrite_limit; no measurement of other values is in a source.
- **Turn off:** n/a: a bound; 0 removes the cap. The pass itself is off with sift.table_auto_defrag = off. RESET returns 128.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave 128 MB. The most segment data one pass rewrites (the most dead first); the worker passes again while the budget stopped the last one. 0 is no limit.

### deprecated

#### sift(mutable_limit)

all · deprecated · since 1.0 · default 4096

- **What it does:** the 1.x and 2.0 spelling of write_buffer_limit, still accepted
- **Gain:** -
- **Impact:** index size: -; build, merge and writes: -; other: none: the same as write_buffer_limit
- **When:** A sift index created by 1.x or 2.0 that still carries the old spelling; the new name wins when both are set. Applies to sift indexes only (sift_col is new in 3.0).
- **Default:** 4096 — Kept accepted forever for compatibility, with the bounds, lock mode and ALTER INDEX behaviour of write_buffer_limit, so old dumps and scripts restore; no measurement.
- **Turn off:** ALTER INDEX .. RESET (mutable_limit), and use SET (write_buffer_limit = n); scripts/upgrade-check.sh lists indexes that still carry it. No REINDEX.
- **Upgrade from 3.6:** unchanged
- **Advice:** Old spelling of write_buffer_limit, still accepted (the new name wins when both are set). Use write_buffer_limit; scripts/upgrade-check.sh lists indexes that still carry it.

#### sift.agg_lookup_max_rows

`sift_col`, `sift_table` · deprecated · since 3.2 · default -1

- **What it does:** the table of a lookup join loads only the rows its own conditions can match, and keeps its rows between queries (backend cache, shared memory)
- **Gain:** -
- **Impact:** index size: as `sift.agg_lookup_pushdown`; build, merge and writes: as `sift.agg_lookup_pushdown`; other: the older form of agg_lookup_max_kb; -1 lets the memory decide
- **When:** The same joins as sift.agg_lookup_max_kb, limited by the estimated rows whatever their width; used only when set to 0 or more, and then it replaces the memory limit.
- **Default:** -1 — -1 hands the decision to sift.agg_lookup_max_kb, the limit by memory that replaced the row count; the setting is kept so a configuration of the older form keeps working.
- **Turn off:** SET sift.agg_lookup_max_rows = -1 (or RESET) returns to the memory limit; the setting itself is deprecated and cannot be removed.
- **Upgrade from 3.6:** unchanged
- **Advice:** Leave at -1 (the memory limit decides); use sift.agg_lookup_max_kb. When set it still limits the rows alone, whatever their width.
<!-- sift-cases:end -->

## Upgrading from 3.6

After `ALTER EXTENSION pg_sift UPDATE` the new library reads every index of 3.6 as it is and starts with the next backend (a database that preloads the library, `session_preload_libraries`, after a reconnect; a restart for `shared_preload_libraries`). An index keeps the units it holds until a merge or a `REINDEX` rewrites them, and `sift.upgrade_commands()` lists the `REINDEX` that gives older units a section the options ask for. The block below is generated from the `upgrade` cell of the options that are new, have another default or write a newer column format minor than 3.6 reads; each of them has a line. The upgrade from 3.5 is in [the 3.6.0 release notes](releases/v3.6.0.md), and the ones before in the notes of their release.

<!-- sift-upgrade:start -->
No default changed since 3.6; new: none.
<!-- sift-upgrade:end -->

## NOTICE and WARNING texts

The library tells what an automatic choice did with a NOTICE; `SET client_min_messages = warning` silences them. A WARNING says that something went wrong and is retried or needs a `REINDEX`. The table is generated from the `ereport(NOTICE ...)` and `ereport(WARNING ...)` calls of `src/`.

<!-- sift-notices:start -->
| Level | Message (`%s` is a placeholder) | Raised in | Options it names |
|---|---|---|---|
| NOTICE | sift.gpu: no usable Vulkan device (%s); the aggregate runs on the CPU | `src/agg/gpu.c` | `sift.gpu` |
| NOTICE | sift.gpu: %s; the aggregate runs on the CPU and the device is not used again in this session | `src/agg/gpu.c` | `sift.gpu` |
| NOTICE | sift.gpu = %s: this build has no Vulkan support (build with VULKAN=1); the aggregate runs on the CPU | `src/agg/gpu.c` | `sift.gpu` |
| NOTICE | sift: the rows of a joined table are not shared with other backends: no dynamic shared memory for %s bytes | `src/agg/lookshm.c` |  |
| NOTICE | SiftAgg not used: %s: %s | `src/agg/refuse.c` |  |
| WARNING | maintenance of sift index "%s" failed: %s | `src/bm25/step.c` |  |
| NOTICE | sift_col index "%s": key_cut = '%s' was chosen: %s "%s" would read about %s of %s units, where %s hold its rows (predicted index size %s%%, benefit %s against cost %s) | `src/col/autocut.c` | `key_cut` |
| NOTICE | sift_col index "%s": %s "%s" reads about %s of %s units, where %s hold its rows; key_cut = '%s' would cut that, and the index would grow by %s%% | `src/col/autocut.c` | `key_cut` |
| NOTICE | sift_col: the strings of leaf %s (attribute %s%s%s) in superblock %s pass %sd bytes: the values after them are stored as overflow and read from the heap | `src/col/batch.c` |  |
| NOTICE | sift_col index %s: compression of the text of %s column%s chosen by the formula (lz4 for %s, zstd kept for %s) | `src/col/build.c` | `compression` |
| NOTICE | sift_col index "%s": the keys of %s passed the %s keys the index keeps in %s of %s units (%s%%) | `src/col/build.c` |  |
| NOTICE | sift_col index "%s": point_tid_columns lists the rows of the segment on a key-cut index | `src/col/build.c` | `point_tid_columns` |
| NOTICE | sift_col index "%s": point_tid_columns lists the heap addresses of the rows | `src/col/build.c` | `point_tid_columns` |
| NOTICE | sift_col index "%s": order_array_columns = 'auto' adds the sorted keys and heap addresses of %s, and order_array_group = 'auto' a second order of each date and timestamp column sorted by %s first | `src/col/build.c` | `order_array_columns`, `order_array_group` |
| NOTICE | sift_col index "%s": order_array_columns = 'auto' adds the sorted keys and heap addresses of %s | `src/col/build.c` | `order_array_columns` |
| NOTICE | sift_col index "%s": order_array_columns adds the sorted keys and heap addresses to the sorted runs of %s column%s | `src/col/build.c` | `order_array_columns` |
| NOTICE | sift_col index "%s": the primary key "%s" of "%s" is not a column of the index | `src/col/build.c` |  |
| NOTICE | sift_col index "%s": the foreign key columns %s of "%s" are not columns of the index | `src/col/build.c` |  |
| NOTICE | sift_col index "%s": an UPDATE of %s is not a HOT update, since the index covers %s | `src/col/build.c` |  |
| NOTICE | sift_col index %s: an equality on "%s" reads %s of %sd units, and no other leaf of the index follows the order of the table | `src/col/cluster.c` |  |
| NOTICE | sift_col index %s keeps the keys of %s in every unit | `src/col/keycols.c` |  |
| NOTICE | sift_col index %s keeps the elements of the arrays %s | `src/col/keycols.c` |  |
| NOTICE | sift_col index %s reads the jsonb paths %s | `src/col/keycols.c` | `paths` |
| NOTICE | sift_col index "%s": point_tid_columns includes "%s", chosen by the default formula: an equality on its nearly unique values, which do not follow the order of the table, would read every unit (about %s M instructions against %s M with the directory; predicted index size %s%%, benefit %s against cost %s) | `src/col/keycols.c` | `point_tid_columns` |
| NOTICE | sift_col index "%s": order_array_columns = 'auto' chose the text column "%s": benefit %s against cost %s | `src/col/keycols.c` | `order_array_columns` |
| NOTICE | sift_col index "%s": order_array_columns = 'auto' did not choose the text column "%s": benefit %s against cost %s | `src/col/keycols.c` | `order_array_columns` |
| NOTICE | sift_col index %s gives the dimensions of its cube to the columns whose histograms fit: %s | `src/col/keycols.c` |  |
| NOTICE | sift_col index %s keeps the lengths of the arrays %s | `src/col/keycols.c` |  |
| NOTICE | sift_col index %s writes a rollup of the rows by %s | `src/col/keycols.c` | `rollup` |
| WARNING | maintenance of sift_col index %s failed: %s | `src/col/maint.c` |  |
| NOTICE | sift_col index %s: the cube dim "%s" has no histogram in %s%%sf %s units, which hold more than %s groups of it | `src/col/rollup.c` |  |
| NOTICE | sift_col index %s: the keys of "%s" are a bloom filter, not a list, in %s%%sf %s units, which hold more than %s distinct keys | `src/col/rollup.c` |  |
| NOTICE | sift_col index %s: key_columns wrote no summary of "%s" in %s%%sf %s units, whose keys are one value, or every integer of a range, or inline text | `src/col/rollup.c` | `key_columns` |
| NOTICE | sift_col index %s: key_columns wrote no key list or bitmap of "%s" in %s%%sf %s units, where it would take more than %s%%sf the bytes of the unit | `src/col/rollup.c` | `key_columns` |
| NOTICE | sift_col index %s: rollup_dims wrote no rollup of "%s" in %s%%sf %s units, which hold more combinations of them than a unit may keep | `src/col/rollup.c` | `rollup_dims`, `rollup` |
| NOTICE | sift_col index %s: the cube holds %s dimensions and left out the histograms of %s column%s: %s | `src/col/rollup.c` |  |
| NOTICE | sift_col index %s: the calendar cells of every date and timestamp column (calendar_columns = 'all', the default) left out %s column%s: %s | `src/col/rollup.c` | `calendar_columns` |
| NOTICE | sift_col index "%s": sift.col_auto_merge does not vacuum "%s", whose autovacuum_enabled is false | `src/col/vacreq.c` | `sift.col_auto_merge` |
| NOTICE | sift_col index "%s": sift.col_auto_merge keeps it answerable after churn | `src/col/vacreq.c` | `sift.col_auto_merge` |
| NOTICE | sift index "%s" has %s segments: its write buffer stays open | `src/core/lane.c` |  |
| WARNING | index "%s" was built with other analyzer options | `src/core/ops.c` | `analyzer` |
<!-- sift-notices:end -->
