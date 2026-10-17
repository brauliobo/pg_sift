# pg_sift: design

`pg_sift` is a PostgreSQL 18 extension whose index access method (AM) `sift` does full-text search. Postings are keyed by heap ctid and stored as two-level bitmaps. It ranks with exact BM25, counts from bitmaps, and checks visibility through the visibility map. The goal is to beat ParadeDB/Tantivy, pg_textsearch, Lucene/Elasticsearch, Meilisearch, Typesense, GIN and rum on:
- top-k BM25;
- boolean and phrase queries;
- `count(*)`;
- build time, update cost, and resource use.

Status: 1.1.0. Of the extras below, 9 (parallel scan, opt-in and off by default), 10 (filter-mask pushdown) and 11 (keyword facets) are implemented in 1.1; 1.0.0 had none of them.

This document is authoritative. Each decision gives its reason and the alternatives it rejects. Section numbers match the brief: (a) to (k), (l) for the 1.1 facets, then module contracts and milestones. Estimates are marked **[est]** and are only claims until benchmark M6 confirms them.

User-facing names: extension `pg_sift`, AM `sift`, schema `sift`, operator `text @@> text`, and the functions `sift.score(tid)`, `sift.max_score(tid)`, `sift.highlight(...)`, `sift.tokenize(...)`, `sift.verify(regclass)`, `sift.segments(regclass)`, `sift.promote(regclass)`, `sift.merge(regclass)`, `sift.parse_query(text)`.

---

## 0. Data model and invariants

- **Key.** A document is the heap tuple at ctid `(blk, off)`. Its key is `SiftKey = blk << 16 | off` (48 bits used). There is no doc-id table, and merges never renumber.
- **Superblock (SB).** 256 consecutive heap pages, `sb = blk >> 8`. Within an SB, the pages a term occurs on form a 256-bit **page mask**, one AVX2 register.
- **Offset mask.** For one page, a 512-bit mask where bit `off-1` is set. It is one AVX-512 register, or two AVX2 registers.
  - `MaxHeapTuplesPerPage` is 291 at BLCKSZ 8192, so 512 bits always suffice. BLCKSZ must be 8192 (static assert).
- **Block.** 16 heap pages, 16 per SB. This is the granularity of score upper bounds.
- **Segment.** An immutable or mutable set of postings, plus a docmap that holds the document lengths. Every structure inside a segment is ordered by SB, then by key.
- **Invariants:**
  - **I1:** a ctid is *live* in at most one segment at a time.
  - **I2:** pages of an immutable segment are never modified in place, except its liveness pages. Pages are recycled only after the segment's `safexid` passes the global horizon.
  - **I3:** the directory changes only through one atomic WAL record. Scans copy it once, at start.
  - **I4:** the score of a (doc, term) pair depends only on (tf, dl, df, N, avgdl, k1, b, boost). It is identical in every segment kind and every SIMD level.

I1 holds because heap line pointers are reused only after `ambulkdelete` has cleared the old ctid (VACUUM's two-phase protocol). Any new tuple at that ctid lands in a mutable segment. The same tuple is indexed only once: HOT updates add no entry, and non-HOT updates create a new ctid.

---

## (a) Postings, term frequency, document length, and ranked top-k

### Decision

1. **Matching data: the codec blob only.** Boolean and count queries never touch scoring data. The `codec` module (`src/sift_bitmap.h`, landed; owner: codec agent, `docs/CODEC.md`) encodes posting sets; one codec group (`gid`) is exactly one SB. sift encodes with the codec's tf option **off**: its 4-bit nibbles saturate at 15 and cost 4 bits on every posting. The scoring sidecar below follows the codec blob in the term blob, so it costs no bandwidth unless a query ranks.
2. **Exact term frequency (tf) in a rank-space sidecar stream, one per term.** A *chunk* is one term's postings within one SB, i.e. one codec group. Postings are numbered in key order; that number is the *rank* (codec group `base` + rank within the group, via `sift_grp_ranks`). The stream holds:
   - `L1`: one bit per posting, set when `tf > 1`.
   - A nibble `min(tf-2, 15)` for each L1 member.
   - An escape byte `min(tf, 255) - 17` for each nibble equal to 15.

   Per-chunk checkpoints `SiftGroupScore {nib_base, esc_base, bound, flags, blk_idx}` (12 B, aligned with the codec's group index) give random access, so tf is decoded only for the chunks being scored. A chunk with no L1 member sets a flag and skips decoding; a term whose tf values are all 1 stores no stream.
   - Cost per posting: `1 + 4·P(tf>1) + 8·P(tf>16)` bits. With typical `P(tf>1) ≈ 0.25`, that is ≈ 2.0 bits for frequent terms. For rare terms most chunks have the all-ones flag, so the cost is near 0.
   - tf is exact up to 255, then saturates. Saturation changes a term's score by under 1.5 % even at `dl = 5·avgdl`.
3. **Exact document length (dl) stored once per document, not per posting.** The segment docmap holds an offset-indexed `uint16 dl[maxoff]` row for each heap page that has documents, saturating at 65535. This costs ≈ 2.3 B per document including row headers. That is 345 MB for 150 M documents **[est]**, under 1 % of the index.
4. **Scores are exact BM25 in float32.** For each matched positive leaf term t:

   `s_t = boost_t · idf_t · (k1+1) · tf / (tf + K(dl))`

   where:
   - `K(dl) = k1·(1 − b + b·dl/avgdl)`
   - `idf_t = ln(1 + (N − df_t + 0.5)/(df_t + 0.5))`
   - `N = Σ segment ndocs`
   - `avgdl = Σ segment sum_dl / N`
   - `df_t = Σ segment df_t`

   N, avgdl and df count postings that are indexed but not yet vacuumed, as Lucene does. This makes the statistics cheap and identical in every code path.

   The build uses `-ffp-contract=off`. Kernels use a fixed operation order and no FMA, so scalar, AVX2 and AVX-512 produce bit-identical scores (I4). pgx-bm25 needed this too.
5. **Score upper bounds for pruning (block-max), u8-quantized.** Two per-term bounds are stored:
   - **Per chunk:** `bound`, in the chunk's `SiftGroupScore`.
   - **Per 16-page block:** `blk_bound[16]`, stored only for chunks with at least 32 postings. Elsewhere the block bound equals the chunk bound. This keeps the overhead at ≤ 4 bits per posting, and ≈ 0.05 bits for dense terms.
   - **Per term:** `max_bound`, in the dictionary entry. This is the only bound for codec LIST blobs (sparse terms).

   Each bound is `ceil(255 · max w / (k1+1))` with `w = tf / (tf + K_ref(dl))`. `K_ref` uses `avgdl_ref`, the segment's avgdl at write time.

   At query time a bound is scaled by `c = max(1, avgdl_now / avgdl_ref)`. This stays valid: `K` decreases as avgdl grows, and `w(avgdl') ≤ (avgdl'/avgdl)·w(avgdl)` whenever `avgdl' ≥ avgdl`. So bounds never need rewriting.

   Changing k1 or b requires REINDEX; they are reloptions.
6. **Ranked evaluation: block-ordered best-first over (segment, block) units, with a verified threshold θ.** This is BMP-style, from Mallia et al., SIGIR 2024, adapted to heap blocks. Details below.

### Ranked algorithm (`eval` module)

1. **Bind.** Look up every leaf term in each segment's dictionary. Sum df, compute idf, and expand wildcard, regex and fuzzy leaves (§h).
   - **Mutable segments:** scan them once with term-hash filters and decode the matching docs. These are exact and few (bounded by `mutable_limit`), so they are scored immediately and seeded into the candidate heap.
2. **Upper-bound (UB) pass.** For every immutable (segment, SB) where the query can match, compute block UBs:

   `UB[blk] = Σ_t idf_t·boost_t·(k1+1)·c_seg/255 · bound_t[blk]`

   - The page-mask program also runs at SB level: AND nodes require every child's pages, NOT contributes nothing. Blocks with an empty page intersection get `UB = 0`.
   - The pass reads only the codec group keys and the `SiftGroupScore` entries, about 16 B per (term, SB). SIMD: 16 u8 bounds per SB widen into one 512-bit float vector and accumulate with a multiply-add.
   - Cost: 3 terms × 19.5 k SBs (150 M docs at 30 docs per page) is about 60 k directory entries, ≈ 0.1 ms **[est]**.
   - A 64-entry SB-group maximum (the max over 16 SBs) lets the pass skip whole regions once θ is high.
3. **Best-first loop.** Visit blocks in descending UB order, using a radix bucket queue keyed by the top 12 bits of UB. Stop when `UB_next ≤ θ`. For each block:
   - Decode the offset masks of the surviving pages and evaluate the boolean program exactly.
   - AND with the liveness mask, only where the SB is marked dirty.
   - Gather `K(dl)` per candidate offset and decode tf only for chunks in this block.
   - Accumulate `s_t` per term with SIMD, 16 float lanes per AVX-512 op.
   - Emit candidates with `score > θ` through a SIMD compare-and-compress.
4. **Verified threshold.** A candidate above θ is verified immediately by the caller's `verify` callback before it enters the top-k heap:
   - **Visibility.** If the page is all-visible in the VM and the snapshot is MVCC, nothing more is needed. Otherwise do one heap fetch, following the HOT chain.
   - **Residual quals** that the scan evaluates itself.
   - **Phrase recheck** (§g).

   θ is the k-th best *verified* score. Pruning is therefore exact under deletes, MVCC and extra filters, without a guess-and-retry loop. That retry loop is a pgx-bm25 pitfall: it rebuilt the ranking on over-pull with drifting statistics.
5. **Incremental emission.** Output is a generator. `sift_rank_next` pops the best verified candidate once its score is at least the best remaining block UB. The executor can therefore pull past k (OFFSET, a wrong LIMIT hint, a filter above the scan) and gets more rows in correct descending order. Scores and statistics stay frozen at bind time.
   - **Ties** break by ascending ctid, so output is deterministic.
   - **Without a LIMIT** (k unknown), the loop simply processes all blocks.

**Expected work** **[est]**, for an OR of 3 terms with 5 % document frequency each, k = 10, over 150 M documents:
- Exhaustive scoring is 22.5 M (doc, term) pairs, about 25 ms of SIMD.
- BMP-style ordering typically visits 0.1 % to 1 % of blocks. That is 300 to 3,000 blocks × about 480 documents, or 0.4 to 4 M pairs: **0.5 to 4 ms**.
- Conjunctions prune harder, because AND page masks are empty in most blocks.

### Alternatives rejected

| Option | Why not |
|---|---|
| Store positions | Rejected as the only design, adopted as the default option later (§g): +60–100 % index size at first estimate (one entry per token occurrence instead of per distinct term, ≈ 8–12 bits each; 2.6× on the 218k articles, 48 MB to 126 MB) and a build about 30 % slower, in exchange for phrases decided in the index. `positions = off` keeps the pure-bitmap layout and the heap recheck. |
| No tf (binary model) | Ranking quality loss. Lucene and Tantivy both use tf. |
| Fixed 4-bit tf per posting (the codec's `SIFT_F_TF` option) | 4 bits per posting against ≈ 2.0, and inexact above 15. |
| Nested unary log-bucket tf planes in ctid space or rank space | ≈ 1.5 bits per posting, but measured bucket error is 2.5 % at `K = k1` and 6–9 % for documents of 4–8× avgdl. It also loses exactness. Exact nibbles cost only 0.5 bits more per posting. |
| u8 quantized impact per posting (impact-ordered indexes) | 8 bits per posting, and it freezes k1, b and avgdl. |
| Doc-length buckets (Lucene's 1-byte SmallFloat norms) | Saves 1 B per document, under 0.5 % of the index, but adds up to ≈ 6 % error. Exact u16 is cheap. |
| Document-at-a-time WAND / MaxScore cursors | They need per-posting skip structures and branchy pivot selection. Block-ordered BMP over bitmap blocks is SIMD-dense and matches the heap-block layout. MaxScore's essential/non-essential split is kept *inside* a block: terms whose remaining bound cannot lift a candidate over θ are not decoded. |

---

## (b) Page, segment and group layout; dictionary; statistics

### Pages

- Every page has a standard header and a 16-byte special space, `SiftPageOpaque {u16 type, u16 flags, u32 reserved, u64 gen}`. `gen` is the owning segment's generation number. It detects recycled pages on hot standby (§d) and orphaned pages after a crash. Data pages use the 8152 bytes between header and special space as a raw byte stream.
- **Block 0 is the metapage:**
  - magic, format major/minor;
  - the options snapshot: k1, b, analyzer hash;
  - `next_gen`;
  - free-extent list head;
  - high-water mark;
  - flags.
- **Block 1 is the directory:** up to 128 `SiftSegEntry` records, each with:
  - gen, kind (immutable / mutable-active / mutable-sealed), format;
  - header block, SB range;
  - ndocs, sum_dl, bytes;
  - a dirty flag.

  It also records the active mutable segment. A *directory snapshot* is a plain copy of the metapage and directory (I3).

### Immutable segment

A header page holds the extent table and the sections table. Its data is a logical byte stream over **extents**: runs of contiguous blocks, allocated in multiples of 16 blocks so readahead and AIO see long sequential runs.

| Section | Content |
|---|---|
| DOCMAP | One codec blob of all present keys: the universe for NOT, and its group `base` is the doc rank used for liveness. Per SB with docs: `u32 rowstart[npages]` and `u16 dl` rows indexed by offset, located through a sorted `{sb, off}` directory. |
| POSTINGS | Per term, a *term blob* (`sift_posting.h`): `SiftPostHeader`, then the codec blob (`sift_encode` with tf off; the codec picks LIST for sparse terms or GROUPED with its own group directory `key/poff/base`), then the scoring sidecar: `SiftGroupScore[ng]`, `blk_bound[16]` rows, and the tf stream. df ≤ 2 is inlined in the dictionary entry (read with `sift_cur_tids`). |
| DICT | Front-coded blocks of 32 terms in byte order, each entry carrying `SiftTermInfo {df, flags, max_bound, post_off, post_len \| inline keys+tf}`. |
| DICTIDX | One entry per dictionary block: `{u64 big-endian 8-byte prefix, first-term offset, block offset}`. Every 256th entry is copied into a *root array* in the header. |
| LIVE | Separate pages (they are modified in place): one bit per document in segment rank order, plus a bitmap with one *dirty* bit per SB. |

- **Dictionary lookup:** SIMD search of u64 prefixes in the root array (cached with the opened segment), then one DICTIDX page, then one DICT block of at most 32 terms. That is ≤ 3 buffer touches, ≈ 0.3 µs when hot **[est]**.
- **Expansion** (prefix, wildcard, regex, fuzzy) runs a range scan over DICT blocks bounded by the pattern's literal prefix (§h).
- **FST rejected:** complex, slow to build, and its main benefit is RAM-resident term indexes, which shared buffers already give us.
- **Liveness group = SB.** A query ANDs liveness only in SBs whose dirty bit is set. A clean SB is a fast path with no liveness decode.
- **Exact per-term counts:**
  - The dictionary entry holds `df`, and the codec group directory gives each chunk's posting count (`base` deltas).
  - In a clean SB where every page is all-visible, `count(*)` adds chunk counts with no offset decode. For OR, this works once page masks are disjoint within the SB (a disjoint-OR shortcut, applied per SB). Otherwise sift uses the codec count kernels (`sift_grp_count_mask`, `sift_grp_and_count`).
- **Statistics.** `ndocs`, `sum_dl` and `avgdl_ref` per segment live in the header and the directory entry. The metapage holds totals for immutable segments; mutable totals are summed during the mutable scan.

**Size model** **[est]**, Stack Exchange-like corpus of 150 M documents with about 100 distinct terms each:
- codec postings: per CODEC.md;
- tf: ≈ 2 bits per posting on dense chunks, ≈ 0 on rare terms;
- bounds: under 0.1 bits per posting;
- docmap: 2.3 B per document;
- liveness: 1 bit per document;
- dictionary: about 12 B per distinct term.

**Target:** an index no larger than 60 % of the indexed text.

---

## (c) Mutable segments, promotion, merge (no preload required)

- **Insert path (`aminsert`).**
  1. Analyze the document into (term, tf) pairs and dl.
  2. Append one *doc record* to a page of the active mutable segment: `{tid, flags, nterms, dl, u32 hash[nterms], u8 tf[nterms], u8 len[nterms], term bytes}`.
  3. Each data page header carries a 256-bit bloom of its term hashes and its counters (ndocs, sum_dl).
- **Lanes.** A mutable segment has 8 lanes (`SIFT_LANES`). Each lane is an append chain whose tail block is stored in a lane-header page.
  - A backend picks lane `MyProcNumber % 8`. If `ConditionalLockBuffer` fails it tries the next lane, and blocks only after a full round.
  - This removes GIN's single-tail hot spot.
  - One insert is one generic-WAL record over 1–3 pages (tail, new page, lane header), logged as a delta of roughly the record size.
- **Search.** Read all pages of the mutable segments, skip a page whose bloom misses the query, and use a SIMD compare of u32 hashes per record. A hash hit is confirmed against the term bytes. Matches are decoded into an in-memory mini-segment, using the same scoring (I4).
  - Cost is bounded by `mutable_limit` (reloption, default 4 MB): about 0.1–0.2 ms per query worst case **[est]**.
- **Sealing.**
  1. When the active segment exceeds `mutable_limit`, the inserting backend *seals* it in one directory record: the active entry becomes sealed and a fresh active entry is added.
  2. A concurrent appender holds the lane tail lock while appending and re-checks `sealed` under that lock.
  3. The sealer takes each lane tail lock once after flipping the flag, so no append is in flight afterwards.
- **Promotion** turns a sealed segment into an immutable one.
  1. Read its records and sort them by key.
  2. Build with `SiftInvBuf` and write new extents through shared buffers, logged with `log_newpage_range`.
  3. In a single directory record: replace the sealed entry with the new immutable entry, update metapage totals, and free the old pages with `safexid = ReadNextFullTransactionId()`.

  Records flagged deleted are dropped. A crash before the switch leaves orphan pages, which the sweep reclaims (§i).
- **Who promotes, in this order:**
  1. **Async dynamic background worker.** `RegisterDynamicBackgroundWorker` works without `shared_preload_libraries`. The worker's entry point is resolved from `pg_sift.so` by name.
     - It is launched by the sealing backend if `sift.async_maintenance = on` (default) and a slot is free.
     - At most one worker per index. Duplicate launches are prevented by `ConditionalLockPage(index, 0, ExclusiveLock)` inside the worker, plus a `maint_requested_at` stamp on the metapage.
  2. **`amvacuumcleanup` / `ambulkdelete`.** Autovacuum's insert threshold triggers these on insert-only tables, the same way GIN's pending list is cleaned.
  3. **Back-pressure.** If total sealed bytes exceed `4 × mutable_limit` and the maintenance lock is free, the inserting backend promotes synchronously. If the lock is taken, it keeps appending: queries get slower, but writes never block.
  4. **Manual:** `sift.promote(idx)` and `sift.merge(idx)`, for example from pg_cron.
  - When the library *is* preloaded, `_PG_init` additionally registers a static launcher that periodically scans databases for indexes with pending maintenance. This is optional and off unless `sift.launcher = on`.
- **Merge, tiered policy.** Tiers grow by ×8. When at least 8 segments share a tier, or the segment count exceeds 32, merge the smallest qualifying run.
  - The merge streams a k-way merge of the dictionaries.
  - Per term, chunks are combined in SB order:
    - **Disjoint SBs:** byte-copy of the chunk, tf stream and bounds, with no re-encode.
    - **Overlapping SBs:** decode, OR (disjoint by I1), re-encode.
    - **Dirty SBs:** AND with liveness, dropping dead postings. This is the only garbage collection. A forced merge (`sift.merge`) also rewrites a lone segment that has dirty SBs, so its dead terms leave the dictionary.
  - Bounds are recomputed only for rewritten chunks.
  - Memory is O(chunks in flight), not O(segment). Merges run in maintenance contexts (worker, vacuum, manual) and, as back-pressure, in the foreground (next item).
- **Directory back-pressure.** The directory holds 128 entries. When the async worker lags, a sealing backend that finds 112 or more entries (`ROOM_SEGMENTS`) waits for the maintenance lock and promotes and merges until the directory is below that, before it seals. An insert never fails for lack of entries and memory stays bounded (the active segment is capped by `mutable_limit`). Cost: while VACUUM holds the maintenance lock (all of `ambulkdelete`), such an inserter waits. An error remains only if 112 entries cannot be merged.
- **Rejected:**
  - *In-place mutable term trees*: one page write per term per insert, so ≈ 100× the WAL of an append record.
  - *Shared-memory mutable buffers* (DSM registry): data would be lost on crash unless WAL-logged anyway, and they add a second search path.
  - *Synchronous promotion in every insert*: latency spikes.

---

## (d) WAL strategy

| Operation | Mechanism | Reason |
|---|---|---|
| CREATE INDEX / REINDEX | `smgr_bulk_*` (`storage/bulk_write.h`). Batched `log_newpages`, skipped under `wal_level=minimal` for a new relfilenode, writes bypass shared buffers, and there is one fsync at finish. | The fastest supported path, the same as nbtree's in PG17+. |
| Promotion / merge pages | `ExtendBufferedRelBy` into shared buffers, fill, `MarkBufferDirty`, then `log_newpage_range` (32 full-page images per record) before the directory switch. | The bulk API needs exclusive ownership of the relation's tail. Concurrent inserters extend the same relation, so it cannot be used here. |
| Directory switch, metapage totals, sealing | Generic WAL, ≤ 4 buffers, in the fixed order metapage → directory → others. | Atomic and small. |
| Mutable appends | Generic WAL delta. | The record is about the appended bytes. |
| Liveness updates (VACUUM) | Generic WAL delta per LIVE page and the dirty-SB page, up to 4 per record. | Only changed bytes are logged. |
| Freeing extents | Same record as the directory switch (free-list entries carry `safexid`). | Crash consistency. |

- **Custom rmgr rejected.** `RegisterCustomRmgr` only works from `shared_preload_libraries`. If the rmgr is missing, recovery crashes, including on replicas that never loaded the library. Generic WAL keeps sift preload-free and replica-safe.
  - Cost: diffing (memcmp of each 8 KB page) and the 4-buffer limit. The bulk build path never pays it.
- **Rules (binding):**
  - Nothing may throw between `GenericXLogStart` and `Finish`: allocate before starting.
  - Maintain `pd_lower`/`pd_upper` so page holes are real.
  - Register the metapage first, because buffer lock order equals registration order on standby.
  - Never publish anything that is not yet logged: data pages are always logged before the directory record that makes them reachable.
- **Hot standby.** Generic WAL cannot raise recovery conflicts for recycled extents. So:
  - Every page read checks the page's `gen` against the expected segment gen.
  - On a mismatch on a standby, the scan raises `ERRCODE_T_R_SERIALIZATION_FAILURE` ("sift segment recycled during scan, retry").
  - `hot_standby_feedback = on` makes this impossible and is recommended.
  - On the primary, the `safexid` horizon guarantees no scan can see a recycled page, so the check is an assertion there.

---

## (e) Visibility, MVCC, `count(*)`

- **Recheck model.** Index results are exact for term and boolean queries, so `recheck=false` in `amgetbitmap`. They are a superset for phrases the positions do not decide, so those use recheck (§g). Every returned heap tuple passes heap visibility (bitmap heap scan, or `table_index_fetch_tuple` inside our custom scan).
  - An index entry may point to a dead tuple until VACUUM (standard).
  - The custom scan takes `PredicateLockRelation(heap)` under SERIALIZABLE, because `ampredlocks = false`.
- **Index-only `count(*)`.** Only for MVCC snapshots. For each SB:
  1. `vis = pext(VM bytes for the SB's 256 pages)` → a 256-bit all-visible page mask. These are 64 consecutive VM bytes, with 2 bits per heap page.
  2. `cnt += Σ popcount(result_offmask ∧ live)` over all pages, or Σ chunk counts on the fast path.
  3. Pages with `result_pages ∧ ¬vis` are heap-checked. The heap page is read once, and `heap_hot_search_buffer` runs per candidate offset under the scan snapshot.
- **Reading the VM.** sift reads the VM fork directly. Its layout (`HEAPBLOCKS_PER_BYTE = 4`, 2 bits per page, `MAPSIZE = BLCKSZ − MAXALIGN(SizeOfPageHeaderData)`) has been unchanged since 9.6.
  - Calling `visibilitymap_get_status` per page costs about 5–10 ns × 5 M pages, ≈ 25–50 ms. The SIMD path costs 64 B per SB.
  - The layout constants are pinned by a regression test that compares against `visibilitymap_get_status` on random pages.
- **Non-MVCC snapshots** (SnapshotAny or SnapshotDirty in RI checks, CLUSTER) always use heap checks.
- **The table must use the heap table AM**, checked in `ambuild`. Keys assume heap ctid semantics.

---

## (f) Planner and executor integration

Hooks used, all present in the PG18 headers (verified):
- `set_rel_pathlist_hook` (`optimizer/paths.h`);
- `create_upper_paths_hook` (`optimizer/planner.h`);
- `CustomPath`/`CustomScan` with `CUSTOMPATH_SUPPORT_PROJECTION` (`nodes/extensible.h`);
- `find_ec_member_matching_expr` (`optimizer/paths.h`);
- `SupportRequestSimplify` and `SupportRequestIndexCondition` (`nodes/supportnodes.h`).

**Hook installation does not need preload.** The planner opens every index's relcache entry in `get_relation_info`, which calls the AM handler and so loads `pg_sift.so`. `_PG_init` installs the hooks, chaining any previous ones. This happens before `set_rel_pathlist` runs for that query. The `@@>` support and selectivity functions also live in `pg_sift.so` and load it even earlier. This follows from reading the code, not from a test; M3 adds a regression test that runs a first query in a fresh backend.

1. **Plain AM paths.** `amgetbitmap` supports Bitmap Heap Scan and BitmapAnd/BitmapOr with other indexes. `amgettuple` is NULL; there are no plain index scans or index-only scans. `amcostestimate` uses real term statistics.
2. **`SiftScan` custom path** (added in `set_rel_pathlist_hook`). Conditions:
   - the rel is a base rel or partition child;
   - it has a sift index;
   - `baserestrictinfo` contains `key @@> q` where `key` equals the index key expression and `q` is a pseudo-constant (Const or Param).

   All such clauses are ANDed into one query. The remaining quals become scan quals. There are two variants:
   - **Unordered.** Heap-order emission with a fused visibility check. A `ReadStream` gets heap blocks from the match iterator for AIO prefetch (perfect, because order is known), and also feeds index chunk pages ahead.
   - **Ranked.** Added when `root->query_pathkeys[0]` is `sift.score(<this rel>.ctid) DESC`, matched with `find_ec_member_matching_expr` (this also handles partition child members). The path's pathkeys are that first pathkey, so no Sort is needed. Partitioned tables get MergeAppend over per-child ranked scans (see "Partitioned parents" below). The k hint is `root->limit_tuples`.
3. **Score delivery, the key reliability decision.** `CustomScan.custom_scan_tlist` = the Vars the rel needs + every `sift.score(ctid)` / `sift.max_score(ctid)` expression for this rel found in `root->processed_tlist` and the pathkeys.
   - `setrefs` (`fix_upper_expr` with `has_non_vars`) replaces those expressions in the scan's projection, and in every upper node that references them, with `INDEX_VAR` columns that the scan fills.
   - The score travels inside the tuple. There is **no backend-global scan registry**: nested loops, self-joins, cursors, rescans, parallel workers, and errors in portals cannot mix up scores.
   - The C bodies of `sift.score` and `sift.max_score` only raise `ERRCODE_FEATURE_NOT_SUPPORTED`: "sift.score() must be computed by a sift scan of the same relation; move it to the query level that scans the table".
   - **Rejected:** pgx-bm25-style registries keyed by ctid. Their history (ARCHITECTURE.md §score accessor: about ten ADRs on wrong-scan attribution, aborted portals, HOT roots, and security) shows the approach is inherently fragile.
   - **Partitioned parents.** `sift.score(parent.ctid)` names the parent's ctid, which is ambiguous across partitions, so the score cannot be one column of the parent. Each partition's scan computes the score of its own `sift.score(<partition>.ctid)`; the planner finds it by translating the parent-form score expressions of the target list to the partition (`adjust_appendrel_attrs_multilevel`, any depth) and by reading the partition's members of the sort key's equivalence class (`setup_eclass_member_iterator` with the partition's relids; `find_ec_member_matching_expr` ignores child members of other relids, and a plain walk of `ec_members` never sees them). The first implementation looked for the partition's own relid in the target list, which is written for the parent, and iterated `ec_members`, which holds only the parent's members, so the partition scans computed no score, were not ranked, and the Sort above the Append evaluated the stub. The score then reaches the top as part of the target list pushed down to the partitions (and as the sort column of the MergeAppend); a ranked partition scan has the sort key as its pathkey, so the planner builds `MergeAppend` over the per-partition ranked scans and `Limit` stops it after k rows (each scan has the k hint). Scores are the partitions' own: BM25 statistics (N, df, avgdl) are per partition index, so equal documents in different partitions can score differently, and the global order is by those scores; ties keep each partition's ctid order and fall in the partitions' order (MergeAppend takes equal keys from the earlier child), which is the single-table order when the partitions follow the heap order of the data. With one partition left after pruning there is no Append and the scan is the plain ranked scan; with none, the plan is a one-time-false Result. Multi-level partitioning flattens into one Append of the leaf partitions. Inheritance children behave the same way.
     - A query that reads the score needs a sift index on every partition it scans: a partition without one (or whose partial index does not cover the `@@>` clause) raises `ERRCODE_FEATURE_NOT_SUPPORTED` "sift.score() needs a sift index on every partition the query reads" at plan time, instead of silently ranking part of the data. Unranked matching and `count(*)` have no such requirement (partitions without the index are filtered).
     - `sift.max_score()` is the best score of one scan's query; through a parent it would be one value per partition, so it raises "sift.max_score() is not available through a partitioned table" (call it on a partition).
     - The score is read at the query level that scans the parent, in the select list or `ORDER BY` (a subquery the planner keeps, e.g. with `OFFSET 0` or `LIMIT`, works for anything above it). The Append carries the scores its partitions' scans output (its path target lists them after the parent's Vars, in the order the partitions' scans output them), so joins and aggregates above it find them (see "Score above a join").
     - A ranked partition scan is never parallel-safe: under a Parallel Append the planner would add a Sort above a Gather Merge, which loses the scan's tie order and ranks every match. With `sift.parallel_scan = on` unranked partition scans may still run in parallel, and results equal the serial ones.
     - A ranked scan replaces the rel's other paths, and its paths are not added with `add_path`: `add_path` frees a path it rejects (the seq scan of an empty partition is cheaper), and the planner then kept a pointer to freed memory.
   - **Score above a join.** The scan outputs every score the query reads next to its Vars (`score_outputs`), so a join node that evaluates `sift.score(p.ctid)` in its own target finds it in the child's output instead of calling the function. Nested loop, hash and merge joins are tested against the per-relation result (`rank_scan`).
     A join rel's target holds only Vars, so a join node above another join found no score in its child (the child's tlist is its path target) and evaluated `sift.score(ctid)` itself: the semi-join-plus-join and the lateral shapes of the first integration raised the "must be computed" error. `set_join_pathlist_hook` therefore gives every join path under the topmost scan/join rel a target that also lists the scores of the SiftScan (or Append of SiftScans) rels it covers, so the score travels through joins of any depth, method and kind (semi, anti, outer, lateral, self joins); the topmost rel gets its target from the query's scan/join target, which already lists the scores of the select list and `ORDER BY`, and for an aggregate (whose scan/join target holds only the Vars of its arguments) the score is added in `create_upper_paths_hook` (`UPPERREL_GROUP_AGG`) to the paths the aggregate reads. The topmost rel's paths are not extended earlier because `apply_scanjoin_target_to_paths` assigns the scan/join target's sort/group references to them when the expressions are the same, and a longer target would read past that array. A ranked scan is not parallel-safe in a query with more than one base rel: a Gather (Merge) over a join builds its target from the rel's Vars and would drop the score. An Append takes its columns by position, so only the Append of a parent's scans carries scores; the Append of a partitionwise join's child joins does not (an aggregate of the score over a partitionwise join keeps the error, a select list or `ORDER BY` reads it through the scan/join target pushed to the child joins). A score no SiftScan produces (no `@@>` condition on a sift-indexed column, `sift.enable_scan = off`, the other table of a join) still raises the error. Regression `rank_join` runs the shapes on a table, a partitioned and an inheritance parent against the score read in a join-free subquery, under every join method, with `sift.parallel_scan` and every `sift.filter_pushdown` mode. The ranked path is still chosen on the planner's estimate of the join: a join whose real selectivity is far below the estimate makes the scan produce candidates until the limit is met, which can be all of them.
4. **`SiftCount` upper path** (`create_upper_paths_hook`, stage `UPPERREL_GROUP_AGG`). Conditions:
   - one base rel, with no GROUP BY, grouping sets, window functions or SRFs;
   - every aggregate is `count(*)` or `count(<non-null const>)`;
   - every base restriction is a sift match on the same index;
   - no RLS or security quals other than those.

   The path builds `CustomScan(scanrelid = 0)` with `custom_scan_tlist` holding the Aggrefs, which setrefs maps to `INDEX_VAR`. The query expressions travel in `custom_exprs`. The node emits one row with no per-tuple executor work.
   - Anything else falls back to `Agg → SiftScan(unordered, empty tlist)`, which still skips heap access for all-visible pages.
   - **Conjunction order.** An AND cursor sorts its kids by posting count, rarest first, and stops seeking the rest of them at a group once the page intersection is empty. A 14-term conjunction of common words with one match took 780 ms on a 2.6M-document corpus because every LIST cursor built every group; it now skips them.
   - **Ranked unit seek (`sift_seek_eq`).** A ranked scan visits one superblock at a time and used to seek the operator tree with a plain `sift_seek`, which leapfrogs the AND/ANDNOT kids to the next group that matches anywhere. When the unit had no match, the kids ran far past it and the next unit rewound and repeated the walk. `sift_seek_eq` bounds the operators at the wanted gid, so a miss costs one probe. On a 2.6M-document corpus with 8 clients: `and` 80 -> 1016 qps (p95 516 -> 24.5 ms), `phrase` 65 -> 677 qps (p95 593 -> 35 ms), `mixed` 91 -> 342, `filter` 79 -> 217.
   - **Scoring reuses the tree's group.** A unit used to decode each live term's group twice: once through the tree cursor in `sift_seek_eq` and once more through the term's own scoring cursor. Scoring now takes the tree cursor's group when it already sits at the unit's gid and only seeks the clone when the tree skipped the term. One pgbench client on a 1M-document corpus, loaded host, interleaved A/B, `and`: 790-910 -> 950-970 qps.
   - **Bounds of LIST terms without group bitmaps.** The unit list needs the gids a term touches; for LIST terms (under `grouped_min_df`) that used to walk the whole cursor and build every group's bitmap. `sift_cur_gids` decodes the Elias-Fano blocks and keeps the distinct gids. 1M documents, one client, ranked `and`: 5.2 -> 4.0 ms mean; a query of rare terms 1.15 -> 0.54 ms.
   - **Per-block tf positions.** Scoring a group first counts the postings of each 16-page block to place its tf values. That was one `sift_popcount` call per page; it is one per block now (a block's offset bitmaps are contiguous). The escape count that places a LIST term's tf stream (`list_tf_pos`) tests 16 nibbles per step instead of one. Same interleaved A/B on a loaded host (load ~10), ranked `and`, one client: 4.8-4.9 -> 3.6-3.9 ms.
   - **Heap read-ahead (`sift.prefetch`).** An unordered SiftScan that returns tuples holds the next 16 matching pages and calls `PrefetchBuffer` on those more than 8 pages past the previous one. Closer pages are left to the kernel's readahead, which beats single-page reads when matches are dense. Measured cold (index and heap evicted, 2.6M documents, sum of per-query medians): `and` -26%, `or` -9%, `term` -9%, selective queries 2-4x faster, dense ones unchanged. Ranked scans and posting-blob reads were also tried and dropped (-4..-6% on `and`/`or`/`phrase`/`prefix`, 0% on `term`, below the 10% bar).
5. **Analyzer binding for non-index evaluation.** `sift.match` (the `@@>` function) has a planner support function.
   - **Simplify:** if the left argument matches the key of a sift index whose analyzer is not the default, rewrite the clause to `sift.match_idx(key, q, <index oid>)`. Seq-scan evaluation and bitmap recheck then use the index's analyzer.
   - **IndexCondition:** for `match_idx`, return `key @@> q` as an index qual (`lossy = false`).
   - With several indexes on one key, `@@>` means the default analyzer when one of them has it, else the analyzer of the lowest OID. An index whose analyzer answers differently (`sift_analyzer_config_agrees`: folding, stemmer, keyword and fields; pair and prefix terms only speed up) does not serve the operator: the clause is bound with `match_idx(key, q, <that index, or InvalidOid for the default analyzer>)`, which `index_serves` and `sift_match_clause` match to the indexes that agree, and `sift.matchsel` estimates with such an index (the call `match_idx` has no RESTRICT of its own: the support function answers `SupportRequestSelectivity` for it with the same estimate, from the index the call names; without it the call kept the planner's default of a third of the rows, whatever the query). Otherwise a query with `name:word` would answer differently on the cheapest plan. Partitions bind per partition (the clause is simplified again for each child); an index on the parent serves every partition.
   - *To verify in M3:* PG18 `simplify_function` passes OpExpr calls through `SupportRequestSimplify` with `root` set. If it does not, the fallback is ERROR at execution for non-default analyzers evaluated outside an index.
6. **Costs.** `sift.matchsel` and `amcostestimate` estimate selectivity from the index's term statistics: df for terms, an independence product for AND, inclusion-exclusion capped for OR. Lookups cost ≈ 1 µs per term per segment at plan time.
   - Ranked SiftScan cost: `startup = UB pass + estimated k-block work`. It is strictly cheaper than Sort over a bitmap scan when there is a LIMIT. The total is not the startup: drained past its hint (`sift_rank_next` searches again for 16 times as many hits, 4 times beyond 4096) the scan ranks every match, so the total is the cost of the scan without a limit, and a consumer that stops after a fraction of the rows (a Limit, a Nested Loop Semi Join under it) interpolates from the startup to it, while a hash join or a sort that drains the scan pays it all. The scan is the only path of its rel, so the pick between its paths (filter pushed down or not) is by the cost of the k rows (`pick_cost`).
   - **Filter pushdown (`sift.filter_pushdown = off | on | force`, default `on`).** For a ranked scan the planner also builds the relation's bitmap path over its other indexes (btree, BRIN, GIN, BitmapAnd/Or) and adds a second SiftScan path whose child is that bitmap. The executor turns the TIDBitmap into per-superblock page masks and per-page offset masks (`src/sift_filter.c`) before the ranker starts; the ranker drops superblocks and blocks with no kept page and ANDs the masks into every block it scores, so only tuples the bitmap keeps are verified. The quals stay as residual quals, so lossy pages, BRIN and GIN rechecks, MVCC and `sift.match` results are the same as without pushdown. The mask holds at most `work_mem` bytes of 64-byte page rows; pages beyond it are kept whole (lossy). Cost: the bitmap scan, plus a charge per bitmap tuple and mask row (the tuples are the bitmap's selectivity, not the planner's lossy-page inflation of them), with the page count corrected by the btree column's correlation, against the post-filter estimate of verified candidates. The post-filter scan visits superblocks in heap order with no threshold until it holds k passing rows, so it verifies every match before the first row the quals can keep, `k / pass` of them (with the logarithmic overshoot) within the span of the rows that can pass, and past the span the matches that beat the k-th best of all passing rows (`k / (matches * pass)`). The span comes from the btree quals of the bitmap (`=`, ranges, `= ANY(const array)`; BitmapAnd intersects, BitmapOr unites) placed in the heap through the histogram of the column (`col < min`, `col > max`) and its correlation (squared, as for the pages); a filter with no such quals, or a column uncorrelated with the heap, spans the whole heap, which is the plain `k / pass` estimate. Both paths also pay for the blocks the ranker scores (`SCORE_BLOCK_COST` per block, in place of a per-posting charge): a block is scored when its bound can still beat the threshold, which is every block until k rows are held and, when superblock bounds tie (probability `1 - exp(-(matches per superblock / 3000)^2)`, so a broad term ties and a rare one does not), the blocks of the rows' span in heap order. A `= ANY(const array)` bitmap is priced as ordered descents: elements on one leaf page share a descent (PG 17+), so a list over hot data costs its distinct leaf pages, not one random page per element (`saop_saving` re-runs the btree estimate with `random_page_cost = seq_page_cost`). `force` takes the pushdown whenever a bitmap path exists. EXPLAIN shows the Bitmap Index Scan child and `Filter Superblocks / Pages Exact / Pages Lossy`. A query the planner cannot read (the parameter of a generic plan: prepared statements, `plan_cache_mode`) has no match count, and the operator's default (0.5% of the rows) made a filter after the ranker look free, so a generic plan never pushed the filter down (a prepared filter query took 1.8 times the cycles of the literal one). A ranked scan now prices such a query as one that matches a tenth of the rows (`UNKNOWN_QUERY_SEL`; the ranked classes of the benchmark match 26% to 78% of the articles) and takes the rows that pass from the quals' own selectivity, so a selective filter is pushed down in the generic plan as in the plan of the literal query, and a loose one stays after the ranker. Wrongly pushing down costs the bitmap (forcing it on every query of the filter class: +12% cycles), wrongly filtering after costs up to twice that class's cycles. Regression `filter_pushdown_prepared` compares both plans and the answers of every mode.
   - **Parallel scan (`sift.parallel_scan`, default off).** With the GUC on, the planner also adds partial paths: a parallel-aware SiftScan (workers take morsels of superblocks from a shared counter; a ranked scan shares its threshold θ through DSM and the participants' top-k lists are merged by the last one to finish, so rows, scores and ties equal the serial plan) and a partial `SiftCount` under a Finalize aggregate. Workers follow the planner's index-scan rule (`parallel_workers`, `min_parallel_index_scan_size`, `max_parallel_workers_per_gather`) and never exceed the superblocks of the heap. A ranked parallel path needs a known `LIMIT` and a single relation (the score must reach the target list, which a Gather below a join would not pass on). A plain Sort over a Gather of a ranked scan is removed, because it would lose the tie order. It is off by default because a Gather costs milliseconds before any work (see item 9: it pays only for unordered scans with a heavy residual qual or on much larger corpora). With the GUC off no parallel path exists and plans are the serial ones.
   - **Parallel scan and filter pushdown.** The mask of a pushed-down filter is built from a bitmap child that one backend executes, so a SiftScan with a bitmap child is never parallel-aware: workers would each rebuild the bitmap, and sharing the mask in DSM would add a build barrier and a copy per query that the measured gains (item 10, serial) do not need. `add_scan_paths` therefore plans the parallel (partial) path only for the scan without a filter child, next to the serial path with and without the pushdown, and the planner chooses by cost: the pushed-down serial scan or the Gather over the unfiltered parallel scan. `sift.filter_pushdown = force` takes the pushdown whenever a bitmap path exists and therefore plans no parallel path for that scan. Regression `parallel_pushdown` runs all six combinations of the two GUCs and compares ranked top-k (ctids, scores, order), matches and counts with both off.
7. **EXPLAIN.** `ExplainCustomScan` prints the query (deparsed), mode, k, segments (immutable/mutable), SBs and blocks scored or skipped, candidates, heap checks, final θ, and pages prefetched. ANALYZE prints actual counters.

---

## (g) Phrase queries

- **Decision.** A phrase is the AND of its terms at the bitmap level, then a check of adjacent positions: from the positions stored in the index where the segment has them (below), else by a recheck that re-tokenizes the candidate's heap text with the index analyzer. The rest of this section describes the heap recheck, which `positions = off`, old indexes, mutable segments and negated phrases still use.
  - Inside SiftScan the recheck is part of `verify`, so ranked phrase queries only recheck candidates that would enter the top-k.
  - In the AM bitmap path it uses `recheck = true`, and the operator function evaluates it.
- **Cost model** **[est]:**
  - Recheck ≈ heap fetch (0.3 µs hot, 100 µs cold) + tokenization (1–2 ns per byte on the ASCII fast path, ≈ 5 ns per byte with Unicode folding).
  - Top-10 ranked: about k·ln(N/k) ≈ 150 verifications → ≈ 0.5–2 ms.
  - `count(*)` with a phrase: C candidates × (1–10 µs). For C = 100 k that is 0.1–1 s. This is the weak spot.
- **Mitigation (M7): the phrase pair index (reloption `phrase_index = pairs`, adjacent token pairs as extra terms) was removed in 1.3.0.** Word positions (below) decide phrases from the index; on the 218k articles the pairs bought `count(*)` 1.6x over them for a 2.7x larger index.
- **Positions (reloption `positions`, default on, REINDEX to change; on-disk format 1.1).**
  - `CREATE INDEX` stores a position stream after each term's postings (layout in docs/CODEC.md 3.1): the ordinals of the term in each document, delta coded, with a checkpoint every 32 postings, so the positions of one posting are read without decoding the others. Terms with at most two postings keep their records in the dictionary entry. Queries without a phrase never read the stream.
  - A phrase on a path of ANDs from the root (not under OR or NOT) is *required*: the relaxed AND of its terms still finds the candidates, and every candidate is checked against the positions (rarest term first, narrowing the possible start positions term by term) before the heap is touched. Counts, bitmaps, unordered and ranked scans do this; a candidate refuted by the positions costs no heap page and no tokenizing. A phrase under OR is *optional*: a cursor of its own yields the documents of the AND that hold it, so the OR sees only real matches.
  - What the positions do not decide stays on the heap recheck: documents of mutable segments and of the segments promoted and merged from them (no positions are written for them), segments of indexes built by earlier versions or with `positions = off`, and phrases under NOT (the index only knows a superset). A page match carries `recheck` only when it holds such candidates, and a `sure` mask with the candidates that need none (below), so `count(*)` over a fully positioned index fetches no heap tuple.
  - **Sure tree.** For unranked scans with a phrase that is not required (an OR branch, a NOT) the plan also binds the relaxation read from below: a phrase the bitmaps cannot decide matches nothing, a NOT excludes the superset of what it names. Its matches are a subset of the real matches, so a page is rechecked only for the candidates outside it (for example `dog OR "quick fox"` rechecks only the documents without `dog`). The matches of a segment whose positions decide every phrase are sure as well, unless a negated phrase is left to the heap. Direct group counting (`count(*)` summing popcounts of the all-visible pages of a group) applies whenever no candidate needs the heap and no required phrase needs its positions.
  - Format 1.1: the metapage minor version goes 0 to 1. This build reads 1.0 (segments without positions) and 1.1; a newer minor or another major is refused with a REINDEX hint. Earlier releases ignore the minor version, so a 1.1 index must be reindexed before an older release opens it.
- **Fixed-size phrase-candidate caches rejected:** no locality between queries.

---

## (h) Query language: SiftQL

```
query    := or_expr                       -- empty/whitespace-only: syntax error
or_expr  := and_expr ( "OR" and_expr )*
and_expr := unary ( [ "AND" ] unary )*    -- juxtaposition means AND
unary    := "NOT" unary | primary
primary  := ( "(" or_expr ")" | "{" atom+ "}" | phrase | atom ) [ "^" number ]
phrase   := '"' chars '"'
atom     := word [ "~" [ "1" | "2" ] ]    -- fuzzy, default distance 1
          | word "*"                      -- prefix
          | pattern                       -- word containing * or ? (not only trailing *)
          | "/" regex "/"                 -- POSIX ARE (PG regex), whole-term match
word     := run of chars other than whitespace and ( ) { } " ^ ~ / ; backslash escapes any char
```

- **Keywords.** `AND`, `OR`, `NOT` are keywords only in uppercase. `{a b c}` means "any of" (OR group).
- **Fields (`fields = 'name,city'`, REINDEX to change).** The index key is still one text, but an expression may return several sections split at `chr(29)`; the reloption names the first sections. Unqualified queries see the plain concatenation (the same terms, tf and dl as one text), so nothing changes for them. The words of a named section are also indexed as terms of their own, `\x10`/`\x11`... + word (`\x10 \x1E` + prefix when `prefix_index` is set), and `name:word`, `name:wor*`, `name:a?c`, `name:word~1` and groups `name:(a b)` / `name:{a b}` search only those (the innermost qualifier wins; the qualifier distributes over the group's leaves, and boosts such as `name:{a b}^3` multiply as usual). That gives the usual should-boost: `(a* b*) AND (name:{a* b*}^3 OR (a* b*))` scores a name hit as the sum of its field and plain terms. Notes: a field term has its own df/idf but shares the document's dl and avgdl (no per-field length normalization); phrases and regular expressions cannot be qualified; at most 14 fields; a name the index lacks raises `ERRCODE_SYNTAX_ERROR` listing the fields (`unknown field "nope" (available fields: name, city)`); an analyzer without fields (an index without the option, or `@@>` on a column with no sift index) reads the colon as text, as before: `name:word` is the word `name:word` and `name:(a b)` the words `name:a` and `name:b`, and a literal colon after an identifier is written `\:`. The parser hands the qualifier of a group to its members when it splices the group into a parent of the same operator. Seq-scan evaluation (`sift.match`, rechecks) knows the sections, so every path agrees. Measured on GeoNames places (`bench/bench run --dataset places`, name boosted x3, top 20, p50): see bench/README.md against ParadeDB with two edge n-gram fields.
- **Precedence:** NOT > AND > OR.
- **NOT rules.** A NOT must have a positive AND sibling. A query that is entirely negative raises an error (`42601`), because sift has no "match all" postings in the public API.
- **Analysis.**
  - A word goes through the index analyzer. If it yields several tokens (`e-mail`), it becomes a phrase; if it yields none, it is dropped.
  - If the whole query yields nothing, it matches nothing. This is not an error.
  - Prefix and wildcard literals are folded for case and accent but not stemmed. Regex is matched against dictionary terms as written (already folded).
  - Fuzzy is Levenshtein distance ≤ 1 or 2 with the first character fixed. The requested distance is clamped by term length, as in Elasticsearch AUTO: terms under 3 characters match exactly, terms under 6 allow at most distance 1 (so `saq~2` is `saq~1`), longer terms allow the requested distance. `parse_query` shows the effective distance.
- **Expansion.** Expanded terms are the union across segments. More than `sift.max_expansions` (default 512) raises an error, rather than silently truncating, for wildcard, regex and fuzzy leaves; a prefix leaf reads as a *wide prefix* instead (below). Only terms with a live posting count: a term whose docs were all deleted stays in the dictionary until a merge rewrites the segment, but does not use up the limit. Liveness is checked lazily, only when the limit is reached (dirty segments are re-expanded keeping live terms), so queries under the limit pay nothing.
  - **Short prefixes (`prefix_index = n`, default 0, REINDEX to change).** Expanding `s*` on a multi-field directory reaches thousands of terms (an error above `sift.max_expansions`, and slow below it). The reloption also indexes the first 1..n characters of every word as the term `\x1E prefix`, with tf = the occurrences of words that start with it (what an edge n-gram analyzer indexes, as ParadeDB's `edge_ngram(1, 15)` does). A prefix leaf of at most n characters is then one term lookup: no expansion, exact `df`/idf, bounds like any term. Longer prefixes still expand, over real words only: the control byte sorts the prefix terms (like the pair terms) before every word, and the matcher rejects them. Document evaluation (`sift.match`, rechecks) ignores the option, so results are the same with or without it. Measured on GeoNames places (`bench/bench run --dataset places`): see bench/README.md for index size, build time and latency against pg_search edge n-grams; a one-letter prefix is one term lookup where `max_expansions` errored.
  - **Wide prefixes.** A prefix leaf with more terms than `sift.max_expansions` (in a segment or over all of them) is never an error or a truncation. In every segment it reads as the term `prefix_index >= len` would have indexed (`sift_prefix.c`): the documents with a word that has the prefix, tf the occurrences of those words, df the documents, weighed like any term. The match set is exactly the union of the expansion (counts, filters and NOT equal document evaluation).
    - Mechanism: one dictionary range scan decodes the postings of each matching term once and collects them per superblock (4 bytes each); a superblock is summed in a dense tf array and emitted in key order into the posting writer, whose in-memory blob (with bounds, dl read from the docmap) is opened by the same cursor as a stored term, so the iterator, the count and the ranker (bounds, filters, parallel scans) need no special case. Count and unordered scans skip tf and bounds. Mutable segments match through the prefix matcher and score the term from their records (tf summed over its words).
    - Cost: the postings of the expansion per segment (10-15 ms for a one-letter prefix of a million names), then a memcpy: blobs are cached per backend (`sift.prefix_cache_size`, LRU, 8 MB) under the segment's relation file, generation and header block, which never name other postings (a segment is immutable, liveness is applied by the readers, and the union keeps deleted documents like dictionary df does). Memory while building: about 14 bytes per posting.
    - Scoring: with at most `sift.max_expansions` terms a prefix still scores as the max over them, each with its own idf (above); above it, as the one term, with the score `prefix_index` gives (equal bits, `test/sql/prefix_wide.sql`; the SQL reference models both). The limit is thus the number of terms scored individually. `prefix_index` only makes the lookup cheaper (no postings to read, no cache) for a larger index.
    - Estimates: the planner's expansion stops at the limit for wildcard, regex and fuzzy leaves, but a prefix reads its dictionary range on, df only, so its selectivity and cost are those of all its terms (about 1 ms of planning for a one-letter prefix of a million names, none for a prefix `prefix_index` covers). Wildcard, regex and fuzzy leaves keep the error.
  - Wildcard and regex ranges are limited to the literal prefix when there is one; otherwise every dictionary block is scanned.
  - Fuzzy uses bit-parallel Myers Levenshtein (≤ 64 bytes) over the first-character range.
- **Scoring.** Sum over matching positive leaves, multiplied by boost. NOT subtrees score 0. An expanded leaf scores the *max* over its expansions, with each expansion's own idf (dismax), so a document matching many variants does not get an inflated score (a prefix over `sift.max_expansions` terms scores as one term, see Expansion). A phrase scores the sum of its distinct terms.
- **Errors.** `ERRCODE_SYNTAX_ERROR` with a 1-based character position. `sift.parse_query(text)` returns the canonical deparse for tests and debugging.
- **Limits.** 256 leaves, nesting depth 32, phrase length 128 tokens (CJK text analyses to a token per character), boost in [0, 10000].

---

## (i) Concurrency, build, VACUUM, REINDEX, versioning

### Locks

| Who | Lock |
|---|---|
| Scan | Share lock on metapage + directory to copy them (I3). Pins and share locks per data page for the duration of each access, never across executor returns. |
| Insert | Exclusive lock on its lane tail page. Metapage exclusive lock only to allocate pages or extents or to seal. |
| Maintenance (promote, merge, `ambulkdelete` liveness pass, recycle, orphan sweep) | Heavyweight `LockPage(index, 0, ExclusiveLock)`, the same pattern as GIN pending-list cleanup. Taken with `ConditionalLockPage` by foreground and the worker, and waited for by VACUUM. The directory switch takes buffer locks briefly. |

### Build

- **Parallel build:** `amcanbuildparallel = true`. Own `ParallelContext` with participants = `plan_create_index_workers` + leader; `build_segments` asks for an exact layout and builds serially.
  - Every participant runs the serial pipeline on its share of a `table_parallelscan` (chunks of ascending blocks, interleaved with the others'): heap scan, analyze, `SiftInvBuf`, a sorted run per full buffer. `synchronize_seqscans` is off for the scan, because a wrap-around would break the ascending keys each buffer needs.
  - Worker runs are named files of a `SharedFileSet`; the leader opens them after the workers finish and merges them with its own into the segments a serial build writes (`sift_runs_write` already merges runs with interleaved key ranges). The result is the same index as a serial build: one segment, per-block bounds, no extra size.
  - Runs are merged in passes, so the memory budget never limits the size of an index. Each participant (the leader too, and a serial build) merges the last *fan-in* runs of one level into one run of the next as it spills, like an external sort: the runs it keeps stay a few per level, the files and read buffers fit the budget, and a posting is merged a number of times logarithmic in the runs. The fan-in is the readers that fit half of the budget, each a file buffer and a read buffer of 8 kB to 64 kB, shrunk as the runs grow in number (at least 8 runs, at most 256). A worker merges down to one fan-in of runs before it hands them over; `sift_runs_write` does the same for more runs than readers fit (also for promotion and merges). A pass reads and writes the run format, it takes no term's postings in memory. Only the segment writer does: a term is encoded whole, about 10 bytes a posting, so the runs of a segment are chosen to keep the largest term within the budget. The directory holds 128 segments, so when the budget would need more the build writes fewer, larger segments, which take more than the budget in their largest terms: a build never fails for lack of memory, it is slower or larger than asked.
  - Rejected: one segment per participant. Terms under `grouped_min_df` (64K postings) are LIST-coded and only have the per-term bound, so splitting a term of 75K postings over 3 segments drops its block bounds: the ranked scan of `good` scored 1217 blocks instead of 28, and the index grew 4 to 8 % (M) or more (S). The leader would also have copied segments page by page.
  - Rejected: concurrent bulk writers. `smgr_bulk` assumes it alone extends the fork; parallel `smgrextend` beyond EOF is unsafe, so only the leader writes the index.
  - The leader's merge (encode, dictionary, write) stays serial: about a third of the serial build time, which bounds the speedup. Cost: one extra write and read of the runs (≈ 1.3 bytes per posting).
- **Build throughput target** **[est]:** analysis at 3–5 ns per byte per core. 85 GB on 8 cores is ≈ 60 s of tokenizing, plus encoding and I/O. The target is a full build of 85 GB in under 4 minutes on 8 cores.
- **CREATE INDEX CONCURRENTLY** works through the standard protocol: `ambulkdelete` reports every indexed TID, then `aminsert` fills in.
- **REINDEX** is ambuild into a new relfilenode. `ambuildempty` writes the metapage and an empty directory for the init fork (unlogged tables).

### VACUUM

- **`ambulkdelete`** iterates each segment's docmap. For each present document whose liveness bit is set, it calls `callback(tid)`; for dead ones it clears the bit and marks the SB dirty. Mutable records get their deleted flag set.
  - This is O(indexed docs) callback calls, ≈ 20–40 ns each with PG17+ TidStore: 3–6 s per 150 M documents **[est]**. Fine for background work.
  - Generic WAL batches up to 4 changed pages per record.
- **`amvacuumcleanup`:**
  1. promote sealed segments;
  2. one merge step if the policy asks;
  3. recycle free extents whose `safexid` is past the horizon;
  4. orphan sweep;
  5. report `num_pages` and `num_index_tuples`.
- **Orphan sweep:** mark every block referenced from directory entries (header extents, LIVE pages, lane chains) and the free list. Any other block below the high-water mark goes back to the free list. This is cheap: a few extents per segment.

### Versioning

- **Metapage version.** A different major version raises an ERROR asking for REINDEX on open. A newer minor version is readable; older minors are upgraded lazily by merge, and each segment entry carries `format`.
- **SQL upgrades** use `pg_sift--1.0--1.1.sql` scripts.
- **pg_upgrade** copies the relfilenode files unchanged, so the on-disk format must stay stable across PostgreSQL minor releases. Any PostgreSQL-dependent layout, such as the VM constants, is tested.

---

## (j) SIMD dispatch

- **Build flags.** There are no per-file CFLAGS. The Makefile appends `-march=x86-64 -mtune=generic -O2 -g -ffp-contract=off` (overriding pg_config's `-march=native`), so the `.so` runs on any x86-64.
- **No POPCNT outside the SIMD objects.** GCC lowers `__builtin_popcountll` at baseline to a libgcc call, and a distribution's libgcc may be built with POPCNT. Baseline code therefore uses `sift_pc64` (`sift_popcount.h`, SWAR); the AVX2/AVX-512 objects keep the builtin through `SIFT_PC64`. `scripts/isa_check.sh` (in `ci.sh`) fails if a baseline object has a `popcnt` instruction or `__popcount*` reference, or `pg_sift.so` links libgcc's helper.
- **Kernels** are compiled with function attributes:
  - `SIFT_TARGET_AVX2` = `target("avx2,bmi,bmi2,popcnt")`;
  - `SIFT_TARGET_AVX512` = `target("avx512f,avx512bw,avx512vl,avx512dq,avx512vpopcntdq,avx512vbmi2,bmi,bmi2,popcnt")`.
- **`cpu` module** (`sift_cpu.c`, done in M0):
  - Uses `__builtin_cpu_supports`, which checks OS XSAVE state for AVX.
  - Marks BMI2 PDEP/PEXT as *slow* on AMD family 0x17 (Zen1/Zen2, microcoded at about 250 cycles). Those CPUs use the AVX2 kernels without pdep.
  - The `sift.simd = auto|scalar|avx2|avx512` GUC caps the level, for testing and benchmarking.
  - Modules register a rebind callback, so the codec can share the same dispatch.
- **Dispatch** goes through a `const SiftKernels *` table chosen once per level change, never per call through `cpuid`.
- **Kernel catalogue** (`sift_kernels.h`):
  - rank expand (pdep, or a scalar loop);
  - tf chunk decode (nibbles → u8; VBMI2 `vpexpandb` places them in offset lanes);
  - `K(dl)` gather;
  - BM25 accumulate (`vdivps`, exact IEEE, no FMA);
  - block-UB accumulate (u8→f32 widen + multiply-add as separate ops);
  - threshold compress (`vcompressps`/`vcompressw`);
  - VM pext.
- **The codec owns the bitmap set kernels:** AND, OR, ANDNOT, count, iterate. It keeps its own dispatch (`sift_init` / `sift_set_isa`), which a rebind callback in `sift_handler.c` binds to `sift.simd` (done in M0).
- **Correctness:** every kernel has a scalar reference. The `kernels` regression test runs randomized equivalence at every level the CPU supports, compared bit for bit.
- **AVX-512 frequency licensing.** Zen4/5 does not downclock. On Intel server parts, `auto` prefers the 256-bit AVX-512VL forms for scoring loops; 512-bit forms only where the data is ≥ 64 B per op.

---

## Extras beyond the baseline design: innovations, expected gains, measurement

Every item is measured by the M6 harness (`bench/`). It compares sift, in-database baselines (pg_textsearch, ParadeDB, GIN, rum), and external engines (Elasticsearch/OpenSearch, Tantivy CLI, Meilisearch, Typesense) on the same corpus and hardware. Metrics: QPS, p50/p99, buffers touched per query, build time, index size, and WAL bytes per update. An item that does not deliver ≥ 10 % on its target metric is dropped.

| # | Idea | Status | Expected gain **[est]** | Measure |
|---|---|---|---|---|
| 1 | Exact BM25 from rank-space tf nibbles + u16 dl (a) | core | Exact scores at ≈ 2 bits per posting; no quality gap against Lucene | Score equality vs reference SQL BM25; index size |
| 2 | BMP best-first over (segment, 16-page block) with u8 bounds and verified θ | core | 10–50× over exhaustive disjunction top-10; beats BMW cursors on SIMD density | blocks scored / skipped (EXPLAIN), QPS |
| 3 | Fused SIMD pipeline: page-mask program → offset decode → liveness → tf → BM25 → compress, per block in L1 | core | Removes the intermediate result sets of tbm-based plans | ns per (doc, term) |
| 4 | Score-in-tuple via custom scan tlist (no global registry) | core | Removes a whole bug class; also allows MergeAppend top-k over partitions | Isolation and parallel tests |
| 5 | PG18 ReadStream/AIO prefetch of heap blocks *and* index chunks (order known in advance) | core (M5) | Cold-cache p99 cut 3–10× on NVMe | p99 with `pg_prewarm` off and `io_method=io_uring` |
| 6 | VM pext → 256-bit all-visible mask per SB; chunk-count shortcut on clean SBs | core | `count(*)` at memory bandwidth; single-term count O(chunks) | count QPS against pg_textsearch and ParadeDB (Wikipedia) |
| 7 | Mutable lanes + bloom-gated mutable scan + async dynamic worker without preload | core (M4) | No insert hot spot; bounded query overhead under 1,000 updates/s | QPS under the 1,000 UPDATE/s mix, WAL per update |
| 8 | Copy-only merges for disjoint SBs (ownership transfer of encoded chunks) | M4 | Merge write amplification ≈ 1× for disjoint ranges | Merge MB/s, WAL bytes |
| 9 | Parallel-aware SiftScan: DSM morsels of 16 SBs, **shared atomic θ** across workers for ranked; partial counts + Finalize for count | implemented in 1.1, opt-in: `sift.parallel_scan = on` (default off). Results equal the serial plans (ties included). Measured (`bench/steps/parallel.sh`, 218,058 articles, 1/2/4/8 workers, host load 21–25): wins only for an unordered scan with a heavy residual qual (1.8×); `count(*)` 0.06–0.29× and top-1000/10000 0.5–1.2×, because a Gather costs 2.4 ms (1 worker) to 6–7 ms (8) before any work and the serial plans take 0.35–0.67 ms and 4–20 ms. Likely to pay only on much larger corpora | Near-linear scaling for count and large top-k; shared θ avoids per-worker over-scoring | Speedup at 1/2/4/8 workers |
| 10 | Filter-mask pushdown: SiftScan accepts child BitmapIndex paths (btree, BRIN) as `custom_paths` and converts the TIDBitmap into SB page/offset masks before scoring | implemented in 1.1 (kept; gate met): 0.1% filter, ranked `or`, mean latency 23.6 -> 0.22 ms (clustered) and 15.0 -> 0.86 ms (scattered) on 218k articles, `and` 4.9 -> 0.22 / 4.3 -> 0.64 ms; 25k articles: `or` 2.2 -> 0.21 ms, `and` 0.7 -> 0.19 ms. At 10% scattered the pushdown is slower than filtering after the ranker, so the planner costs both | Exact filtered top-k without post-filter over-fetch; 5–100× on selective filters | `WHERE body @@> q AND tenant = $1 ORDER BY score LIMIT 10` |
| 11 | Facet counts: reloption `analyzer = keyword` makes a sift index of exact values; `sift.facets(index, facet_index, query, lim)` = popcount(query ∧ value) per value, a function and not a GROUP BY rewrite ((l)) | implemented in 1.1: on 25,000 Wikipedia articles with 26 values, 1.9-4.5x sift GROUP BY and 1.9-15.9x GIN GROUP BY on broad queries (1.5-2.6x Elasticsearch terms), 0.6x on a selective AND; with 2,500 values 0.5-0.9x sift GROUP BY (cost is linear in the values), see (l) | ES terms-aggregation-class facets inside PG | GROUP BY facet QPS vs ES |
| 12 | Phrase pair index (g), removed in 1.3.0 for word positions | M7 | Phrase `count(*)` 10–100× | Phrase count QPS |
| 13 | Adaptive representation (codec picks LIST vs GROUPED per term and BITMAP vs ARRAY container per group; inline df ≤ 2) | core | Rare terms at ≈ Elias-Fano cost (~25 bits), frequent at ≈ 1 bit per posting | Bits per posting histogram (`sift.segments`) |
| 14 | Dirty-SB liveness skip: AND liveness only where VACUUM cleared bits | core | No liveness cost on clean data | — |

**Discarded after estimation:**
- **Per-backend dictionary caches.** Shared buffers already hold DICT pages, and a cache adds invalidation bugs.
- **A JIT-compiled query program.** The boolean program per SB is ≤ 256 tiny ops; interpretation overhead is under 3 %.
- **GPU offload.**
- **Statement-level insert batching via `aminsertcleanup`.** AFTER triggers and CTEs in the same statement would miss rows that are still unflushed. Revisit with a same-backend flush-on-scan rule.

---

## Engineering standards and test plan

- **Regression (`test/sql`, pg_regress through `make installcheck`):**
  - DDL and options;
  - analyzer and `tokenize`;
  - SiftQL parse/deparse golden tests;
  - result equivalence: every query shape compared against seq-scan `sift.match` and against a SQL reference BM25 (`test/sql/reference.sql`), with scores bit-equal to the reference computation in float4;
  - EXPLAIN shapes;
  - VM layout pin test.
- **Isolation (`test/isolation`, pg_isolation_regress):**
  - insert during seal and promote;
  - VACUUM during scan;
  - merge during ranked scan;
  - CIC with concurrent inserts.
- **TAP (`test/t`, PostgreSQL::Test::Cluster is present in pgxs):**
  - kill -9 during build, promotion, merge and vacuum, then restart and `sift.verify` plus result equivalence;
  - streaming replica replay plus queries on the standby, including the recycled-segment retry error;
  - `wal_level=minimal` build.
  - replication (`replication.sh`, also `scripts/replication.sh --container` with one container per node): hot standby equal to the primary after every kind of write, promotion, and a logical subscriber with its own index after the initial sync and under apply.
- **`sift.verify(index regclass, heapallindexed bool default false)`** works like amcheck:
  - checks the metapage and directory, page gens, extent overlap, dictionary order, chunk SB order, popcount = df, tf stream lengths, and that bounds really are ≥ the true max;
  - checks that docmap, liveness and postings are subsets;
  - with `heapallindexed`, re-tokenizes the heap and checks postings.

  Errors use `ERRCODE_INDEX_CORRUPTED`.
- **Memory.**
  - Every scan owns a context (`sift scan`) that is a child of the executor or AM scan context.
  - Per-block scratch is preallocated and 64-byte aligned (`palloc_aligned`).
  - The build and maintenance use a context bounded by `maintenance_work_mem`.
  - No allocation in critical sections.
  - Leak check: `MemoryContextStats` after 10 k rescans in a regression test.
- **Interrupts.** `CHECK_FOR_INTERRUPTS()`:
  - per SB in iterators, counts and UB passes;
  - per 64 blocks in the ranked loop;
  - per page in build, merge and bulkdelete;
  - never while holding buffer locks.
- **Sanitizers.** `make SANITIZE=1` adds `-fsanitize=address,undefined -fno-omit-frame-pointer`. A scratch cluster runs under `LD_PRELOAD=$(gcc -print-file-name=libasan.so)` with LeakSanitizer on (`scripts/leakcheck.sh env`).
- **Leak gate.** `scripts/leakcheck.sh` is the one place that decides whether a run leaked, shared by every suite through `pgtest.sh` (config, sanitizer environment, start and stop), `test/t/lib.bash` and `SiftTest.pm` (nodes), `tap.sh`, `soak.sh` and the last stage of `ci.sh`. It fails on the leak diagnostics PostgreSQL logs at transaction and backend end, on assertion failures, crashes and sanitizer reports, on the sift memory contexts that outlive a backend (`SIFT_TESTING`, `src/sift_leak_test.c`), on the state a cluster leaves when it stops, and on sustained memory growth of the soak backends. `leakcheck.sh selftest` and `test/t/leakcheck_gate.sh` (with `sift._test_leak`) show that it fails on each kind of leak and passes a clean log.
- **Fuzzing.**
  - The query parser via the SQL wrapper `sift.parse_query`, driven by a grammar-aware generator script, under ASan.
  - The codec standalone with libFuzzer, owned by the codec agent; the codec has no backend dependencies.
- **Determinism.** Fixed float evaluation order, ties broken by ctid, and `sift.simd` levels compared bit for bit.
- **Docs.** `README.md` (overview, install, quick start, benchmark summary), `docs/USAGE.md` (user guide: DDL, SiftQL, functions, GUCs, limits), `DESIGN.md` (this file), `docs/CODEC.md` (codec agent).
- **Test tiers.** `make check` (`scripts/ci.sh quick`) runs every suite in four parallel lanes on a non-ASan build with small corpora; `make check-full` (`scripts/ci.sh full`) is the ASan run at full sizes. `SIFT_QUICK=1` selects the quick sizes in `test/t`. The quick differential test runs 8 of the 16 harness mutants (the ones the full run kills with the most queries).
- **Dev loop.** `scripts/pgtest.sh install|start|psql|regress|stop|destroy` runs a throwaway cluster under `$SIFT_TMP` (default `/tmp/sift-arch`, port `$SIFT_PORT` in 56100–56119). It loads sift from a `DESTDIR` staging root through PG18's `extension_control_path` and `dynamic_library_path`; nothing is installed system-wide.
- **Packaging.** `packaging/PKGBUILD` (`pg_sift`) builds the committed HEAD with `nice make -j4` and installs through the standard PGXS `DESTDIR` path.
- **M0 behaviour.** `CREATE EXTENSION pg_sift` works. Any entry point whose module has not landed fails with `ERRCODE_FEATURE_NOT_SUPPORTED` "sift: … is not implemented yet"; for example, `CREATE INDEX … USING sift` fails that way. `amcostestimate` prices the index out of plans, and `@@>` selectivity uses `DEFAULT_MATCH_SEL`.

---

## (k) Limits and non-goals

- **Platform limits.** BLCKSZ 8192 only; heap tables only; server encoding UTF8 or SQL_ASCII (in SQL_ASCII, bytes ≥ 0x80 are opaque letters).
- **Index shape.** One key column of type text or varchar, or an expression yielding text. Partial indexes are supported.
- **Size caps.**
  - Terms are ≤ 255 bytes; longer ones are truncated at a character boundary.
  - dl saturates at 65535 and tf at 255 (score effect < 1.5 %).
  - ≤ 128 segments, ≤ 256 query leaves, ≤ `sift.max_expansions` expansions of a wildcard, regex or fuzzy leaf (a wide prefix needs memory proportional to its postings).
- **Scoring.** k1, b and analyzer options are fixed per index; changing them requires REINDEX. The index only grows to a high-water mark, and REINDEX shrinks it.
- **Non-goals for v1:**
  - positions and proximity / slop queries;
  - `amgettuple` plain and index-only scans;
  - backward scan, mark/restore;
  - multi-column indexes;
  - custom rmgr;
  - statement-level insert batching;
  - non-x86 SIMD (scalar works everywhere; NEON later);
  - parallel scan by default (opt-in since 1.1 through `sift.parallel_scan`, item 9).

---

## (l) Keyword facets

`SELECT category, count(*) FROM docs WHERE body @@> q GROUP BY category ORDER BY 2 DESC LIMIT 10` as bitmap arithmetic.

- **Keyword index (`analyzer = keyword`, REINDEX to change).** The whole value is one term, so each distinct value is one dictionary entry whose posting is the bitmap of its rows. Folding follows `case_folding` / `accent_folding` (both off: bytes as stored). An empty value, or one that analyses to nothing, and NULL have no term. A value over 255 bytes raises `ERRCODE_PROGRAM_LIMIT_EXCEEDED` instead of being truncated, because two long values sharing a prefix would be merged. It cannot be combined with `stemmer`, `prefix_index` or `fields`. A query word on a keyword index is one value (`"gamma delta"` for one containing a space), so `@@>` on it is an exact-value filter; the analyzer is part of the config hash, and non-keyword hashes are unchanged.
- **API: `sift.facets(index, facet_index, query, lim = 10, nulls = false)` returns `(value, count)`**, count descending then value bytewise; `lim` NULL returns every value; `nulls` adds a row with a NULL value for the matches without one. `index` is the search index the query runs on and `facet_index` the keyword index of the facet column, both on the same table. A multi-column facet is one keyword index per column.
- **Function, not an upper path.** A custom scan that replaces `Agg` over the scan would have to prove, in `create_upper_paths_hook`, that the grouping key is exactly the column of a keyword index, that the only qual is one `@@>` on another index, that the aggregate is a plain `count(*)` (no FILTER, DISTINCT, HAVING, joins, partial aggregation under parallel plans), and then absorb the Sort and Limit above it to get the early exit. Any shape it misses falls back to the ordinary plan silently, and any shape it wrongly accepts returns a wrong count. The function states the contract (two indexes, one query), is the same code path whatever the planner would pick, and costs one call. The price is that `GROUP BY` queries are not accelerated automatically.
- **Algorithm (`sift_facet.c`).**
  - The query's matches are collected once with the `count(*)` rules (visibility map masks for all-visible pages, heap checks for the others, phrase recheck) into superblock groups with a 64-byte offset row per page; the heap is read only where `count(*)` reads it.
  - Every value of the keyword index is a term; its count is the popcount of the AND of its posting groups with the matches (`sift_grp_and_count` over the codec cursors, `min` of the group widths). Dirty superblocks of a segment (VACUUM cleared bits) are ANDed with the liveness of the segment, as in a query; the other segments' postings are used as stored. Inline postings (df <= 2) are probed against the matches; the others leapfrog their groups against the matches.
  - Every dictionary is scanned once to list the values (the cost is linear in the number of distinct values, not in the matches); mutable segments are read once and counted per value; a value present in several segments is the sum of its counts.
  - **Top-k.** Values are tried in descending order of an upper bound (their df, plus the matches in the mutable segments); once the k-th count is at least the next bound the rest cannot enter and the scan stops. A value whose bound only ties the k-th count is still counted when it sorts before the k-th value. `nulls` needs every value counted (the NULL row is the matches minus the counted ones), so it disables the early exit.
- **Exactness.** The counts are those of the `GROUP BY` in the same snapshot (`GetActiveSnapshot`): uncommitted, deleted-and-vacuumed, updated and ctid-reused rows included (regression `facets`, isolation `facets_writes`: each stage compares every query with the SQL `GROUP BY`). Values are not limited by `sift.max_expansions`, which applies to the query only. SSI: both relations get a relation predicate lock.
- **Checks and limits.**
  - The two indexes must be on one table, valid, the facet index a non-partial keyword index. The caller needs `SELECT` on the table. Row-level security is refused (the bitmaps cannot apply policies).
  - No multi-valued fields: an index has one key and the whole value is one term, so a `text[]` cannot be facetted by element. Index one row per value in a side table, or index `array_to_string` and accept the joined value as the term.
  - Memory: 64 bytes per matching heap page, plus the term bytes and a small entry per distinct value and segment, in the function's context (not `work_mem`). A 20,000-value column over 218,000 articles is a few MB.
- **Cost model.** A call costs the matches (collected like `count(*)`) plus about 0.7 microseconds per distinct value (dictionary scan, two sorts, one posting seek and AND-count; a top-k stops early only when values have fewer documents than the k-th count). `GROUP BY` costs the matches plus a heap fetch of every match. So the function wins when the matches are many and the values few (a category, a year, a letter), and loses when the query is selective or the column has thousands of values; the planner is not asked to choose, the caller is.
- **Measured** (`bench/steps/facets.sh`: the first 25,000 Simple English Wikipedia articles, 1 client, median of 2 rounds of 3 s, PostgreSQL 18 with `max_parallel_workers_per_gather = 2`, a loaded shared host: load average 27 before and 23 after, so absolute numbers carry noise). 24 queries per workload; `one` = one word of every document-frequency band, `and` / `or` = two words. Queries per second; the facet result equalled the sift `GROUP BY` for every query.

  | column | workload | `sift.facets` | sift GROUP BY | GIN GROUP BY | ES terms agg |
  |---|---|---|---|---|---|
  | first letter (26 values) | and | 844 | 1421 | 453 | 323 |
  | | one | 1224 | 271 | 105 | 834 |
  | | or | 1468 | 755 | 92 | 984 |
  | hashed category (2,500 values) | and | 462 | 852 | 793 | 529 |
  | | one | 405 | 531 | 86 | 982 |
  | | or | 328 | 356 | 72 | 1074 |

  Each cell includes about 1 ms of client round trips and statement lookup shared by all methods (a timed call inside the server: 0.16-0.19 ms for 26 values and 1.6-1.7 ms for 2,500, against 0.15-0.21 ms for the GROUP BY on these small match sets). At 218,000 articles and 20,000 hashed values (one earlier run, load average 21-27, 5 s cells) the 26-value column gave 511 against 87 (GROUP BY, `one`) and 550 against 392 (`or`), 530 against 1497 (`and`), and the 20,000-value column 47-54 against 93-2080: the function is linear in the number of values.

---

## MODULE MAP

Prefixes: every exported symbol is `sift_<module>_*` unless it is an AM or SQL entry point. Headers live in `src/`. Ownership is by file. "Codec" is the concurrent codec agent.

| Module | Files | Owner scope | Depends on |
|---|---|---|---|
| core | `sift.h`, `sift_handler.c`, `sift_stubs.c` | Constants, options, GUCs, catalog OID cache, `_PG_init`, AM handler, weak stubs. **Done (M0).** | — |
| cpu | `sift_cpu.h/.c` | Feature detection, `sift.simd` GUC, dispatch rebind registry. **Done (M0).** | core |
| codec | `sift_bitmap.h`, `sift_bitmap*.c`, `docs/CODEC.md` | Term blob encoding (LIST/GROUPED), group cursors and AND/OR/ANDNOT cursor trees, set and count kernels, its own ISA dispatch. **Landed.** | — |
| kernels | `sift_kernels.h/.c` | Scoring SIMD kernels + scalar references | cpu |
| analyze | `sift_analyze.h/.c` | Tokenizer, folding, stemming, term hashing | core |
| query | `sift_query.h/.c` | SiftQL parse, analyze, deparse, document evaluation | analyze |
| store | `sift_page.h`, `sift_store.h/.c` | Page formats, metapage and directory IO, extent alloc and free, byte streams, WAL helpers | core |
| dict | `sift_dict.h/.c` | Dictionary writer, reader, term matchers | store, query |
| posting | `sift_posting.h/.c` | Term blob writer and reader: chunks, tf stream, bounds, sparse and inline | store, codec, kernels |
| segment | `sift_segment.h/.c` | Segment writer and reader: docmap, liveness, sections; streaming merge input | posting, dict |
| mutable | `sift_mutable.h/.c` | `aminsert`, lanes, seal, mutable reader, mutable delete | store, analyze |
| build | `sift_build.h/.c` | `SiftInvBuf`, `ambuild` (serial and parallel), `ambuildempty` | segment, analyze |
| maint | `sift_maint.h/.c` | Promote, merge, recycle, orphan sweep, `ambulkdelete`, `amvacuumcleanup`, worker, `sift.promote` / `sift.merge` / `sift.segments` / `sift.verify` | segment, mutable, build |
| visibility | `sift_visibility.h/.c` | VM masks, heap checks, fetch | core |
| eval | `sift_eval.h/.c` | Bind, iterate, rank, count, estimate | segment, mutable, query, dict, kernels, visibility |
| amscan | `sift_amscan.c` (`sift_am.h`) | `ambeginscan`, `amrescan`, `amgetbitmap`, `amendscan` | eval |
| planner | `sift_planner.h`, `sift_planner.c`, `sift_cscan.c` | Hooks, paths, costs, `amcostestimate`, SiftScan / SiftCount exec, EXPLAIN | eval |
| sql | `sift_sql.h`, `sift_funcs.c` | `sift.match`, `match_idx`, support, `matchsel`, highlight, tokenize, `parse_query`, score stubs | query, analyze, planner |

### Cross-module contracts (binding)

1. **SQL entry points.** `PG_FUNCTION_INFO_V1` appears **only** in `sift_handler.c`. Implementers define `Datum fn(PG_FUNCTION_ARGS)`, declared in `sift_sql.h` (`PGDLLEXPORT`).
   - `sift_stubs.c` holds `__attribute__((weak))` stubs for every AM callback and SQL entry point. Each raises `ERRCODE_FEATURE_NOT_SUPPORTED` "not implemented yet".
   - A module that lands overrides its stubs automatically; the module owner then deletes them from `sift_stubs.c`.
   - A module that calls a function its dependency has not landed yet adds a weak stub for that callee.
2. **Memory.**
   - Functions that allocate take an explicit `MemoryContext` or document "allocates in CurrentMemoryContext".
   - Long-lived objects (`SiftIndexState`, `SiftPlan`, `SiftRanker`, `SiftSegment`) live in their creator's context. `*_close` / `*_end` release buffers and pins only, never memory; memory is freed by deleting the context.
   - No static mutable state besides GUCs, hooks, the catalog cache and dispatch tables.
3. **Buffers.**
   - A function that returns a pointer into a buffer documents its lifetime: "valid until the next call on the same reader".
   - No buffer lock is held when returning to the executor or calling `verify`.
   - Every reader verifies the page `type` and `gen` (`sift_store_check_page`).
4. **Errors.**
   - `ereport(ERROR)` with specific errcodes: `ERRCODE_INDEX_CORRUPTED` for format violations, `ERRCODE_SYNTAX_ERROR` for SiftQL, `ERRCODE_PROGRAM_LIMIT_EXCEEDED` for limits, `ERRCODE_T_R_SERIALIZATION_FAILURE` for standby recycle.
   - Nothing returns error codes except the documented boolean "not found" results.
   - No `PG_TRY`: the parallel build's shared fileset goes away with its DSM segment.
5. **Masks.**
   - The matching path works on the codec's decoded group view `sift_grp`: a 256-bit page mask (bit i is page `sb*256 + i`) and, for each set page, an offset bitmap of `w` bytes (bit `off-1` is offset `off`).
   - Scoring kernels take fixed `uint64[SIFT_OFFMASK_WORDS = 8]` offset masks, 64-byte aligned. `eval` widens `w`-byte bitmaps to 64 bytes only for the pages it scores.
   - `SiftKey` and the codec's `sift_tid` use the same encoding (`blk << 16 | off`).
6. **Codec usage** (`sift_bitmap.h`, landed):
   - `sift_encode` with tf off;
   - `sift_cur_blob`, and `sift_cur_tids` for inline postings;
   - cursor trees `sift_cur_and` / `sift_cur_or` / `sift_cur_andnot` for the boolean program;
   - `sift_seek` / `sift_next`, and the `sift_grp_*` kernels;
   - the codec's `sift_count` for pure boolean counts over clean, all-visible ranges;
   - `sift_grp_ranks` to map candidates to term ranks for tf lookup.

   The codec is pure C with no palloc: callers pass `SIFT_CUR_BUF` buffers from their own memory context. To avoid name clashes, sift-side APIs use module prefixes (`sift_eval_count`, not `sift_count`).
7. **Concurrency.**
   - Only `store` touches the metapage and directory, through `sift_dir_read` and `sift_dir_update`.
   - Only `maint` takes the maintenance lock.
   - Only `mutable` writes lane pages.
   - Only `store` frees or recycles extents.

---

## MILESTONES

Each milestone ends with green `make installcheck` plus its listed tests, under `scripts/pgtest.sh`. Modules within a milestone run in parallel.

| M | Deliverable | Parallel work items (owner = module) | Exit test |
|---|---|---|---|
| M0 | Skeleton (this commit): builds, loads, stubs, `pgtest.sh`, packaging | architect | `CREATE EXTENSION`; CREATE INDEX gives a clear "not implemented" error |
| M1 | Foundations | analyze (+`sift.tokenize`), query (+`sift.parse_query`), store (+ unit SQL test functions under `sift._test_*` behind `#ifdef SIFT_TESTING`), kernels, codec | tokenize/parse golden tests; kernel equivalence; store page round-trip |
| M2 | Immutable index | posting, dict, segment, build (serial), eval (iterator + count), amscan, sql (`match`, `matchsel`) | Bitmap Heap Scan results = seq scan for 200 random queries; `sift.verify` |
| M3 | Planner | planner (SiftScan unordered and ranked, SiftCount, EXPLAIN), eval (ranker), visibility, sql (support functions, `highlight`) | top-k = reference BM25 (bit-equal scores, ctid ties); count pushdown plan shapes |
| M4 | Mutability | mutable, maint (seal, promote, merge, recycle, sweep, worker), vacuum liveness | isolation suite; TAP crash and replica; 1,000 UPDATE/s soak with verify |
| M5 | Scale | parallel build, ReadStream/AIO, cost model, VM pext fast path | build time and size vs ParadeDB; cold p99 |
| M6 | Hardening and benchmarks | `verify` (`heapallindexed`), ASan/UBSan, fuzzing, bench harness vs all engines | No sanitizer findings in 24 h of fuzzing; bench report |
| M7 | Extras beyond the baseline design | phrase pairs (done, removed in 1.3.0); parallel scan with shared θ (1.1, opt-in); filter-mask pushdown and keyword facets (1.1) | Each meets its ≥ 10 % gate or is dropped |

## Shared core and hosts

The codec, dictionary, segment writer and reader, scoring, SiftQL parser, analyzer and evaluator described above are host-independent C. The files named in
`src/portable.list` (the **portable core**) are compiled by every host: the PostgreSQL extension and the SQLite extension (`sqlite/`). Everything else in this
document (the index access method, WAL, MVCC, planner integration, VACUUM) belongs to the PostgreSQL host and does not apply to the others; the SQLite
host's own design is in [sqlite/DESIGN.md](sqlite/DESIGN.md).

**What is shared.** Besides the codec and the evaluator: the segment format and its writer/reader (`sift_segment.c`), the tiered merge policy
(`sift_merge.c`: which segments merge, from the directory and the sizes), the sorted runs and their merge into a segment (`sift_runs.c`: the inverted buffer `SiftInvBuf`, `sift_run_from_segment`, `sift_runs_write`; promotion and merge of PostgreSQL, merge of SQLite), the deletion of one document (`sift_seg_delete_key`, which VACUUM's `sift_seg_bulkdelete`
shares with the SQLite DELETE: the dirty flag, then the liveness bits), the byte stream's extent bookkeeping and padding (`sift_store.h`, `sift_stream.c`), the
analyzer-options validation and hash (`sift_analyze.c`), the layout constants and error texts. **What a host owns**: where bytes live (page store), transactions and
crash safety, memory and error plumbing, the way a query reaches the evaluator (planner and custom scan, xBestIndex), the write path that feeds the writer
(heap build and mutable segments, the in-memory buffer), background work, and option syntax (reloptions, `CREATE VIRTUAL TABLE` arguments).

**The host interface.** A portable file includes the C library, compiler intrinsics, the other files of the list and `src/sift_host.h`, nothing else
(`scripts/check-portable.sh`, a stage of `scripts/ci.sh` and the first step of `sqlite/test.sh`). `sift_host.h` is a contract, not an abstraction layer on the hot
path: its vocabulary is that of PostgreSQL's backend API (`palloc`, `MemoryContext`, `ereport` that does not return, `StringInfo`, `List`, `HTAB`, `pg_mb*`,
`pg_reg*`, atomics and spin locks for `sift_parallel.c`, `BufFile` for the builder's runs), plus `sift_host_enum_option` for the one process setting (`sift.simd`).
The page/extent store is the link-time interface declared in `sift_store.h` (`ReadBuffer`/`ReleaseBuffer`, `sift_wt_*`, `sift_sw_*`, `sift_sr_*`, `sift_dir_*`):
`sift_store.c` implements it over shared buffers and generic WAL, `sqlite/sqstore.c` over rows of a shadow table and a bounded cache. The PostgreSQL build includes the
real headers (identical calls, identical speed); the SQLite one provides the vocabulary in `sqlite/host/host.h` over `malloc`, `setjmp` and SQLite rows. Optional
collaborators the core calls but a host may not have (mutable segments, visibility, TID bitmap filters) are declared in portable headers and implemented by the
host; `sqlite/sqstubs.c` is the SQLite side of them.

**Adding a host.** (1) Provide a header with the vocabulary above and add its branch to `sift_host.h` (a `SIFT_HOST_<NAME>` define). (2) Implement `sift_store.h`
over the host's storage, and the optional collaborators it does not need as stubs that fail. (3) Build the files of `portable.list` (the Makefile reads it) and write the
host's front end: option parsing, the write path (feed `sift_segw_*` in key order: `sqlite/sqbuild.c` is the smallest example), and the merge driver
(`sift_merge_pick` chooses, the host rewrites). (4) Run the equivalence tests: scores must be bit-equal to PostgreSQL's evaluator on the same corpus
(`sqlite/bench/compare_pg.py`). The MariaDB proof of concept can adopt the same pieces without any change to the core.

**Still two copies.** The SQLite in-memory write buffer (`SqInv` in `sqlite/sqbuild.c`, any key order, deletes) against `SiftInvBuf` (ascending keys) and the
highlight/snippet/facet code are not unified; why, and what it would take, is in [sqlite/DESIGN.md](sqlite/DESIGN.md#what-is-not-shared-yet). The heap build, parallel workers
and the leader (`src/sift_build.c`) stay with the PostgreSQL host.
