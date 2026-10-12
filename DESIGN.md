# sift: design

`sift` is a PostgreSQL 18 index access method (AM) for full-text search. Postings are keyed by heap ctid and stored as two-level bitmaps. It ranks with exact BM25, counts from bitmaps, and checks visibility through the visibility map. The goal is to beat TIN, ParadeDB/Tantivy, pg_textsearch, Lucene/Elasticsearch, Meilisearch, Typesense, GIN and rum on:
- top-k BM25;
- boolean and phrase queries;
- `count(*)`;
- build time, update cost, and resource use.

This document is authoritative. Each decision gives its reason and the alternatives it rejects. Section numbers match the brief: (a) to (k), then module contracts and milestones. Estimates are marked **[est]** and are only claims until benchmark M6 confirms them.

User-facing names: extension `sift`, AM `sift`, schema `sift`, operator `text @@> text`, and the functions `sift.score(tid)`, `sift.max_score(tid)`, `sift.highlight(...)`, `sift.tokenize(...)`, `sift.verify(regclass)`, `sift.segments(regclass)`, `sift.promote(regclass)`, `sift.merge(regclass)`, `sift.parse_query(text)`.

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

   Changing k1 or b requires REINDEX, as in TIN; they are reloptions.
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
| Store positions | +60–100 % index size: one entry per token occurrence instead of per distinct term, ≈ 8–12 bits each. It also slows the build 1.5–2× and breaks the pure-bitmap design. Phrases are handled with recheck plus an optional pair index (§g). |
| No tf (binary model) | Ranking quality loss. TIN, Lucene and Tantivy all use tf. |
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
- **Liveness group = SB.** A query ANDs liveness only in SBs whose dirty bit is set. A clean SB is a fast path with no liveness decode (TIN-style).
- **Exact per-term counts:**
  - The dictionary entry holds `df`, and the codec group directory gives each chunk's posting count (`base` deltas).
  - In a clean SB where every page is all-visible, `count(*)` adds chunk counts with no offset decode. For OR, this works once page masks are disjoint within the SB (TIN-style disjoint-OR shortcut, generalized per SB). Otherwise sift uses the codec count kernels (`sift_grp_count_mask`, `sift_grp_and_count`).
- **Statistics.** `ndocs`, `sum_dl` and `avgdl_ref` per segment live in the header and the directory entry. The metapage holds totals for immutable segments; mutable totals are summed during the mutable scan.

**Size model** **[est]**, Stack Exchange-like corpus of 150 M documents with about 100 distinct terms each:
- codec postings: per CODEC.md;
- tf: ≈ 2 bits per posting on dense chunks, ≈ 0 on rare terms;
- bounds: under 0.1 bits per posting;
- docmap: 2.3 B per document;
- liveness: 1 bit per document;
- dictionary: about 12 B per distinct term.

**Target:** at or below TIN's index size for the same corpus, 50.7 GB for 85 GB of text.

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
  1. **Async dynamic background worker.** `RegisterDynamicBackgroundWorker` works without `shared_preload_libraries`. The worker's entry point is resolved from `sift.so` by name.
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

- **Recheck model.** Index results are exact for term and boolean queries, so `recheck=false` in `amgetbitmap`. They are a superset for phrases, so phrases use recheck (§g). Every returned heap tuple passes heap visibility (bitmap heap scan, or `table_index_fetch_tuple` inside our custom scan).
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

**Hook installation does not need preload.** The planner opens every index's relcache entry in `get_relation_info`, which calls the AM handler and so loads `sift.so`. `_PG_init` installs the hooks, chaining any previous ones. This happens before `set_rel_pathlist` runs for that query. The `@@>` support and selectivity functions also live in `sift.so` and load it even earlier. This follows from reading the code, not from a test; M3 adds a regression test that runs a first query in a fresh backend.

1. **Plain AM paths.** `amgetbitmap` supports Bitmap Heap Scan and BitmapAnd/BitmapOr with other indexes. `amgettuple` is NULL; there are no plain index scans or index-only scans. `amcostestimate` uses real term statistics.
2. **`SiftScan` custom path** (added in `set_rel_pathlist_hook`). Conditions:
   - the rel is a base rel or partition child;
   - it has a sift index;
   - `baserestrictinfo` contains `key @@> q` where `key` equals the index key expression and `q` is a pseudo-constant (Const or Param).

   All such clauses are ANDed into one query. The remaining quals become scan quals. There are two variants:
   - **Unordered.** Heap-order emission with a fused visibility check. A `ReadStream` gets heap blocks from the match iterator for AIO prefetch (perfect, because order is known), and also feeds index chunk pages ahead.
   - **Ranked.** Added when `root->query_pathkeys[0]` is `sift.score(<this rel>.ctid) DESC`, matched with `find_ec_member_matching_expr` (this also handles partition child members). The path's pathkeys are that first pathkey, so no Sort is needed. Partitioned tables get MergeAppend over per-child ranked scans. The k hint is `root->limit_tuples`.
3. **Score delivery, the key reliability decision.** `CustomScan.custom_scan_tlist` = the Vars the rel needs + every `sift.score(ctid)` / `sift.max_score(ctid)` expression for this rel found in `root->processed_tlist` and the pathkeys.
   - `setrefs` (`fix_upper_expr` with `has_non_vars`) replaces those expressions in the scan's projection, and in every upper node that references them, with `INDEX_VAR` columns that the scan fills.
   - The score travels inside the tuple. There is **no backend-global scan registry**: nested loops, self-joins, cursors, rescans, parallel workers, and errors in portals cannot mix up scores.
   - The C bodies of `sift.score` and `sift.max_score` only raise `ERRCODE_FEATURE_NOT_SUPPORTED`: "sift.score() must be computed by a sift scan of the same relation; move it to the query level that scans the table".
   - **Rejected:** pgx-bm25-style registries keyed by ctid. Their history (ARCHITECTURE.md §score accessor: about ten ADRs on wrong-scan attribution, aborted portals, HOT roots, and security) shows the approach is inherently fragile.
   - **Known limit:** a score referenced only above a join (`SELECT sift.score(p.ctid) FROM p JOIN u ...`) gets pushed into the base scan only if the expression is in the base rel's target. PG puts it there only when it is computed below the join. In that case the documented pattern is a subquery or CTE for top-k followed by the join.
4. **`SiftCount` upper path** (`create_upper_paths_hook`, stage `UPPERREL_GROUP_AGG`). Conditions:
   - one base rel, with no GROUP BY, grouping sets, window functions or SRFs;
   - every aggregate is `count(*)` or `count(<non-null const>)`;
   - every base restriction is a sift match on the same index;
   - no RLS or security quals other than those.

   The path builds `CustomScan(scanrelid = 0)` with `custom_scan_tlist` holding the Aggrefs, which setrefs maps to `INDEX_VAR`. The query expressions travel in `custom_exprs`. The node emits one row with no per-tuple executor work.
   - Anything else falls back to `Agg → SiftScan(unordered, empty tlist)`, which still skips heap access for all-visible pages.
   - **Conjunction order.** An AND cursor sorts its kids by posting count, rarest first, and stops seeking the rest of them at a group once the page intersection is empty. A 14-term conjunction of common words with one match took 780 ms on the M tier because every LIST cursor built every group; it now skips them.
   - **Ranked unit seek (`sift_seek_eq`).** A ranked scan visits one superblock at a time and used to seek the operator tree with a plain `sift_seek`, which leapfrogs the AND/ANDNOT kids to the next group that matches anywhere. When the unit had no match, the kids ran far past it and the next unit rewound and repeated the walk. `sift_seek_eq` bounds the operators at the wanted gid, so a miss costs one probe. Remote M quick suite with `--full`, 8 clients: `and` 80 -> 1016 qps (p95 516 -> 24.5 ms), `phrase` 65 -> 677 qps (p95 593 -> 35 ms), `mixed` 91 -> 342, `filter` 79 -> 217.
   - **Heap read-ahead (`sift.prefetch`).** An unordered SiftScan that returns tuples holds the next 16 matching pages and calls `PrefetchBuffer` on those more than 8 pages past the previous one. Closer pages are left to the kernel's readahead, which beats single-page reads when matches are dense. Measured cold (index and heap evicted, remote M, quick-suite queries, sum of per-query medians): `and` -26%, `or` -9%, `term` -9%, selective queries 2-4x faster, dense ones unchanged. Ranked scans and posting-blob reads were also tried and dropped (-4..-6% on `and`/`or`/`phrase`/`prefix`, 0% on `term`, below the 10% bar).
5. **Analyzer binding for non-index evaluation.** `sift.match` (the `@@>` function) has a planner support function.
   - **Simplify:** if the left argument matches the key of a sift index whose analyzer is not the default, rewrite the clause to `sift.match_idx(key, q, <index oid>)`. Seq-scan evaluation and bitmap recheck then use the index's analyzer.
   - **IndexCondition:** for `match_idx`, return `key @@> q` as an index qual (`lossy = false`).
   - With several such indexes on one key, the lowest OID wins, with a NOTICE at CREATE INDEX.
   - *To verify in M3:* PG18 `simplify_function` passes OpExpr calls through `SupportRequestSimplify` with `root` set. If it does not, the fallback is ERROR at execution for non-default analyzers evaluated outside an index.
6. **Costs.** `sift.matchsel` and `amcostestimate` estimate selectivity from the index's term statistics: df for terms, an independence product for AND, inclusion-exclusion capped for OR. Lookups cost ≈ 1 µs per term per segment at plan time.
   - Ranked SiftScan cost: `startup = UB pass + estimated k-block work`, total ≈ startup. It is strictly cheaper than Sort over a bitmap scan when there is a LIMIT.
7. **EXPLAIN.** `ExplainCustomScan` prints the query (deparsed), mode, k, segments (immutable/mutable), SBs and blocks scored or skipped, candidates, heap checks, final θ, and pages prefetched. ANALYZE prints actual counters.

---

## (g) Phrase queries

- **Decision.** A phrase is the AND of its terms at the bitmap level, then a recheck that re-tokenizes the candidate's heap text with the index analyzer and checks adjacent positions.
  - Inside SiftScan the recheck is part of `verify`, so ranked phrase queries only recheck candidates that would enter the top-k.
  - In the AM bitmap path it uses `recheck = true`, and the operator function evaluates it.
- **Cost model** **[est]:**
  - Recheck ≈ heap fetch (0.3 µs hot, 100 µs cold) + tokenization (1–2 ns per byte on the ASCII fast path, ≈ 5 ns per byte with Unicode folding).
  - Top-10 ranked: about k·ln(N/k) ≈ 150 verifications → ≈ 0.5–2 ms.
  - `count(*)` with a phrase: C candidates × (1–10 µs). For C = 100 k that is 0.1–1 s. This is the weak spot.
- **Mitigation (M7): reloption `phrase_index = pairs` (default `none`, REINDEX to change).** It indexes every adjacent token pair as the extra term `\x1F a \x1F b`.
  - Pair terms start with a byte no word can start with, so they sort before every word in the dictionary: prefix, wildcard, regex and fuzzy scans start above them and never see them, and the matcher rejects them in mutable segments.
  - Pair leaves are non-scoring, so scores stay the sum over the word leaves and equal the plain index bit for bit.
  - 2-word phrases become exact bitmap ANDs with no recheck; longer phrases AND their pairs and are still rechecked (rarely failing). Pairs longer than a term are not indexed and fall back to the recheck.
  - Measured on tier S (300 k documents, AVX2 host): `count(*)` of the phrases p95 126 ms -> 1.5 ms; top-10 phrase unchanged (about 2 ms, it only rechecks candidates that enter the top-k); index 18 MB -> 77 MB, build 1.4 s -> 5.0 s. It is a reloption because it costs 4x the index size on this corpus (most pairs are unique).
- **Positions rejected** (see (a)).
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
- **Precedence:** NOT > AND > OR.
- **NOT rules.** A NOT must have a positive AND sibling. A query that is entirely negative raises an error (`42601`), because sift has no "match all" postings in the public API.
- **Analysis.**
  - A word goes through the index analyzer. If it yields several tokens (`e-mail`), it becomes a phrase; if it yields none, it is dropped.
  - If the whole query yields nothing, it matches nothing. This is not an error.
  - Prefix and wildcard literals are folded for case and accent but not stemmed. Regex is matched against dictionary terms as written (already folded).
  - Fuzzy is Levenshtein distance ≤ 1 or 2 with the first character fixed. The requested distance is clamped by term length, as in Elasticsearch AUTO: terms under 3 characters match exactly, terms under 6 allow at most distance 1 (so `saq~2` is `saq~1`), longer terms allow the requested distance. `parse_query` shows the effective distance.
- **Expansion.** Expanded terms are the union across segments. More than `sift.max_expansions` (default 512) raises an error, rather than silently truncating. Only terms with a live posting count: a term whose docs were all deleted stays in the dictionary until a merge rewrites the segment, but does not use up the limit. Liveness is checked lazily, only when the limit is reached (dirty segments are re-expanded keeping live terms), so queries under the limit pay nothing.
  - Wildcard and regex ranges are limited to the literal prefix when there is one; otherwise every dictionary block is scanned.
  - Fuzzy uses bit-parallel Myers Levenshtein (≤ 64 bytes) over the first-character range.
- **Scoring.** Sum over matching positive leaves, multiplied by boost. NOT subtrees score 0. An expanded leaf scores the *max* over its expansions, with each expansion's own idf (dismax), so a document matching many variants does not get an inflated score. A phrase scores the sum of its distinct terms.
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
  - Rejected: one segment per participant. Terms under `grouped_min_df` (64K postings) are LIST-coded and only have the per-term bound, so splitting a term of 75K postings over 3 segments drops its block bounds: the ranked scan of `good` scored 1217 blocks instead of 28, and the index grew 4 to 8 % (M) or more (S). The leader would also have copied segments page by page.
  - Rejected: concurrent bulk writers. `smgr_bulk` assumes it alone extends the fork; parallel `smgrextend` beyond EOF is unsafe, so only the leader writes the index.
  - The leader's merge (encode, dictionary, write) stays serial: about a third of the serial build time, which bounds the speedup. Cost: one extra write and read of the runs (≈ 1.3 bytes per posting).
- **Build throughput target** **[est]:** analysis at 3–5 ns per byte per core. 85 GB on 8 cores is ≈ 60 s of tokenizing, plus encoding and I/O. The target is to beat TIN's 8 min 10 s by ≥ 2×.
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
- **SQL upgrades** use `sift--1.0--1.1.sql` scripts.
- **pg_upgrade** copies the relfilenode files unchanged, so the on-disk format must stay stable across PostgreSQL minor releases. Any PostgreSQL-dependent layout, such as the VM constants, is tested.

---

## (j) SIMD dispatch

- **Build flags.** There are no per-file CFLAGS. The Makefile appends `-march=x86-64 -mtune=generic -O2 -g -ffp-contract=off` (overriding pg_config's `-march=native`), so the `.so` runs on any x86-64.
- **No POPCNT outside the SIMD objects.** GCC lowers `__builtin_popcountll` at baseline to a libgcc call, and a distribution's libgcc may be built with POPCNT. Baseline code therefore uses `sift_pc64` (`sift_popcount.h`, SWAR); the AVX2/AVX-512 objects keep the builtin through `SIFT_PC64`. `scripts/isa_check.sh` (in `ci.sh`) fails if a baseline object has a `popcnt` instruction or `__popcount*` reference, or `sift.so` links libgcc's helper.
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

## Beyond TIN: innovations, expected gains, measurement

Every item is measured by the M6 harness (`bench/`). It compares sift, TIN-equivalent baselines (pg_textsearch, ParadeDB, GIN, rum), and external engines (Elasticsearch/OpenSearch, Tantivy CLI, Meilisearch, Typesense) on the same corpus and hardware. Metrics: QPS, p50/p99, buffers touched per query, build time, index size, and WAL bytes per update. An item that does not deliver ≥ 10 % on its target metric is dropped.

| # | Idea | Status | Expected gain **[est]** | Measure |
|---|---|---|---|---|
| 1 | Exact BM25 from rank-space tf nibbles + u16 dl (a) | core | Exact scores at ≈ 2 bits per posting; no quality gap against Lucene | Score equality vs reference SQL BM25; index size |
| 2 | BMP best-first over (segment, 16-page block) with u8 bounds and verified θ | core | 10–50× over exhaustive disjunction top-10; beats BMW cursors on SIMD density | blocks scored / skipped (EXPLAIN), QPS |
| 3 | Fused SIMD pipeline: page-mask program → offset decode → liveness → tf → BM25 → compress, per block in L1 | core | Removes the intermediate result sets of tbm-based plans | ns per (doc, term) |
| 4 | Score-in-tuple via custom scan tlist (no global registry) | core | Removes a whole bug class; also allows MergeAppend top-k over partitions | Isolation and parallel tests |
| 5 | PG18 ReadStream/AIO prefetch of heap blocks *and* index chunks (order known in advance) | core (M5) | Cold-cache p99 cut 3–10× on NVMe | p99 with `pg_prewarm` off and `io_method=io_uring` |
| 6 | VM pext → 256-bit all-visible mask per SB; chunk-count shortcut on clean SBs | core | `count(*)` at memory bandwidth; single-term count O(chunks) | count QPS against TIN's 10,260 QPS (Wikipedia) |
| 7 | Mutable lanes + bloom-gated mutable scan + async dynamic worker without preload | core (M4) | No insert hot spot; bounded query overhead under 1,000 updates/s | QPS under the 1,000 UPDATE/s mix, WAL per update |
| 8 | Copy-only merges for disjoint SBs (ownership transfer of encoded chunks) | M4 | Merge write amplification ≈ 1× for disjoint ranges | Merge MB/s, WAL bytes |
| 9 | Parallel-aware SiftScan: DSM morsels of 16 SBs, **shared atomic θ** across workers for ranked; partial counts + Finalize for count | M7 | Near-linear scaling for count and large top-k; shared θ avoids per-worker over-scoring | Speedup at 1/2/4/8 workers |
| 10 | Filter-mask pushdown: SiftScan accepts child BitmapIndex paths (btree, BRIN) as `custom_paths` and converts the TIDBitmap into SB page/offset masks before scoring | M7 | Exact filtered top-k without post-filter over-fetch; 5–100× on selective filters | `WHERE body @@> q AND tenant = $1 ORDER BY score LIMIT 10` |
| 11 | Facet counts: reloption `analyzer = keyword` makes a sift index of exact values; `SiftCount` on GROUP BY over the keyword index = popcount(query ∧ value) per value | M7 | ES terms-aggregation-class facets inside PG | GROUP BY facet QPS vs ES |
| 12 | Phrase pair index (g) | M7 | Phrase `count(*)` 10–100× | Phrase count QPS |
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
- **Sanitizers.** `make SANITIZE=1` adds `-fsanitize=address,undefined -fno-omit-frame-pointer`. A scratch cluster runs under `LD_PRELOAD=$(gcc -print-file-name=libasan.so)` with `ASAN_OPTIONS=detect_leaks=0`.
- **Fuzzing.**
  - The query parser via the SQL wrapper `sift.parse_query`, driven by a grammar-aware generator script, under ASan.
  - The codec standalone with libFuzzer, owned by the codec agent; the codec has no backend dependencies.
- **Determinism.** Fixed float evaluation order, ties broken by ctid, and `sift.simd` levels compared bit for bit.
- **Docs.** `README.md` (user guide: DDL, SiftQL, functions, GUCs, limits), `DESIGN.md` (this file), `docs/CODEC.md` (codec agent).
- **Dev loop.** `scripts/pgtest.sh install|start|psql|regress|stop|destroy` runs a throwaway cluster under `$SIFT_TMP` (default `/tmp/sift-arch`, port `$SIFT_PORT` in 56100–56119). It loads sift from a `DESTDIR` staging root through PG18's `extension_control_path` and `dynamic_library_path`; nothing is installed system-wide.
- **Packaging.** `packaging/PKGBUILD` (`sift-pg`) builds the committed HEAD with `nice make -j4` and installs through the standard PGXS `DESTDIR` path.
- **M0 behaviour.** `CREATE EXTENSION sift` works. Any entry point whose module has not landed fails with `ERRCODE_FEATURE_NOT_SUPPORTED` "sift: … is not implemented yet"; for example, `CREATE INDEX … USING sift` fails that way. `amcostestimate` prices the index out of plans, and `@@>` selectivity uses `DEFAULT_MATCH_SEL`.

---

## (k) Limits and non-goals

- **Platform limits.** BLCKSZ 8192 only; heap tables only; server encoding UTF8 or SQL_ASCII (in SQL_ASCII, bytes ≥ 0x80 are opaque letters).
- **Index shape.** One key column of type text or varchar, or an expression yielding text. Partial indexes are supported.
- **Size caps.**
  - Terms are ≤ 255 bytes; longer ones are truncated at a character boundary.
  - dl saturates at 65535 and tf at 255 (score effect < 1.5 %).
  - ≤ 128 segments, ≤ 256 query leaves, ≤ `sift.max_expansions` expansions.
- **Scoring.** k1, b and analyzer options are fixed per index; changing them requires REINDEX. The index only grows to a high-water mark, and REINDEX shrinks it.
- **Non-goals for v1:**
  - positions and proximity / slop queries;
  - `amgettuple` plain and index-only scans;
  - backward scan, mark/restore;
  - score above joins without a subquery;
  - multi-column indexes;
  - custom rmgr;
  - statement-level insert batching;
  - non-x86 SIMD (scalar works everywhere; NEON later);
  - parallel scan (M7).

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
| M5 | Scale | parallel build, ReadStream/AIO, cost model, VM pext fast path | build time and size vs TIN/ParadeDB; cold p99 |
| M6 | Hardening and benchmarks | `verify` (`heapallindexed`), ASan/UBSan, fuzzing, bench harness vs all engines | No sanitizer findings in 24 h of fuzzing; bench report |
| M7 | Beyond-TIN extras | parallel scan with shared θ, filter-mask pushdown, keyword facets, phrase pairs | Each meets its ≥ 10 % gate or is dropped |
