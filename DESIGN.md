# pg_sift: design

`pg_sift` is a PostgreSQL extension (17 and 18; 19 is experimental) whose index access method (AM) `sift` does full-text search. This document is the design of that BM25 index and of the code it shares with the SQLite and MariaDB ports; the column engine (`sift_col`, `sift_table`, the aggregate pushdown) is described in [docs/COL.md](docs/COL.md) and the documents it links. Postings are keyed by heap ctid and stored as two-level bitmaps. It ranks with exact BM25, counts from bitmaps, and checks visibility through the visibility map. The goal is to beat ParadeDB/Tantivy, pg_textsearch, Lucene/Elasticsearch, Meilisearch, Typesense, GIN and rum on:
- top-k BM25;
- boolean and phrase queries;
- `count(*)`;
- build time, update cost, and resource use.

Status: 3.1.0 (extension version 3.1). Of the extras below, 9 (parallel scan, opt-in and off by default), 10 (filter-mask pushdown) and 11 (keyword facets) were implemented in 1.1 (1.0.0 had none of them); the phrase pair index (extra 12) was removed in 1.3.0 for word positions.

This document is authoritative. Each decision gives its reason and the alternatives it rejects. Section numbers match the brief: (a) to (k), (l) for the 1.1 facets, then the module map and contracts. Estimates are marked **[est]**: they are design-time figures, not measurements; measured figures are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md) and [bench/README.md](bench/README.md).

User-facing names: extension `pg_sift`, AM `sift`, schema `sift`, operator `text @@> text`, and the functions `sift.score(tid)`, `sift.max_score(tid)`, `sift.highlight(...)`, `sift.tokenize(...)`, `sift.verify(regclass)`, `sift.segments(regclass)`, `sift.promote(regclass)`, `sift.merge(regclass)`, `sift.parse_query(text)`.

---

## 0. Data model and invariants

- **Key.** A document is the heap tuple at ctid `(blk, off)`. Its key is `SiftKey = blk << 16 | off` (48 bits used). There is no doc-id table, and merges never renumber.
- **Superblock (SB).** 256 consecutive heap pages, `sb = blk >> 8`. Within an SB, the pages a term occurs on form a 256-bit **page mask**, one AVX2 register.
- **Offset mask.** For one page, a 512-bit mask where bit `off-1` is set. It is one AVX-512 register, or two AVX2 registers.
  - `MaxHeapTuplesPerPage` is 291 at BLCKSZ 8192, so 512 bits always suffice. BLCKSZ must be 8192 (static assert).
- **Block.** 16 heap pages, 16 per SB. This is the granularity of score upper bounds.
- **Segment.** An immutable set of postings (or a write buffer of them), plus a docmap that holds the document lengths. Every structure inside a segment is ordered by SB, then by key.
- **Invariants:**
  - **I1:** a ctid is *live* in at most one segment at a time.
  - **I2:** pages of an immutable segment are never modified in place, except its liveness pages. Pages are recycled only after the segment's `safexid` passes the global horizon.
  - **I3:** the directory changes only through one atomic WAL record. Scans copy it once, at start.
  - **I4:** the score of a (doc, term) pair depends only on (tf, dl, df, N, avgdl, k1, b, boost). It is identical in every segment kind and every SIMD level.

I1 holds because heap line pointers are reused only after `ambulkdelete` has cleared the old ctid (VACUUM's two-phase protocol). Any new tuple at that ctid lands in a write buffer. The same tuple is indexed only once: HOT updates add no entry, and non-HOT updates create a new ctid.

---

## (a) Postings, term frequency, document length, and ranked top-k

### Decision

1. **Matching data: the codec blob only.** Boolean and count queries never touch scoring data. The `codec` module (`src/bm25/sift_bitmap.h`, `docs/CODEC.md`) encodes posting sets; one codec group (`gid`) is exactly one SB. sift encodes with the codec's tf option **off**: its 4-bit nibbles saturate at 15 and cost 4 bits on every posting. The scoring sidecar below follows the codec blob in the term blob, so it costs no bandwidth unless a query ranks.
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

   N, avgdl and df count the live documents of the index: documents that VACUUM marked dead are left out of all three (segment format 2, "Liveness" below), so a dirty index scores as the merged one does. Rows of rolled-back transactions and deleted rows count until VACUUM has marked them dead, as in Lucene (which keeps deleted documents in its statistics until a merge). Segments of format 1 (written by 2.x, and by the SQLite and MariaDB hosts) keep counting their dead documents until they are merged.

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
   - **Write buffers:** scan them once with term-hash filters and decode the matching docs. These are exact and few (bounded by `write_buffer_limit`), so they are scored immediately and seeded into the candidate heap.
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
   - **Memory of the top-k.** The heap of the K best hits (K = LIMIT + OFFSET, or 1024 and then four times as many when the executor pulls past it) starts at 1024 slots and doubles as hits are found, in huge allocations with 64-bit counts: it costs 16 bytes per hit found, not per K, so `LIMIT 2147483647` ("all"), an `OFFSET` of tens of millions and a scan drained past tens of millions of matches work (they raised `invalid memory alloc request size` above 67,108,864 slots). The score upper bounds that prune it carry a slack that grows with the number of scored leaves above `SIFT_MAX_LEAVES` (`docs/KERNELS.md`), because a plan has a leaf for every distinct term of its phrases.
   - **Memory of the readers.** A cursor over a sparse term decodes into a buffer of its rows (at most one per posting), a frequent term's cursor into a group's 32 kB; a term's blob is copied whole out of the index (not windowed yet: a term in every row of a billion-row index is hundreds of MB per segment and query). A backend keeps the SB directory of the docmap of segments of 128 superblocks or more (`sift_docdir.c`, 16 MB, by generation), and a dictionary lookup of ids that share their first 8 bytes bisects their run of blocks.

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
  - gen, kind (immutable / buffer / sealed buffer), format;
  - header block, SB range;
  - ndocs, sum_dl, bytes;
  - a dirty flag.

  It also records the open write buffer. A *directory snapshot* is a plain copy of the metapage and directory (I3).

### Immutable segment

A header page holds the extent table and the sections table. Its data is a logical byte stream over **extents**: runs of contiguous blocks, allocated in multiples of 16 blocks so readahead and AIO see long sequential runs.

| Section | Content |
|---|---|
| DOCMAP | One codec blob of all present keys: the universe for NOT, and its group `base` is the doc rank used for liveness. Per SB with docs: `u32 rowstart[npages]` and `u16 dl` rows indexed by offset, located through a sorted `{sb, off}` directory. |
| POSTINGS | Per term, a *term blob* (`sift_posting.h`): `SiftPostHeader`, then the codec blob (`sift_encode` with tf off; the codec picks LIST for sparse terms or GROUPED with its own group directory `key/poff/base`), then the scoring sidecar: `SiftGroupScore[ng]`, `blk_bound[16]` rows, and the tf stream. df ≤ 2 is inlined in the dictionary entry (read with `sift_cur_tids`). |
| DICT | Front-coded blocks of 32 terms in byte order, each entry carrying `SiftTermInfo {df, flags, max_bound, post_off, post_len \| inline keys+tf}`. |
| DICTIDX | One entry per dictionary block: `{u64 big-endian 8-byte prefix, first-term offset, block offset}`. Every 256th entry is copied into a *root array* in the header. |
| LIVE | Separate pages (they are modified in place). Format 2: a header (`ndead`, `dead_dl` sum), the dirty bit of each SB, a *region* per SB with the live offsets of its present pages in the layout of a term's group of bitmaps, and a table with the vacuumed postings of each term. Format 1: one bit per document in segment rank order, plus the dirty bit of each SB. |

- **Dictionary lookup:** SIMD search of u64 prefixes in the root array (cached with the opened segment), then one DICTIDX page, then one DICT block of at most 32 terms. That is ≤ 3 buffer touches, ≈ 0.3 µs when hot **[est]**.
- **Expansion** (prefix, wildcard, regex, fuzzy) runs a range scan over DICT blocks bounded by the pattern's literal prefix (§h).
- **FST rejected:** complex, slow to build, and its main benefit is RAM-resident term indexes, which shared buffers already give us.
- **Liveness group = SB.** A query ANDs liveness only in SBs whose dirty bit is set. A clean SB is a fast path with no liveness decode.
  - **Format 2 (`SIFT_SEGFMT_LIVEROWS`, the segment format since v3).** A dirty SB's region holds the live offsets of each present page, the bits in the layout of a term's group of bitmaps (`sift_grp`). A dirty SB is therefore ANDed with a term's group page by page like a second term, and `count(*)` hands the regions to the same `and_count` kernel: no expansion of rank bits over the present offsets. Each non-inline term has a *slot* (its position among the dictionary's non-inline terms) and a table `u32 dead[nslots]` counts its postings of vacuumed documents, so the live df of a term is `df - dead[slot]` (inline terms, df ≤ 2, are walked: two postings). The header holds the vacuumed count and the sum of their lengths, so N, avgdl and df are exact over live documents. VACUUM rewrites the regions of the SBs it touches, and after its merges recomputes the table of the segments that remain dirty from scratch (idempotent, one WAL record per changed page; a segment it rewrites does not pay for it); `tbl_ndead` in the header is the vacuumed count the table was computed for; it equals `ndead` except after a delete that did not come from VACUUM (a host that deletes one document at a time) or a crash between the regions and the table, until the next VACUUM of the index, which recomputes the table. A counter that finds `tbl_ndead != ndead` recounts from the regions, and `sift.verify` checks the table only when they are equal. Size: a region is `w` bytes per present page (1 bit per document slot of the SB in the offset space, as many bytes as format 1's rank bits), plus the 4 bytes per non-inline term of the table, which is what an index grows by: `scripts/merge-bytes.sh` on 200,000 documents of 12 terms drawn from 5,000 (four segments, so the table is a few pages each) gives +1.4% (with positions) and +2.0 to 2.4% (without) after a build or a merge, +0.1% after churn and VACUUM (2.0 reference).
  - **Format 1 (rank bits).** A dirty SB's liveness bits are one run of its docs' bits in rank order, copied once (`live_sb_bits`) and spread over the present offsets with the `expand` kernel (a PDEP per word). It stays readable (2.x indexes need no REINDEX: a segment is rewritten as format 2 when it is merged or vacuum-rewritten; the metapage minor version becomes 2 when a format 2 segment is written, and a 2.x library refuses such an index with its format-version error instead of misreading it) and is what the SQLite and MariaDB hosts write, since their portable core does not carry the format 2 writer. Its dirty `count(*)` costs about 6.6 to 6.7 times the clean one (`bench/dirty-query.sh` data, user instructions per query), what the decode of the SB's docs and the expansion of every present page's rows add.
  - **Counting a dirty SB.** The dirty bit is read from the segment's bitmap without decoding the SB's docs (`sift_seg_sb_dirty`), so a clean SB of a dirty segment takes the clean path. `count(*)` of a single plain term in format 2 starts from the sum of the segments' live df (`df - dead[slot]`) and corrects it for the pages the heap decides (not all-visible): for each such page, the page's live offsets are counted out and the visible ones counted back in, so a dirty SB costs what a clean one does. The corrections are valid if no VACUUM ran meanwhile: the count re-reads the segment's header at the end (`sift_seg_live_stable`: `tbl_ndead == ndead`, unchanged since open) and recounts from the regions otherwise. Other counts (AND, OR, NOT, filters) cut the group of all-visible pages out of the regions and use the `and_count` kernel. Measured on 200,000 documents with 10%, 25% and 45% deleted and vacuumed (`bench/dirty-query.sh` data, user instructions per query, dirty over the same live rows rebuilt): `count(*)` of a term 1.16, 1.22 and 1.28 times the clean one (18.5 with 2.0.0, 6.6 to 6.7 with 2.0.1), two terms 1.0 to 1.25, three with OR 1.0 to 1.2; ranked AND 1.0 to 1.3, ranked OR of three terms 1.3 to 1.8 and a rare term 1.3 to 2.0 times, which read the dead postings they drop (the volume grows with the dead fraction).
- **Exact per-term counts:**
  - The dictionary entry holds `df`, and the codec group directory gives each chunk's posting count (`base` deltas).
  - In a clean SB where every page is all-visible, `count(*)` adds chunk counts with no offset decode. For OR, this works once page masks are disjoint within the SB (a disjoint-OR shortcut, applied per SB). Otherwise sift uses the codec count kernels (`sift_grp_count_mask`, `sift_grp_and_count`).
- **Statistics.** `ndocs`, `sum_dl` and `avgdl_ref` per segment live in the header and the directory entry. The metapage holds totals for immutable segments; write buffer totals are summed during the write buffer scan. A query binds the live totals: a segment's `ndocs - ndead`, `sum_dl - dead_dl` (format 2; `sift_seg_live_totals`) and a term's live df (`sift_seg_term_stats`), and the write buffer's live documents, so N, df and avgdl equal those of the merged index. The score upper bounds stay valid for them: they scale the tf part only, and a smaller avgdl lowers w.

**Size model** **[est]**, Stack Exchange-like corpus of 150 M documents with about 100 distinct terms each:
- codec postings: per CODEC.md;
- tf: ≈ 2 bits per posting on dense chunks, ≈ 0 on rare terms;
- bounds: under 0.1 bits per posting;
- docmap: 2.3 B per document;
- liveness: format 2 about 1 bit per document slot in the offset space, plus 4 bytes per non-inline term; format 1 1 bit per document;
- dictionary: about 12 B per distinct term.

**Target:** an index no larger than 60 % of the indexed text.

---

## (c) Write buffers, promotion, merge (no preload required)

- **Insert path (`aminsert`).**
  1. Analyze the document into (term, tf) pairs and dl.
  2. Append one *doc record* to a page of the open write buffer: `{tid, flags, nterms, dl, u32 hash[nterms], u8 tf[nterms], u8 len[nterms], term bytes}`.
  3. Each data page header carries a 256-bit bloom of its term hashes and its counters (ndocs, sum_dl).
- **Lanes.** A write buffer has 8 lanes (`SIFT_LANES`). Each lane is an append chain whose tail block is stored in a lane-header page.
  - A backend picks lane `MyProcNumber % 8`. If `ConditionalLockBuffer` fails it tries the next lane, and blocks only after a full round.
  - This removes GIN's single-tail hot spot.
  - One insert is one generic-WAL record over 1–3 pages (tail, new page, lane header), logged as a delta of roughly the record size.
- **Search.** Read all pages of the write buffers, skip a page whose bloom misses the query, and use a SIMD compare of u32 hashes per record. A hash hit is confirmed against the term bytes. Matches are decoded into an in-memory mini-segment, using the same scoring (I4).
  - **Limit of the page bloom.** With two probes in 256 bits a term is a false positive with probability `(1 - e^(-2n/256))^2` for a page that holds `n` distinct terms: 1% at 20 terms, 15% at 60, 50% at 130, 82% at 300. A page of documents with a hundred terms each (a few documents) therefore gates almost nothing, and a lagging maintenance worker (write buffers that grow before they are promoted) costs a full read of every page of them per query. The filter is part of the page header (format), so a larger one, or a per-page hash set, needs a format bump with the next one; until then the cost is bounded by `write_buffer_limit` (promotion) and the worker. Wildcard, regex and fuzzy leaves never use it.
  - Cost is bounded by `write_buffer_limit` (reloption, default 4 MB): about 0.1–0.2 ms per query worst case **[est]**.
- **Sealing.**
  1. When the active segment exceeds `write_buffer_limit`, the inserting backend *seals* it in one directory record: the active entry becomes sealed and a fresh active entry is added.
  2. A concurrent appender holds the lane tail lock while appending and re-checks `sealed` under that lock.
  3. The sealer takes each lane tail lock once after flipping the flag, so no append is in flight afterwards.
- **Promotion** turns a sealed segment into an immutable one.
  1. Read its records and sort them by key, in a tuplesort: each live record is packed into a bytea (key and arrival number big-endian, so byte order is key order and the first record of a repeated tid comes first), which keeps the promotion within `maintenance_work_mem` whatever the size of the segment (`write_buffer_limit` goes up to 1 GB); the excess spills to temporary files. It used to copy every record to memory, about six times the size of the segment: 800 MB at a 16 MB budget for a segment of 138 MB. The inverted buffer then gets what the sort left of the budget when the sort stayed in memory, so a segment that fits promotes as before.
  2. Build with `SiftInvBuf` and write new extents through shared buffers, logged with `log_newpage_range`.
  3. In a single directory record: replace the sealed entry with the new immutable entry, update metapage totals, andfree the old pages with `safexid = ReadNextFullTransactionId()` (a lazy VACUUM, which is left out of snapshots and of the horizon, its xid included, releases with the next xid whatever step it runs; its own truncation under `AccessExclusiveLock` counts every released extent as reusable).

  Records flagged deleted are dropped. A crash before the switch leaves orphan pages, which the sweep reclaims (§i).
- **Who promotes, in this order:**
  1. **Async dynamic background worker.** `RegisterDynamicBackgroundWorker` works without `shared_preload_libraries`. The worker's entry point is resolved from `pg_sift.so` by name.
     - It is launched by the sealing backend if `sift.async_maintenance = on` (default) and a slot is free.
     - At most one worker per index. A second worker finds the worker lock (`ConditionalLockPage(index, 3, ExclusiveLock)`, held by the step in progress) taken and leaves; launches are also rate-limited by a `maint_requested_at` stamp on the metapage.
     - The worker runs each promotion or merge in a transaction of its own, holding no snapshot: its xid and xmin would otherwise keep the extents it frees from being reused (their `safexid` is its xid) until it exits, and a long cascade of merges grew the file to 2.5 times the live segments. A step finds the free space the previous one released, the horizon being computed anew at each allocation (the GlobalVis one a backend caches moves only when it takes a snapshot). The streams of a step know the size they are expected to need (the inputs of a merge, the pages of a sealed segment) and take runs of free space down to a 256th of it, so the fragments left by merged segments are reused instead of extending the file; 60 small segments merged by the worker leave a file of 1.6 times the live segments (was 2.6). An old snapshot elsewhere (an idle transaction) still keeps everything released after it from being reused, whatever the worker does: the P4 test is by xmin, and only a registry of the readers of a directory copy (shared memory) could do better.
  2. **`amvacuumcleanup` / `ambulkdelete`.** Autovacuum's insert threshold triggers these on insert-only tables, the same way GIN's pending list is cleaned.
  3. **Back-pressure.** If total sealed bytes exceed `4 × write_buffer_limit` and the maintenance lock is free, the inserting backend promotes synchronously. If the lock is taken, it keeps appending: queries get slower, but writes never block.
  4. **Manual:** `sift.promote(idx)` and `sift.merge(idx)`, for example from pg_cron.
  - A static launcher that scans databases for indexes with pending maintenance when the library is preloaded was designed (`sift.launcher`) and never built: preloading changes nothing, and the dynamic worker above is the only background process.
- **Merge, tiered policy.** Tiers grow by ×8. When at least `merge_fanin` segments (reloption, default 8, 2 to 32) share a tier, or the segment count exceeds 32 (`max(32, 2 * merge_fanin)`), merge the smallest qualifying run. A merge reads at most 16 GB of segments (the smallest of the run that fit; `sift.merge(index, true)` too), so a segment past that stays as it is: its output could otherwise need more extents than a segment header page lists (about 1000). The runs of a stream grow with it (16 blocks doubling to 1024, then a 16th of what is written up to 8192, a 64 MB extension under the metapage lock), so a 16 GB segment takes about 300 extents even when another writer allocates between any two runs, where fixed runs of 1024 blocks took 1000 at 8 GB and failed the merge, VACUUM's cleanup and the insert that merges for room. A built segment is contiguous and has no such bound.
  - The merge streams a k-way merge of the dictionaries.
  - Per term, chunks are combined in SB order:
    - **Disjoint SBs:** byte-copy of the chunk, tf stream and bounds, with no re-encode.
    - **Overlapping SBs:** decode, OR (disjoint by I1), re-encode.
    - **Dirty SBs:** AND with liveness, dropping dead postings. This is the only garbage collection. A forced merge (`sift.merge`) also rewrites a lone segment that has dirty SBs, so its dead terms leave the dictionary.
  - Bounds are recomputed only for rewritten chunks.
  - Memory is O(chunks in flight), not O(segment). Merges run in maintenance contexts (worker, vacuum, manual) and, as back-pressure, in the foreground (next item).
  - **What is and is not bounded today.** The k-way merge runs over temporary sorted runs: each input segment is first dumped to a run file (`sift_run_from_segment`), then `sift_runs_write` merges the runs, with the readers kept within `maintenance_work_mem` (`sift_shrink_runs` merges the smallest runs into temporary ones first when more would not fit). The memory is bounded; the temporary space is not nil, and the postings are written twice (10.5 MB of temporary files for the 60-segment backlog of `scripts/perf-gate-maint.sh`, `merge_temp_kb`, at a peak of 6 MB of anonymous memory, `merge_peak_mb`). Promotion no longer copies a segment to memory (see Promotion), and an inserter waits for a step only when everything left to merge is claimed by it (Locks, §i), so neither the memory of a promotion nor the stall of an inserter grows with the size of the index.
  - **Streaming merge: memory independent of the segments.** The memory of a merge does not grow with the size of the segments (the old ~50 B a document: the keys and lengths of every input, the postings of a term collected before it was written, the whole position stream of a segment, nothing freed between inputs). `sift_seg_stream` (in `sift_segment.c`; `sift_seg_dump` is a collecting adapter over it, for the facets and the tests) walks a segment one term at a time and reads the document lengths from a cache of per-superblock rows (LRU within half of `maintenance_work_mem`; a length of 0 is a document that is absent or dead, so no array of every key is built), and the positions through a window (`sift_post_pos_reader`). The writer takes a large term (at least half of what a segment's budget holds, and `grouped_min_df`) in the same pieces the format has, because the groups of a grouped term are independent: `sift_postw_spool` stages the tf stream and the positions of each group in temporary files and, at the end of the term, writes the header, the groups and the staged streams in the order of the format, so the blob is byte for byte the one built in memory (terms below 8192 postings, lists, stay buffered). The dictionary index goes through a temporary file as well (`sift_dict.c`, `sift_sw_write_file` copies it). The run file format and the segment format are unchanged; `scripts/merge-bytes.sh` compares the index files of two builds page by page after a build, merge, churn and rewrite. Left O(size): the O(superblocks) arrays (the docmap directory 24 B, the spool index 36 B a group, the cache's pointers 8 B a superblock), and `sift_post_open` still copies the blob of the term it reads (ROBUSTNESS F-05). The run files and their second write of the postings stay (`merge_temp_kb`): the inputs are not read straight into the writer. `sift_wbuf_*` and the claims are not involved.
- **Worker lifetime.** `sift.maintenance_naptime` (default 2 s, 200 to 60000 ms) is both the shortest time between two worker launches for an index (`maint_requested_at`, times the failure backoff) and the time a worker stays after it started: it looks for work every naptime/20 (100 ms to 1 s) and makes its last look after the window ended. A seal inside the window asked for no worker of its own (the request path is rate-limited by the stamp), and without the lingering its sealed segments stayed unpromoted after a bulk load until the next seal or a VACUUM (queries read them as write buffer data: 8 to 16 ms against 1.3 ms). A failed step ends the worker, as before. Cost: one worker slot per loaded index for the window.
- **Dirty segments.** VACUUM leaves segments with partly dead documents dirty (rewritten only when more than `dead_rewrite_pct` of it is dead). In format 2 the term `count(*)` costs 1.16 to 1.28 times the clean one and the statistics are exact; queries that walk postings (OR of terms, a rare term, a scan) still read the dead postings they drop, up to 2 times the clean cost at 45% dead. Per-term dead counts fix the counts and the statistics only; dropping the postings themselves is the merge's job.
- **Prior art for dirty segments.** Lucene (and Elasticsearch, OpenSearch), Tantivy and ParadeDB keep deleted documents in segment statistics (`maxDoc`, df) until a merge and drop them from results with a per-segment live-docs bitset (`numDocs = maxDoc - delCount`; Tantivy's alive bitset); none keeps a live df per term. GIN and RUM remove dead TIDs from posting lists in `ambulkdelete`, which immutable segments cannot do. The live bitset ANDed with a posting bitmap and counted is Roaring's `andCardinality`; the per-SB regions are that over the two-level layout. Block-max bounds taken over all postings stay valid upper bounds over the live subset, so VACUUM does not rewrite them. Here the stored extra is the per-term dead-posting counter, which makes the statistics exact and the term count a subtraction.
- **Directory back-pressure.** The directory holds 128 entries. When the async worker lags, a sealing backend that finds 112 or more entries (`ROOM_SEGMENTS`) promotes and merges the segments nobody else is working on until the directory is below that, before it seals. A merge of the worker, however long, does not stall it: it takes the segments the worker has not claimed (the policy leaves out the claimed ones), and only when everything left to merge is claimed does it wait for the holder of one of those claims, whose step frees entries. An insert never fails for lack of entries and memory stays bounded (the active segment is capped by `write_buffer_limit`). Cost: while VACUUM holds the maintenance lock exclusively (all of `ambulkdelete`), such an inserter waits. An error remains only if 112 entries cannot be merged.
- **Privileges and logging.** `sift.promote`, `merge`, `verify`, `segments`, `segment_postings` and `index_info` call `sift_check_maintain` (`src/core/sift_ops.c`): the owner of the table or `MAINTAIN` on it, the check `VACUUM` and `REINDEX` use. The background worker and VACUUM run as the system and are not checked. Promotions, merges and worker runs lasting at least `sift.log_min_duration` (default 1000 ms) are logged at LOG with index, documents, bytes and time, as are an insert that waited that long in `sift_maint_make_room` and (once a minute) a maintenance worker that `RegisterDynamicBackgroundWorker` refused; an error in the worker names its index in the context. The lock state is also visible as a `page` lock wait in `pg_stat_activity` and `sift.index_info(index).maintenance_busy`.
- **Rejected:**
  - *In-place write buffer term trees*: one page write per term per insert, so ≈ 100× the WAL of an append record.
  - *Shared-memory write buffers* (DSM registry): data would be lost on crash unless WAL-logged anyway, and they add a second search path.
  - *Synchronous promotion in every insert*: latency spikes.

---

## (d) WAL strategy

| Operation | Mechanism | Reason |
|---|---|---|
| CREATE INDEX / REINDEX | `smgr_bulk_*` (`storage/bulk_write.h`). Batched `log_newpages`, skipped under `wal_level=minimal` for a new relfilenode, writes bypass shared buffers, and there is one fsync at finish. | The fastest supported path, the same as nbtree's in PG17+. |
| Promotion / merge pages | `ExtendBufferedRelBy` into shared buffers, fill, `MarkBufferDirty`, then `log_newpage_range` (32 full-page images per record) before the directory switch. | The bulk API needs exclusive ownership of the relation's tail. Concurrent inserters extend the same relation, so it cannot be used here. |
| Directory switch, metapage totals, sealing | Generic WAL, ≤ 4 buffers, in the fixed order metapage → directory → others. | Atomic and small. |
| Write buffer appends | Generic WAL delta. | The record is about the appended bytes. |
| Liveness updates (VACUUM) | Generic WAL delta per LIVE page and the dirty-SB page, up to 4 per record. | Only changed bytes are logged. |
| Freeing extents | Same record as the directory switch (free-list entries carry `safexid`). | Crash consistency. |
| Truncating the end of the file (VACUUM) | One generic WAL record writes the free list without the free run at the end and lowers the high-water mark; then `RelationTruncate` (its own WAL record, flushed) cuts the file. | The record first: a file longer than the mark is harmless (allocation extends over it, the next truncation cuts it), a free list that reaches past a shorter file is not. |

- **Custom rmgr rejected for the index.** `RegisterCustomRmgr` only works from `shared_preload_libraries`. If the rmgr is missing, recovery crashes, including on replicas that never loaded the library. Generic WAL keeps sift preload-free and replica-safe. (`sift_table` follows the same rule for every page change; it emits one custom-rmgr logical change record only for a relation that is published under logical replication, which is why that case needs the library preloaded: docs/COL-TAM.md.)
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
  - Opt-in `sift.standby_conflicts` (superuser) makes the primary log a btree page-reuse record for the index before it reuses released blocks (`log_reuse`, once per backend and horizon: a newer `safexid` covers an older one). The standby replays it as the usual recovery conflict with the snapshots older than `safexid`: it waits for them up to `max_standby_streaming_delay`, then cancels them. It touches no page. It is off by default because it also delays or cancels unrelated standby queries whose snapshot is older, and adds one WAL record per reuse horizon.
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
- **Order of reads.** A VACUUM removes the index entries of the dead tuples (`ambulkdelete`: liveness bits cleared, the dirty flags set) before the heap marks their pages all-visible, so a page is all-visible only once the liveness behind it shows the removals. That holds for liveness read after the bit. A scan reads its liveness (the directory's dirty flag of a segment, the dirty-SB bitmap, the live bits of an SB, the write buffers' records) lazily and keeps what it read, and it reads the VM bits of an SB when it reaches it, so some liveness is always older than a bit. `ambulkdelete` therefore ends, when it removed anything, with a directory update (the version moves), and every scan reads the directory version after each VM read (`sift_vis_sb_mask`): once the version differs from the one the plan read before it opened any segment, the mask is zero and the heap decides every candidate for the rest of the scan. A merge or a promotion moves the version as well, which covers the segments a scan holds after a merge replaced them (a later VACUUM clears the bits of the new ones only). The cost is one unlocked read of the pinned directory page (`sift_dir_version_peek`) per VM read of an SB that has an all-visible page, and heap checks only for a scan that overlapped a VACUUM, a merge or a promotion.
- **Reading the VM.** sift reads the VM fork directly. Its layout (`HEAPBLOCKS_PER_BYTE = 4`, 2 bits per page, `MAPSIZE = BLCKSZ − MAXALIGN(SizeOfPageHeaderData)`) has been unchanged since 9.6.
  - Calling `visibilitymap_get_status` per page costs about 5–10 ns × 5 M pages, ≈ 25–50 ms. The SIMD path costs 64 B per SB.
  - The layout constants are pinned by a regression test that compares against `visibilitymap_get_status` on random pages.
- **Non-MVCC snapshots** (SnapshotAny or SnapshotDirty in RI checks, CLUSTER) always use heap checks.
- **The table must use the heap table AM or `sift_table`**, checked in `ambuild` ("sift indexes require a heap or sift_table table"). Keys assume heap ctid semantics; a `sift_table` presents virtual block numbers with the same shape (docs/COL-TAM.md).

---

## (f) Planner and executor integration

Hooks used, all present in the PG18 headers (verified):
- `set_rel_pathlist_hook` (`optimizer/paths.h`);
- `create_upper_paths_hook` (`optimizer/planner.h`);
- `CustomPath`/`CustomScan` with `CUSTOMPATH_SUPPORT_PROJECTION` (`nodes/extensible.h`);
- `find_ec_member_matching_expr` (`optimizer/paths.h`);
- `SupportRequestSimplify` and `SupportRequestIndexCondition` (`nodes/supportnodes.h`).

**Hook installation does not need preload.** The planner opens every index's relcache entry in `get_relation_info`, which calls the AM handler and so loads `pg_sift.so`. `_PG_init` installs the hooks, chaining any previous ones. This happens before `set_rel_pathlist` runs for that query. The `@@>` support and selectivity functions also live in `pg_sift.so` and load it even earlier. This follows from reading the code; no test isolates it.

1. **Plain AM paths.** `amgetbitmap` supports Bitmap Heap Scan and BitmapAnd/BitmapOr with other indexes. `amgettuple` is NULL; there are no plain index scans or index-only scans. `amcostestimate` uses real term statistics.
2. **`SiftScan` custom path** (added in `set_rel_pathlist_hook`). Conditions:
   - the rel is a base rel or partition child;
   - it has a sift index;
   - `baserestrictinfo` contains `key @@> q` where `key` equals the index key expression and `q` is a pseudo-constant (Const or Param).

   All such clauses are ANDed into one query. The remaining quals become scan quals. There are two variants:
   - **Unordered.** Heap-order emission with a fused visibility check. A `ReadStream` gets heap blocks from the match iterator for AIO prefetch (perfect, because order is known), and also feeds index chunk pages ahead.
   - **Ranked.** Added when `root->query_pathkeys[0]` is `sift.score(<this rel>.ctid) DESC`, matched with `find_ec_member_matching_expr` (this also handles partition child members). The path's pathkeys are that first pathkey, so no Sort is needed. Partitioned tables get MergeAppend over per-child ranked scans (see "Partitioned parents" below). The k hint is `root->limit_tuples` when nothing sorts above the scan (`scan_limit`): a LIMIT with no ORDER BY hints too, and a scan under a Sort is drained, so it has no hint and ranks every match in one search (`rank_ensure` searches with an unbounded K when k is 0; it used to search for 1024 hits, then 16 times as many, then 4 times as many, each from scratch, and verified and fetched every match each time).
   - **Keyed (`ORDER BY <columns>, sift.score(<this rel>.ctid) DESC LIMIT n`).** With the score not first, the Sort above a ranked scan needs every match, so the planner adds a second path, chosen by cost, that declares the order (leading keys, then score DESC) as its pathkeys: no Sort, or an Incremental Sort for keys after the score. The keys are the leading pathkeys that have a member reading only this rel's columns (not volatile, no sub-plan, no score), up to the score key DESC or the first key that is not such a member (`keyed_order`); `SIFT_PRIV_KEYOPS` holds the sort operator, collation and NULLS FIRST of each, and the key expressions are the fourth list of `custom_exprs` (queries, residual quals, limit params, key expressions). It needs a LIMIT or a LIMIT Param, no partition, no `sift.max_score` of the rel (the best score of all matches) and no parallel plan. The executor works in rounds (`keyed_search`): (1) a pass over the matches in heap order (the unordered iterator, each heap page once) evaluates the keys and keeps in a max-heap the `kheld` rows with the best keys and the tids of every row tied with the worst of them; (2) an exact `SiftFilter` of those tids restricts the ranker, which runs unbounded, so only those rows are verified and scored; (3) the rows are sorted by keys, score DESC and heap order and emitted. The first round keeps k + 1 rows (an Incremental Sort reads one row past the limit), or 1024 without a hint; a consumer that asks for more (OFFSET, a filter above, an Incremental Sort that needs the rest of a group) gets another round that keeps `max(sift_rank_next_k(kheld), rows so far + 1)`. The answer is the one of ranking every match and sorting after: the ties of the k-th key are all scored, and the order inside them is the sort's. `keyed_path` charges a pass (index, a tuple and the key operators per match, the heap) times the rounds the limit may need (log base 16 of the matches over the rows kept) plus the ranking of the kept rows. EXPLAIN shows `Order: name, score DESC`, `Limit Hint` and `Key Passes`; `Candidates` is the last round's. Regressions `rank_sort_key` (the heap checks and the answers of ties, NULLs, DESC, OFFSET, joins and prepared plans against ranking everything) and `rank_unbounded`.
     - Measured on 100K rows, 40,122 matches (user-space instructions of the transaction, before to after): `ORDER BY name, score DESC LIMIT 5` 170.5M to 53.9M, `ORDER BY created_at DESC, score DESC` 184.3M to 51.0M, the same through a join 160.6M to 55.2M, `ORDER BY id DESC LIMIT 5` 169.4M to 63.3M; heap checks 108,994 to 42,748 (1.07 per match, was 2.72); a drained `ORDER BY score DESC` 193.7M to 155.1M (heap checks 80,244, two per match). `ORDER BY score DESC LIMIT 5` is unchanged (1.00).
     - Known limits: a generic plan whose query term is unknown rarely takes the keyed path (the drained plain path is priced at one heap access per match, the executor makes two); when the first key group is a tie larger than the limit and an Incremental Sort sits above, the scan takes a second pass; a partition, a leading key of another rel of a join and the filter pushdown keep the plain path; PostgreSQL 17 has its own `sift_pathkey_sortop` branch in `src/core/sift_compat.h`.
     - Not done: walking a btree on the leading key and scoring the rows it reads (a point score by tid). The index stores postings by term and bounds by block, not a score by document, so it needs a new primitive (a seek in each query term's postings per row) and a path that merges a btree with the match set, which is not small; the keyed scan reads the matches once and ranks only the boundary rows, which already removes most of the work.
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
   - `simplify_match` (src/bm25/sift_funcs.c) needs `root` in the request and leaves the call alone when it is NULL; test `fields` checks the binding in the plans.
6. **Costs.** `sift.matchsel` and `amcostestimate` estimate selectivity from the index's term statistics: df for terms, an independence product for AND, inclusion-exclusion capped for OR. Lookups cost ≈ 1 µs per term per segment at plan time. The planner asks for the same estimate several times per statement (the selectivity of the clause, the cost of each path); `sift_estimate_cached` (src/bm25/sift_estimate.c, PostgreSQL only; the estimate itself is portable) keeps the last 8 answers of a backend under the index file, the version of its directory (bumped by every change of a segment, so a promotion, a merge, VACUUM and REINDEX are estimated again), the expansion cap and the query texts (a query over 8 KB is not kept). An estimate reads the dictionaries of the immutable segments only, never the write buffer, so nothing else decides it. A count statement spent 28% of its cycles planning before (1.18M to 1.03M cycles per statement with the memo). Regression `plan_estimate`.
   - Ranked SiftScan cost: `startup = UB pass + estimated k-block work`. It is strictly cheaper than Sort over a bitmap scan when there is a LIMIT. The total is not the startup: drained past its hint (`sift_rank_next` searches again for 16 times as many hits, 4 times beyond 4096) the scan ranks every match, so the total is the cost of the scan without a limit, and a consumer that stops after a fraction of the rows (a Limit, a Nested Loop Semi Join under it) interpolates from the startup to it, while a hash join or a sort that drains the scan pays it all. The scan is the only path of its rel, so the pick between its paths (filter pushed down or not) is by the cost of the k rows (`pick_cost`).
   - **Filter pushdown (`sift.filter_pushdown = off | on | force`, default `on`).** For a ranked scan the planner also builds the relation's bitmap path over its other indexes (btree, BRIN, GIN, BitmapAnd/Or) and adds a second SiftScan path whose child is that bitmap. The executor turns the TIDBitmap into per-superblock page masks and per-page offset masks (`src/core/sift_filter.c`) before the ranker starts; the ranker drops superblocks and blocks with no kept page and ANDs the masks into every block it scores, so only tuples the bitmap keeps are verified. The quals stay as residual quals, so lossy pages, BRIN and GIN rechecks, MVCC and `sift.match` results are the same as without pushdown. The mask holds at most `work_mem` bytes of 64-byte page rows; pages beyond it are kept whole (lossy). Cost: the bitmap scan, plus a charge per bitmap tuple and mask row (the tuples are the bitmap's selectivity, not the planner's lossy-page inflation of them), with the page count corrected by the btree column's correlation, against the post-filter estimate of verified candidates. The post-filter scan visits superblocks in heap order with no threshold until it holds k passing rows, so it verifies every match before the first row the quals can keep, `k / pass` of them (with the logarithmic overshoot) within the span of the rows that can pass, and past the span the matches that beat the k-th best of all passing rows (`k / (matches * pass)`). The span comes from the btree quals of the bitmap (`=`, ranges, `= ANY(const array)`; BitmapAnd intersects, BitmapOr unites) placed in the heap through the histogram of the column (`col < min`, `col > max`) and its correlation (squared, as for the pages); a filter with no such quals, or a column uncorrelated with the heap, spans the whole heap, which is the plain `k / pass` estimate. Both paths also pay for the blocks the ranker scores (`SCORE_BLOCK_COST` per block, in place of a per-posting charge): a block is scored when its bound can still beat the threshold, which is every block until k rows are held and, when superblock bounds tie (probability `1 - exp(-(matches per superblock / 3000)^2)`, so a broad term ties and a rare one does not), the blocks of the rows' span in heap order. A `= ANY(const array)` bitmap is priced as ordered descents: elements on one leaf page share a descent (PG 17+), so a list over hot data costs its distinct leaf pages, not one random page per element (`saop_saving` re-runs the btree estimate with `random_page_cost = seq_page_cost`). `force` takes the pushdown whenever a bitmap path exists. EXPLAIN shows the Bitmap Index Scan child and `Filter Superblocks / Pages Exact / Pages Lossy`. A query the planner cannot read (the parameter of a generic plan: prepared statements, `plan_cache_mode`) has no match count, and the operator's default (0.5% of the rows) made a filter after the ranker look free, so a generic plan never pushed the filter down (a prepared filter query took 1.8 times the cycles of the literal one). A ranked scan now prices such a query as one that matches a tenth of the rows (`UNKNOWN_QUERY_SEL`; the ranked classes of the benchmark match 26% to 78% of the articles) and takes the rows that pass from the quals' own selectivity, so a selective filter is pushed down in the generic plan as in the plan of the literal query, and a loose one stays after the ranker. Wrongly pushing down costs the bitmap (forcing it on every query of the filter class: +12% cycles), wrongly filtering after costs up to twice that class's cycles. Regression `filter_pushdown_prepared` compares both plans and the answers of every mode. A filter that is an equality on a one-column btree (`country_code = 'US'`) is placed in the heap by reading the btree while planning, as the planner does for the end of a range (`get_actual_variable_endpoint`): the entries of a key are in heap order, so the nth of them (n = the limit divided by the fraction of the rows the query matches, at most 4096 entries read; the last entry too) is the heap position before which a post-filter scan verifies every match, and where the rows end. The column's statistics cannot say: a value stored in one stretch of the heap (rows loaded by country) leaves the correlation near zero, and the plan priced the post-filter scan as if the rows that pass were spread over the heap (a one-letter prefix of the places filtered by country: the scan verified 9,434 of the 12,700 matches, the plan expected 1,000). A stretch at the start of the heap keeps the post-filter scan; one that starts late is pushed down. Regression `filter_pushdown_clustered`; `bench/steps/ab.sh shapes` times the shapes of both plan-choice tests under the planner's choice, `off` and `force`.
   - **Filter pushdown in generic plans.** A generic plan has no query to count: a common query wants the mask (the post-filter ranker verifies the heap in order until it holds k rows that pass), a rare one pays for the bitmap more than the verification would cost. So `unknown_query_path` keeps the bitmap as the child of one scan and stores a window of match counts (`SIFT_PRIV_GATE` and `SIFT_PRIV_GATE_TO`, found by `push_window`: the cost of the pushed down path over the cost of the post-filter one on a geometric grid of match counts from 1 to the rows, then six bisections at the edges; the filter's place in the heap is the one estimated for a tenth of the rows). The window opens where the ratio falls under a quarter (`GATE_OPEN`: the planner charges a random page for every candidate the post-filter ranker checks, which data in memory does not pay, so only a bitmap that is clearly worth its build is built: measured, a margin of one half made a prepared rare AND query with a 10% filter 7% dearer in instructions) and closes where it reaches one (`GATE_CLOSE`); no window, no child. `start_scan` binds the query first and builds the bitmap only if `sift_plan_docs` (the estimate of `sift_eval_estimate`, from the bound dictionaries) is inside it; otherwise the child never executes (`never executed` in EXPLAIN ANALYZE) and the quals, which stay residual, do the filtering. The plan's cost is the one of the post-filter scan at the operator's default match fraction, whatever the query will be: the plan cache compares a generic plan with the average of the custom ones, and a plan priced by an assumed tenth of the rows would lose to the custom plans of every rare query and be planned again each time (measured: +14% instructions on a prepared rare AND query with a 2.3% filter). A custom plan knows its query and takes the cheaper path outright; `force` takes the pushdown at once. A parallel path for such a scan is not built (the bitmap is built by one backend). Regression `filter_pushdown_gate`; `bench/steps/ab.sh filters` and `filter_matrix` measure the prepared shapes at 0.1, 2.3, 10 and 50% filters, in the generic, the custom and the automatic plan cache mode.
   - **Parallel scan (`sift.parallel_scan`, default off).** With the GUC on, the planner also adds partial paths: a parallel-aware SiftScan (workers take morsels of superblocks from a shared counter; a ranked scan shares its threshold θ through DSM and the participants' top-k lists are merged by the last one to finish, so rows, scores and ties equal the serial plan) and a partial `SiftCount` under a Finalize aggregate. Workers follow the planner's index-scan rule (`parallel_workers`, `min_parallel_index_scan_size`, `max_parallel_workers_per_gather`) and never exceed the superblocks of the heap. A ranked parallel path needs a known `LIMIT` and a single relation (the score must reach the target list, which a Gather below a join would not pass on). A plain Sort over a Gather of a ranked scan is removed, because it would lose the tie order. It is off by default because a Gather costs milliseconds before any work (see item 9: it pays only for unordered scans with a heavy residual qual or on much larger corpora). With the GUC off no parallel path exists and plans are the serial ones.
   - **Parallel scan and filter pushdown.** The mask of a pushed-down filter is built from a bitmap child that one backend executes, so a SiftScan with a bitmap child is never parallel-aware: workers would each rebuild the bitmap, and sharing the mask in DSM would add a build barrier and a copy per query that the measured gains (item 10, serial) do not need. `add_scan_paths` therefore plans the parallel (partial) path only for the scan without a filter child, next to the serial path with and without the pushdown, and the planner chooses by cost: the pushed-down serial scan or the Gather over the unfiltered parallel scan. `sift.filter_pushdown = force` takes the pushdown whenever a bitmap path exists and therefore plans no parallel path for that scan. Regression `parallel_pushdown` runs all six combinations of the two GUCs and compares ranked top-k (ctids, scores, order), matches and counts with both off.
7. **EXPLAIN.** `ExplainCustomScan` prints the query (deparsed), mode, k, segments (immutable/write buffer), SBs and blocks scored or skipped, candidates, heap checks, final θ, and pages prefetched. ANALYZE prints actual counters.
8. **Coverage and what has no SiftScan.**
   - *Relations:* tables (heap or `sift_table`) and materialized views (`RELKIND_RELATION`, `RELKIND_MATVIEW`); not foreign tables, views or system catalogs.
   - *Array quals:* `key @@> ANY (array)` is an index qual of the bitmap path only (`amsearcharray` is false, so the executor runs one index scan per element and ORs the bitmaps). `amcostestimate` skips its term estimate for it (`qual_query_exprs` returns no queries), the selectivity comes from `scalararraysel` over `sift.matchsel`. `ALL` and `NOT ANY` are not index quals. There is no ranked or count scan for an array, so `sift.score()` raises the usual error.
   - *Row locks and writes:* a `SiftScan` cannot re-check a locked or updated row (a CustomScan has no EvalPlanQual recheck), so the hook adds no path when `root->rowMarks` is set or the command is not a SELECT, and `reject_scores` raises "sift.score() is not available in FOR UPDATE and FOR SHARE / UPDATE / DELETE / MERGE" when the statement reads a score of that rel (target list, `RETURNING`, quals, sort key). The same statements without a score use the bitmap path.
   - *LIMIT $n:* `root->limit_tuples` is the k hint, and a generic plan has none for a parameter. The ranked scan then carries the `LIMIT`/`OFFSET` Params in `custom_exprs` (third list, `limit_params`; only for a plain ungrouped, non-`WITH TIES` query, `param_limits`) and evaluates them in `sift_begin` unless it is an EXPLAIN without ANALYZE: k = LIMIT + OFFSET, capped at 1M (`LIMIT_HINT_MAX`), where it used to stay `RANK_INITIAL_K` (1024) and search again fourfold past it. A scan under a Sort carries no Params (the Sort drains it, so it ranks every match in one search). Costing is unchanged (a generic plan cannot cost a parameter).
   - *SERIALIZABLE:* `sift_begin` takes `PredicateLockRelation` on the heap; the access method sets `ampredlocks = false`, so PostgreSQL also locks the whole index for a bitmap scan and checks the index relation at every `aminsert`. Finer locks were examined and not built: a document is appended to the write buffer, not to the page where its terms are read, so a page lock cannot detect a phantom; a correct lock is one per searched term (and per vocabulary range of a wildcard, prefix or regex), for instance a predicate lock on a TID derived from the term hash that `aminsert` checks for each term of the new document. That needs the term hash of every leaf at lock time and a check per inserted term; not done.
   - *Row-level security and security barrier views:* the match is not leakproof, so `sift_match_clause` requires `restriction_is_securely_promotable` (as an index qual does) and `match_selectivity` returns the default for a rel with `securityQuals`: a role under a policy gets no SiftScan, no SiftCount and no estimate from the index, the rows come from a sequential scan filtered after the policy, and `sift.score()` raises "is not available ... while row-level security or a security barrier view applies". Before 2.0.1 the scan and the estimates evaluated the match before the policy: ranked scores used N, df and avgdl of all rows (with a policy hiding 3,000 of 3,020 rows a visible row scored `ln(1 + (3020 - 20 + 0.5) / (20 + 0.5))`) and `EXPLAIN` costs reflected the hidden postings; the rows returned were always only the visible ones. Test: `rls_stats`.

---

## (g) Phrase queries

- **Decision.** A phrase is the AND of its terms at the bitmap level, then a check of adjacent positions: from the positions stored in the index where the segment has them (below), else by a recheck that re-tokenizes the candidate's heap text with the index analyzer. The rest of this section describes the heap recheck, which `positions = off`, old indexes, write buffers and negated phrases still use.
  - Inside SiftScan the recheck is part of `verify`, so ranked phrase queries only recheck candidates that would enter the top-k.
  - In the AM bitmap path it uses `recheck = true`, and the operator function evaluates it.
- **Cost model** **[est]:**
  - Recheck ≈ heap fetch (0.3 µs hot, 100 µs cold) + tokenization (1–2 ns per byte on the ASCII fast path, ≈ 5 ns per byte with Unicode folding).
  - Top-10 ranked: about k·ln(N/k) ≈ 150 verifications → ≈ 0.5–2 ms.
  - `count(*)` with a phrase: C candidates × (1–10 µs). For C = 100 k that is 0.1–1 s. This is the weak spot.
- **Mitigation: the phrase pair index (reloption `phrase_index = pairs`, adjacent token pairs as extra terms) was removed in 1.3.0.** Word positions (below) decide phrases from the index; on the 218k articles the pairs bought `count(*)` 1.6x over them for a 2.7x larger index.
- **Positions (reloption `positions`, default on, REINDEX to change; on-disk format 1.1).**
  - `CREATE INDEX` stores a position stream after each term's postings (layout in docs/CODEC.md 3.1): the ordinals of the term in each document, delta coded, with a checkpoint every 32 postings, so the positions of one posting are read without decoding the others. Terms with at most two postings keep their records in the dictionary entry. Queries without a phrase never read the stream.
  - A phrase on a path of ANDs from the root (not under OR or NOT) is *required*: the relaxed AND of its terms still finds the candidates, and every candidate is checked against the positions (rarest term first, narrowing the possible start positions term by term) before the heap is touched. Counts, bitmaps, unordered and ranked scans do this; a candidate refuted by the positions costs no heap page and no tokenizing. A phrase under OR is *optional*: a cursor of its own yields the documents of the AND that hold it, so the OR sees only real matches.
  - What the positions do not decide stays on the heap recheck: documents of write buffers and of the segments promoted and merged from them (no positions are written for them), segments of indexes built by earlier versions or with `positions = off`, and phrases under NOT (the index only knows a superset). A page match carries `recheck` only when it holds such candidates, and a `sure` mask with the candidates that need none (below), so `count(*)` over a fully positioned index fetches no heap tuple.
  - **Sure tree.** For unranked scans with a phrase that is not required (an OR branch, a NOT) the plan also binds the relaxation read from below: a phrase the bitmaps cannot decide matches nothing, a NOT excludes the superset of what it names. Its matches are a subset of the real matches, so a page is rechecked only for the candidates outside it (for example `dog OR "quick fox"` rechecks only the documents without `dog`). The matches of a segment whose positions decide every phrase are sure as well, unless a negated phrase is left to the heap. Direct group counting (`count(*)` summing popcounts of the all-visible pages of a group) applies whenever no candidate needs the heap and no required phrase needs its positions.
  - Format 1.1: the metapage minor version goes 0 to 1. This build reads 1.0 (segments without positions), 1.1 and 1.2 (segment format 2); a newer minor or another major is refused with a REINDEX hint (`sift_store.c`, `SIFT_FORMAT_MINOR` is 2). Earlier releases ignore the minor version, so a 1.1 index must be reindexed before an older release opens it.
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
- **Expansion.** Expanded terms are the union across segments. More than `sift.max_expansions` (default 512, a superuser setting) never truncates and never fails: the leaf reads as a *wide leaf* (below), whatever its kind. A regex cannot have back-references (the one feature that makes a match exponential). Only terms with a live posting count: a term whose docs were all deleted stays in the dictionary until a merge rewrites the segment, but does not use up the limit. Liveness is checked lazily, only when the limit is reached (dirty segments are re-expanded keeping live terms), so queries under the limit pay nothing.
  - **Short prefixes (`prefix_index = n`, default 0, REINDEX to change).** Expanding `s*` on a multi-field directory reaches thousands of terms (an error above `sift.max_expansions`, and slow below it). The reloption also indexes the first 1..n characters of every word as the term `\x1E prefix`, with tf = the occurrences of words that start with it (what an edge n-gram analyzer indexes, as ParadeDB's `edge_ngram(1, 15)` does). A prefix leaf of at most n characters is then one term lookup: no expansion, exact `df`/idf, bounds like any term. Longer prefixes still expand, over real words only: the control byte sorts the prefix terms (like the pair terms) before every word, and the matcher rejects them. Document evaluation (`sift.match`, rechecks) ignores the option, so results are the same with or without it. Measured on GeoNames places (`bench/bench run --dataset places`): see bench/README.md for index size, build time and latency against pg_search edge n-grams; a one-letter prefix is one term lookup where `max_expansions` errored.
  - **Wide leaves.** A prefix, wildcard, regex or fuzzy leaf with more terms than `sift.max_expansions` (in a segment or over all of them) is never an error or a truncation. In every segment it reads as the term `prefix_index >= len` would have indexed for a prefix (`sift_prefix.c`), and as the like term for the other kinds: the documents with a word the leaf matches, tf the occurrences of those words, df the documents, weighed like any term. The match set is exactly the union of the expansion (counts, filters and NOT equal document evaluation).
    - Mechanism: one dictionary range scan decodes the postings of each matching term once and collects them per superblock (4 bytes each); a superblock is summed in a dense tf array and emitted in key order into the posting writer, whose in-memory blob (with bounds, dl read from the docmap) is opened by the same cursor as a stored term, so the iterator, the count and the ranker (bounds, filters, parallel scans) need no special case. Count and unordered scans skip tf and bounds. Write buffers match through the prefix matcher and score the term from their records (tf summed over its words).
    - Cost: the postings of the expansion per segment (10-15 ms for a one-letter prefix of a million names), then a memcpy. A wildcard, regex or fuzzy leaf also scans the dictionary range its literal prefix leaves, with the matcher on every term of it (`*ing`, `/.*tion/`: the whole dictionary): 100,000 articles of Simple English Wikipedia, 456,027 terms, 72,478 documents with a word ending in `ing`: 90-170 ms (42 ms with the blob cached), `/.*tion/` 52,929 documents 320 ms, `*ation` 126 ms, where they used to fail above 512 terms; a leaf under the limit is unchanged: blobs are cached per backend (`sift.prefix_cache_size`, LRU, 8 MB) under the segment's relation file, generation and header block, which never name other postings (a segment is immutable, liveness is applied by the readers, and the union keeps deleted documents like dictionary df does). Memory while building: a pass gathers about 4 bytes per posting up to 128 MB (`sift_prefix_gather_kb`), then counts the postings of every superblock and builds the superblocks in windows that fit that budget, one scan of the expansion per window; a superblock that alone exceeds the budget sums the tf of its postings as they come (128 kB), so the memory of a build is its budget whatever the expansion, besides the blob it makes (about the size of the term's own postings). A scan of the dictionary and of the terms' postings serves interrupts and `statement_timeout` at every dictionary block and posting group.
    - Scoring: with at most `sift.max_expansions` terms a prefix still scores as the max over them, each with its own idf (above); above it, as the one term, with the score `prefix_index` gives (equal bits, `test/sql/prefix_wide.sql`; the SQL reference models both). The limit is thus the number of terms scored individually. `prefix_index` only makes the lookup cheaper (no postings to read, no cache) for a larger index.
    - Estimates: the planner's expansion stops at the limit for wildcard, regex and fuzzy leaves (their estimate counts the first terms), but a prefix reads its dictionary range on, df only, so its selectivity and cost are those of all its terms (about 1 ms of planning for a one-letter prefix of a million names, none for a prefix `prefix_index` covers). 
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
| Maintenance steps (promotion, merge) | `LockPage(index, 0, RowExclusiveLock)`, which steps share, and a claim on each segment the step replaces: a heavyweight lock (`LOCKTAG_OBJECT`, database, index, generation) taken with `LockAcquire(dontWait)` and released when the step ends. The worker (conditionally), the inserters of a full directory and of a backlog of sealed segments (waiting only for the claim of the last segment left to merge) run steps on different segments side by side. A claimed segment can only be changed by its holder, so a step re-reads the directory after claiming. The worker lock `LockPage(index, 3, ExclusiveLock)` keeps one worker of the index at work. |
| `amvacuumcleanup`'s truncation of the end of the file | `ConditionalLockRelation(AccessExclusiveLock)` on the index inside the maintenance lock, never waiting, released as soon as the file is cut. Every scan, insert and step of the worker holds a lock on the index, so none is at work, none holds a directory copy, and the cached lane tails of the appenders are invalidated (a relcache message sent before the lock is released) because a block past the new end cannot be read. A standby replays the lock first and cancels what holds the index. |
| `ambulkdelete` liveness pass, `amvacuumcleanup` (promote, merge to a fixed point, recycle, orphan sweep), `sift.promote()`, `sift.merge()` | `LockPage(index, 0, ExclusiveLock)`, the same pattern as GIN pending-list cleanup: no step is then at work. `sift.verify()` holds `ShareLock` for the same reason. The directory switch takes buffer locks briefly. |

**Metapage contention, measured, not changed.** `scripts/wait-profile.sh` samples the waits of concurrent bulk writers (8 sessions of `INSERT .. SELECT`, 20000 documents of 150 words each, load average above 20 on 16 cores, so indicative). The metapage buffer lock is not what they wait for: `LWLock:BufferContent` (all buffers, the metapage among them) took about 8% of the samples, while the maintenance lock (page 0) was waited for in 420 of 514 page-lock samples, held almost continuously by a sealing writer's promotion or merge (one session: 28 held samples, no waiter), and the throughput of 8 sessions equals that of 1 (about 115k documents/s). A free list pinned by a long transaction (a repeatable-read snapshot while 3000 promotions release about 400 MB) costs the allocation a scan of it under the metapage lock: 16 single-row insert clients 62k against 68k transactions/s, p99 1.6 against 1.3 ms once it is released. A per-backend page reservation would remove that scan from the page allocation of the write buffer, but the gain is below the noise of the host, and it would add pages that a crash leaves for the orphan sweep. The stall was the maintenance lock, which one merge held against every inserter that needed room (see Locks above for the split).

### Build

- **Parallel build:** `amcanbuildparallel = true`. Own `ParallelContext` with participants = `plan_create_index_workers` + leader; `build_segments` asks for an exact layout and builds serially.
  - Every participant runs the serial pipeline on its share of a `table_parallelscan` (chunks of ascending blocks, interleaved with the others'): heap scan, analyze, `SiftInvBuf`, a sorted run per full buffer. `synchronize_seqscans` is off for the scan, because a wrap-around would break the ascending keys each buffer needs.
  - Worker runs are named files of a `SharedFileSet`; the leader opens them after the workers finish and merges them with its own into the segments a serial build writes (`sift_runs_write` already merges runs with interleaved key ranges). The result is the same index as a serial build: one segment, per-block bounds, no extra size.
  - Runs are merged in passes, so the memory budget never limits the size of an index. Each participant (the leader too, and a serial build) merges the last *fan-in* runs of one level into one run of the next as it spills, like an external sort: the runs it keeps stay a few per level, the files and read buffers fit the budget, and a posting is merged a number of times logarithmic in the runs. The fan-in is the readers that fit half of the budget, each a file buffer and a read buffer of 8 kB to 64 kB, shrunk as the runs grow in number (at least 8 runs, at most 256). A worker merges down to one fan-in of runs before it hands them over; `sift_runs_write` does the same for more runs than readers fit (also for promotion and merges). A pass reads and writes the run format, it takes no term's postings in memory. Only the segment writer does: a term is encoded whole, about 10 bytes a posting, so a segment takes runs whose largest terms add up to a tenth of the budget at most. The cascade keeps to that limit: a buffer is full when a term has half of it, and the runs of a level wait until they are a fan-in long (a table of fewer runs merges none, as before) and then merge in groups of as many in a row as fit the limit, a run that fits with none staying as it is, so a run never holds a term larger than a segment takes and a hot term (in nine of ten rows) does not make the memory grow with the table: the runs that stay with the table size are the segments (each at least half of the limit), about 8 kB of file buffer each, not levels of merges. The same rule keeps a segment under the 4 GB of a term's postings and of a dictionary (uint32 lengths and offsets of the format): a run records the bytes its largest term and its dictionary take at most (8 bytes a posting and its position records; 32 bytes an entry besides the term, front coding ignored), and runs merge, and make a segment, only while those add up under the limits, so a term or a dictionary that would not fit makes more segments, not a failed build (merges and promotion still write one). The directory holds 128 segments, so when the budget would need more the build writes fewer, larger segments, which take more than the budget in their largest terms: a build never fails for lack of memory, it is slower or larger than asked, and its memory grows with the table only past what 128 segments of a tenth of the budget hold. The temporary files hold the postings once as runs and again for each merge pass that is not the last (about 2.3 times the index at the default budget, 4.3 times at 1MB, 100,000 articles); dropping the document-length column from runs needs a per-document map that grows with the table, and compressing them costs CPU on every build, so neither is done.
  - Rejected: one segment per participant. Terms under `grouped_min_df` (8192 postings; 64K when this was measured, before 1.2.0) are LIST-coded and only have the per-term bound, so splitting a term of 75K postings over 3 segments drops its block bounds: the ranked scan of `good` scored 1217 blocks instead of 28, and the index grew 4 to 8 % (M) or more (S). The leader would also have copied segments page by page.
  - Rejected: concurrent bulk writers. `smgr_bulk` assumes it alone extends the fork; parallel `smgrextend` beyond EOF is unsafe, so only the leader writes the index.
  - The leader's merge (encode, dictionary, write) stays serial: about a third of the serial build time, which bounds the speedup. Cost: one extra write and read of the runs (≈ 1.3 bytes per posting).
- **Large documents.** The text of the documents of a heap page is copied until the page is done, to analyze them in tid order (a HOT chain reports the tid of its root). The copy stops at a quarter of the budget (1 MB to 256 MB): what is buffered is analyzed, and a document larger than that is analyzed from its detoasted value at once, so a page of large out-of-line documents cannot pass the 1 GB of a string. A document with a lower tid than the buffer's last (after such a flush) starts a run. A document is added to the buffer only if its terms cannot exhaust the pool (a term takes 8 bytes, or a slice of up to 2 kB, and a position at most 5 bytes: a conservative bound from the term count and the text length); the buffer is spilled first when they could, so the 64 kB blocks of its 32-bit addresses (60,000) are never exhausted in the middle of a document, whatever `maintenance_work_mem`.
- **Progress.** `pg_stat_progress_create_index` shows the phases `scanning table`, `spilling runs` (a buffer spilled to a run, with the merges of the cascade), `merging runs` and `writing segments`, and the documents: `tuples_total` is the planner's estimate until the scan is done, then the documents indexed; `tuples_done` counts the documents analyzed (every 1024 of each participant, the workers' through the leader), then the documents of the segments written. A parallel build launching fewer workers than requested, or none, says so at `DEBUG1`.
- **Build throughput target** **[est]:** analysis at 3–5 ns per byte per core. 85 GB on 8 cores is ≈ 60 s of tokenizing, plus encoding and I/O. The target is a full build of 85 GB in under 4 minutes on 8 cores.
- **CREATE INDEX CONCURRENTLY** works through the standard protocol: `ambulkdelete` reports every indexed TID, then `aminsert` fills in.
- **REINDEX** is ambuild into a new relfilenode. `ambuildempty` writes the metapage and an empty directory for the init fork (unlogged tables).

### VACUUM

- **`ambulkdelete`** iterates each segment's docmap. For each present document whose liveness bit is set, it calls `callback(tid)`; for dead ones it clears the bit and marks the SB dirty. Write buffer records get their deleted flag set.
  - This is O(indexed docs) callback calls, ≈ 20–40 ns each with PG17+ TidStore: 3–6 s per 150 M documents **[est]**. Fine for background work.
  - Generic WAL batches up to 4 changed pages per record.
- **`amvacuumcleanup`:**
  1. promote sealed segments;
  2. merge steps until the policy asks for none and the directory holds at most `vacuum_segments` entries (reloption, default 2, 1 to 128, `ShareUpdateExclusiveLock`; not part of the analyzer hash, so no REINDEX). The tiered policy alone stops at 32 or fewer; for the rest `pick_merge` caps a merge at `excess + 1` segments (`excess` = entries beyond the target) and, when the policy has nothing, takes the smallest immutable segments (each within the fan-in of the sum of the smaller ones, as the policy requires). Only goal `MERGE_VACUUM` does this: the background worker and the inserter's merge for room keep the tiered policy, so `vacuum_segments` costs the write path nothing. A target above the segment count is a no-op. `pick_smallest` applies the size-ratio guard from the second segment on (each at most `SIFT_MERGE_FANIN` times the sum of the smaller ones): a tiny segment next to a huge one stays and the index keeps more than the target (a merge of the two rewrote the whole index at every VACUUM: 0.8 to 0.97 s and 11 MB of WAL for 11 MB plus 0.1 MB). With no entries over the target `pick_merge` asks the shared policy for the mostly-dead rewrite only (`sift_merge_pick_dead`), so the tier candidates the target discards no longer hide it;
  3. recycle free extents whose `safexid` is past the horizon;
  4. orphan sweep;
  5. truncate the free blocks at the end of the file (reloption `vacuum_truncate`, default on): only blocks that are reusable now (the same horizon as allocation), when they are 1000 blocks or a 16th of the file, and only if `AccessExclusiveLock` on the index is free (a conditional lock, as for a heap: it is not waited for, the next VACUUM tries again). The free list pages themselves move below the cut if they are in the way. Free space in front of live segments is not moved; merges reuse it, and a merge that lands in it is what leaves the end free;
  6. report `num_pages` and `num_index_tuples`.
- **Orphan sweep:** mark every block referenced from directory entries (header extents, LIVE pages, lane chains) and the free list. Any other block below the high-water mark goes back to the free list. This is cheap: a few extents per segment.

### Versioning

- **Metapage version.** The format is major 1, minor 2 (`SIFT_FORMAT_MAJOR`, `SIFT_FORMAT_MINOR`; the column engine numbers its segment formats on its own, docs/COL.md section 2). A different major, or a minor newer than this build knows, raises an ERROR asking for REINDEX on open; older minors are read as they are and upgraded lazily by merge, and each segment entry carries `format`.
- **SQL upgrades** use `pg_sift--1.0--1.1.sql` scripts; `pg_sift--3.0.sql` is the install script of the current version, `pg_sift--1.1--1.2.sql` adds `sift.index_info`, `sift.version` and the `extents`, `dead_docs` columns of `sift.segments`, and `pg_sift--1.2--3.0.sql` adds the `sift_col` index access method (`sift.col_*`) and the `sift_table` table access method; a fresh 3.0 install and 1.2 plus that update are the same objects (`test/t/extension_lifecycle.sh`). `SIFT_MODULE_VERSION` (`src/core/sift_ops.h`) is the version the library reports and must equal the control file's `default_version`.
- **pg_upgrade** copies the relfilenode files unchanged, so the on-disk format must stay stable across PostgreSQL minor releases. Any PostgreSQL-dependent layout, such as the VM constants, is tested.

---

## (j) SIMD dispatch

- **Build flags.** There are no per-file CFLAGS. The Makefile appends `-march=x86-64 -mtune=generic -O2 -g -ffp-contract=off` (overriding pg_config's `-march=native`), so the `.so` runs on any x86-64.
- **No POPCNT outside the SIMD objects.** GCC lowers `__builtin_popcountll` at baseline to a libgcc call, and a distribution's libgcc may be built with POPCNT. Baseline code therefore uses `sift_pc64` (`sift_popcount.h`, SWAR); the AVX2/AVX-512 objects keep the builtin through `SIFT_PC64`. `scripts/isa_check.sh` (in `ci.sh`) fails if a baseline object has a `popcnt` instruction or `__popcount*` reference, or `pg_sift.so` links libgcc's helper.
- **Kernels** are compiled with function attributes:
  - `SIFT_TARGET_AVX2` = `target("avx2,bmi,bmi2,popcnt")`;
  - `SIFT_TARGET_AVX512` = `target("avx512f,avx512bw,avx512vl,avx512dq,avx512vpopcntdq,avx512vbmi2,bmi,bmi2,popcnt")`.
- **`cpu` module** (`sift_cpu.c`):
  - Uses `__builtin_cpu_supports`, which checks OS XSAVE state for AVX.
  - Marks BMI2 PDEP/PEXT as *slow* on AMD family 0x17 (Zen1/Zen2, microcoded at about 250 cycles). Those CPUs use the AVX2 kernels without pdep.
  - The `sift.simd = auto|scalar|avx2|avx512` GUC caps the level, for testing and benchmarking.
  - Modules register a rebind callback, so the codec can share the same dispatch.
- **Dispatch** goes through a `const SiftKernels *` table chosen once per level change, never per call through `cpuid`.
- **Kernel catalogue** (`sift_kernels.h`):
  - rank expand (pdep, or a scalar loop), with the mask to apply and the popcount of the result;
  - tf chunk decode (nibbles → u8; VBMI2 `vpexpandb` places them in offset lanes);
  - `K(dl)` gather;
  - BM25 accumulate (`vdivps`, exact IEEE, no FMA);
  - block-UB accumulate (u8→f32 widen + multiply-add as separate ops);
  - threshold compress (`vcompressps`/`vcompressw`);
  - VM pext.
- **The codec owns the bitmap set kernels:** AND, OR, ANDNOT, count, iterate. It keeps its own dispatch (`sift_init` / `sift_set_isa`), which a rebind callback in `sift_handler.c` binds to `sift.simd`.
- **Correctness:** every kernel has a scalar reference. The `kernels` regression test runs randomized equivalence at every level the CPU supports, compared bit for bit.
- **AVX-512 frequency licensing.** Zen4/5 does not downclock. On Intel server parts, `auto` prefers the 256-bit AVX-512VL forms for scoring loops; 512-bit forms only where the data is ≥ 64 B per op.

---

## Extras beyond the baseline design: innovations, expected gains, measurement

Every item is measured by the benchmark harness (`bench/`); the profile of where a query spends its time and the ideas not built yet (a 1.3.0 baseline) are in [docs/perf/RESEARCH-2026-10.md](docs/perf/RESEARCH-2026-10.md). It compares sift, in-database baselines (pg_textsearch, ParadeDB, GIN, rum), and external engines (Elasticsearch/OpenSearch, Tantivy CLI, Meilisearch, Typesense) on the same corpus and hardware. Metrics: QPS, p50/p99, buffers touched per query, build time, index size, and WAL bytes per update. An item that does not deliver ≥ 10 % on its target metric is dropped.

| # | Idea | Status | Expected gain **[est]** | Measure |
|---|---|---|---|---|
| 1 | Exact BM25 from rank-space tf nibbles + u16 dl (a) | core | Exact scores at ≈ 2 bits per posting; no quality gap against Lucene | Score equality vs reference SQL BM25; index size |
| 2 | BMP best-first over (segment, 16-page block) with u8 bounds and verified θ | core | 10–50× over exhaustive disjunction top-10; beats BMW cursors on SIMD density | blocks scored / skipped (EXPLAIN), QPS |
| 3 | Fused SIMD pipeline: page-mask program → offset decode → liveness → tf → BM25 → compress, per block in L1 | core | Removes the intermediate result sets of tbm-based plans | ns per (doc, term) |
| 4 | Score-in-tuple via custom scan tlist (no global registry) | core | Removes a whole bug class; also allows MergeAppend top-k over partitions | Isolation and parallel tests |
| 5 | PG18 ReadStream/AIO prefetch of heap blocks *and* index chunks (order known in advance) | core | Cold-cache p99 cut 3–10× on NVMe | p99 with `pg_prewarm` off and `io_method=io_uring` |
| 6 | VM pext → 256-bit all-visible mask per SB; chunk-count shortcut on clean SBs | core | `count(*)` at memory bandwidth; single-term count O(chunks) | count QPS against pg_textsearch and ParadeDB (Wikipedia) |
| 7 | Write buffer lanes + bloom-gated write buffer scan + async dynamic worker without preload | core | No insert hot spot; bounded query overhead under 1,000 updates/s | QPS under the 1,000 UPDATE/s mix, WAL per update |
| 8 | Copy-only merges for disjoint SBs (ownership transfer of encoded chunks) | core | Merge write amplification ≈ 1× for disjoint ranges | Merge MB/s, WAL bytes |
| 9 | Parallel-aware SiftScan: DSM morsels of 16 SBs, **shared atomic θ** across workers for ranked; partial counts + Finalize for count | implemented in 1.1, opt-in: `sift.parallel_scan = on` (default off). Results equal the serial plans (ties included). Measured (`bench/steps/parallel.sh`, 218,058 articles, 1/2/4/8 workers, host load 21–25): wins only for an unordered scan with a heavy residual qual (1.8×); `count(*)` 0.06–0.29× and top-1000/10000 0.5–1.2×, because a Gather costs 2.4 ms (1 worker) to 6–7 ms (8) before any work and the serial plans take 0.35–0.67 ms and 4–20 ms. Likely to pay only on much larger corpora. Historical figures, measured on 2026-10-16 during 1.1 development (commit 800f7b9) and not re-run since | Near-linear scaling for count and large top-k; shared θ avoids per-worker over-scoring | Speedup at 1/2/4/8 workers |
| 10 | Filter-mask pushdown: SiftScan accepts child BitmapIndex paths (btree, BRIN) as `custom_paths` and converts the TIDBitmap into SB page/offset masks before scoring | implemented in 1.1 (kept; gate met): 0.1% filter, ranked `or`, mean latency 23.6 -> 0.22 ms (clustered) and 15.0 -> 0.86 ms (scattered) on 218k articles, `and` 4.9 -> 0.22 / 4.3 -> 0.64 ms; 25k articles: `or` 2.2 -> 0.21 ms, `and` 0.7 -> 0.19 ms. At 10% scattered the pushdown is slower than filtering after the ranker, so the planner costs both. Historical figures, measured on 2026-10-16 during 1.1 development (commit d7a9a70), host and CPUs not recorded in the commit, not re-run since | Exact filtered top-k without post-filter over-fetch; 5–100× on selective filters | `WHERE body @@> q AND tenant = $1 ORDER BY score LIMIT 10` |
| 11 | Facet counts: reloption `analyzer = keyword` makes a sift index of exact values; `sift.facets(index, facet_index, query, lim)` = popcount(query ∧ value) per value, a function and not a GROUP BY rewrite ((l)) | implemented in 1.1: on 25,000 Wikipedia articles with 26 values, 1.9-4.5x sift GROUP BY and 1.9-15.9x GIN GROUP BY on broad queries (1.5-2.6x Elasticsearch terms), 0.6x on a selective AND; with 2,500 values 0.5-0.9x sift GROUP BY (cost is linear in the values), see (l) | ES terms-aggregation-class facets inside PG | GROUP BY facet QPS vs ES |
| 12 | Phrase pair index (g), removed in 1.3.0 for word positions | core | Phrase `count(*)` 10–100× | Phrase count QPS |
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
- **Leak gate.** `scripts/leakcheck.sh` is the one place that decides whether a run leaked, shared by every suite through `pgtest.sh` (config, sanitizer environment, start and stop), `test/t/lib.bash` and `SiftTest.pm` (nodes), `tap.sh`, `soak.sh` and the last stage of `ci.sh`. It fails on the leak diagnostics PostgreSQL logs at transaction and backend end, on assertion failures, crashes and sanitizer reports, on the sift memory contexts that outlive a backend (`SIFT_TESTING`, `src/core/sift_leak_test.c`), on the state a cluster leaves when it stops, and on sustained memory growth of the soak backends. `leakcheck.sh selftest` and `test/t/leakcheck_gate.sh` (with `sift._test_leak`) show that it fails on each kind of leak and passes a clean log.
- **Fuzzing.**
  - The query parser via the SQL wrapper `sift.parse_query`, driven by a grammar-aware generator script, under ASan.
  - The codec standalone with libFuzzer (`test/fuzz`); the codec has no backend dependencies.
- **Determinism.** Fixed float evaluation order, ties broken by ctid, and `sift.simd` levels compared bit for bit.
- **Docs.** `README.md` (overview, install, quick start, benchmark summary), `docs/USAGE.md` (user guide: DDL, SiftQL, functions, GUCs, limits), `DESIGN.md` (this file), `docs/CODEC.md` (the codec).
- **Tests.** The tiers (`make check`, `make check-full`), the throwaway cluster of `scripts/pgtest.sh` and how to run one suite are in CONTRIBUTING.md; `SIFT_QUICK=1` selects the quick sizes in `test/t`. The quick differential test runs 8 of the 16 harness mutants (the ones the full run kills with the most queries).
- **Packaging.** `packaging/PKGBUILD` (`pg_sift`) builds the committed HEAD with `nice make -j4` and installs through the standard PGXS `DESTDIR` path.

---

## (k) Limits and non-goals

- **Platform limits.** BLCKSZ 8192 only; heap and `sift_table` tables only; server encoding UTF8 or SQL_ASCII (in SQL_ASCII, bytes ≥ 0x80 are opaque letters).
- **Index shape.** One key column of type text or varchar, or an expression yielding text. Partial indexes are supported.
- **Dead documents in statistics.** Rows of rolled-back transactions and deleted rows count in N, df and avgdl until VACUUM marks them dead; then a format 2 segment leaves them out (N and avgdl from the header's vacuumed count and length sum, a term's df from its dead-posting count), so scores equal the merged index's exactly (`live_stats` test, `bench/sql/dead_docs.sql`; [docs/SIZING.md](docs/SIZING.md)). Segments of format 1 keep counting them until merged. The write buffer counts its live documents. A non-HOT update inserts the row's text again: documents are found by TID and the new version has its own, so `indexUnchanged` cannot let `aminsert` skip it.
- **Index size.** The orphan sweep and `sift.verify` hold block numbers in `Bitmapset`s (2^31 members): indexes above 16 TB are unsupported.
- **Size caps.**
  - Terms are ≤ 255 bytes; longer ones are truncated at a character boundary.
  - dl saturates at 65535 and tf at 255 (score effect < 1.5 %).
  - ≤ 128 segments, ≤ 256 query leaves, wildcard, regex, fuzzy and prefix leaves of any number of terms (a wide leaf builds a blob of the size of its postings in memory).
- **Scoring.** k1, b and analyzer options are fixed per index; changing them requires REINDEX (`ALTER INDEX ... SET/RESET` of one that changes the analyzer warns, through a `ProcessUtility` hook that compares the metapage's analyzer hash with the index's options; until the REINDEX queries and inserts fail). The index only grows to a high-water mark, and REINDEX shrinks it.
- **Non-goals of the `sift` index** (checked against 3.1.0):
  - proximity / slop queries (word positions, format 1.1, decide phrases, §g, but there is no `NEAR` or slop operator);
  - `amgettuple` plain and index-only scans (bitmap scans and the SiftScan / SiftCount custom scans only);
  - backward scan, mark/restore;
  - multi-column `sift` indexes (one key column; the `sift_col` index takes several columns);
  - a custom rmgr for the index (generic WAL; only the logical change record of a published `sift_table` uses one, §d);
  - statement-level insert batching;
  - non-x86 SIMD (scalar works everywhere; NEON later);
  - parallel scan by default (opt-in since 1.1 through `sift.parallel_scan`, item 9).

---

## (l) Keyword facets

`SELECT category, count(*) FROM docs WHERE body @@> q GROUP BY category ORDER BY 2 DESC LIMIT 10` as bitmap arithmetic.

- **Keyword index (`analyzer = keyword`, REINDEX to change).** The whole value is one term, so each distinct value is one dictionary entry whose posting is the bitmap of its rows. Folding follows `case_folding` / `accent_folding` (both off: bytes as stored). An empty value, or one that analyses to nothing, and NULL have no term. A value over 255 bytes (folded) is its first bytes (cut at a character boundary), U+FFFF and the 16 hex digits of the FNV-1a 64-bit hash of all of it, the same term from an index build, an insert and a query: two long values that share their start stay two values (an error would fail an `INSERT` or the end of a long `CREATE INDEX`, a plain truncation would merge them, and 1.3.1 and before raised `ERRCODE_PROGRAM_LIMIT_EXCEEDED`). The hash is not a cryptographic one, and `sift.facets` returns the term, so such a value shows as its start and the hash; a wildcard or prefix query matches it by the first 236 bytes only. It cannot be combined with `stemmer`, `prefix_index` or `fields`. A query word on a keyword index is one value (`"gamma delta"` for one containing a space), so `@@>` on it is an exact-value filter; the analyzer is part of the config hash, and non-keyword hashes are unchanged.
- **API: `sift.facets(index, facet_index, query, lim = 10, nulls = false)` returns `(value, count)`**, count descending then value bytewise; `lim` NULL returns every value; `nulls` adds a row with a NULL value for the matches without one. `index` is the search index the query runs on and `facet_index` the keyword index of the facet column, both on the same table. A multi-column facet is one keyword index per column.
- **Function, not an upper path.** A custom scan that replaces `Agg` over the scan would have to prove, in `create_upper_paths_hook`, that the grouping key is exactly the column of a keyword index, that the only qual is one `@@>` on another index, that the aggregate is a plain `count(*)` (no FILTER, DISTINCT, HAVING, joins, partial aggregation under parallel plans), and then absorb the Sort and Limit above it to get the early exit. Any shape it misses falls back to the ordinary plan silently, and any shape it wrongly accepts returns a wrong count. The function states the contract (two indexes, one query), is the same code path whatever the planner would pick, and costs one call. The price is that `GROUP BY` queries are not accelerated automatically.
- **Algorithm (`sift_facet.c`).**
  - The query's matches are collected once with the `count(*)` rules (visibility map masks for all-visible pages, heap checks for the others, phrase recheck) into superblock groups with a 64-byte offset row per page; the heap is read only where `count(*)` reads it.
  - Every value of the keyword index is a term; its count is the popcount of the AND of its posting groups with the matches (`sift_grp_and_count` over the codec cursors, `min` of the group widths). Dirty superblocks of a segment (VACUUM cleared bits) are ANDed with the liveness of the segment, as in a query; the other segments' postings are used as stored. Inline postings (df <= 2) are probed against the matches; the others leapfrog their groups against the matches.
  - Every dictionary is scanned once to list the values (the cost is linear in the number of distinct values, not in the matches); write buffers are read once and counted per value; a value present in several segments is the sum of its counts.
  - **Top-k.** Values are tried in descending order of an upper bound (their df, plus the matches in the write buffers); once the k-th count is at least the next bound the rest cannot enter and the scan stops. A value whose bound only ties the k-th count is still counted when it sorts before the k-th value. `nulls` needs every value counted (the NULL row is the matches minus the counted ones), so it disables the early exit.
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

## Source layout

`src/` has one folder per area. The file names are unchanged and unique across the folders, so a source includes another by its bare name
(`#include "sift_page.h"`): every build puts all the folders on the include path (`src/areas.mk`: `SIFT_DIRS`, `SIFT_INC`; `static.sh` and the
SQLite and MariaDB builds do the same). The folder of a file follows from its name, by the table at the top of `scripts/restructure-src.sh`,
whose `--check` fails for a file no rule places and for a path that went stale. `src/portable.list` and `src/portable-col.list` name files with
their folder (`bm25/sift_bitmap.c`); the SQLite and MariaDB builds and `scripts/check-portable.sh` read them.

| Folder | Holds | Includes from |
|---|---|---|
| `core/` | what every area shares: the module shell (`sift_handler.c`: `_PG_init`, GUCs, reloptions, catalog cache, SQL entry points), the page and extent store (`sift_page.h`, `sift_store.*`, `sift_stream.c`), the lane chains of a write buffer (`sift_lane.*`), liveness pages, visibility, the context of a parallel build, the checked reader and varints, CPU dispatch, `sift_host.h` | the other areas only in `sift_handler.c`, which registers them |
| `bm25/` | the BM25 index access method `sift` and its engine: analyzer, SiftQL, bitmap codec, scoring kernels, term blobs, dictionary, segments, sorted runs, merge policy, its write buffer (`sift_wbuf.*`), maintenance, build, evaluator, planner and the SiftScan and SiftCount nodes, facets | `core/` |
| `col/` | the column engine and the `sift_col` access method: chunk codec, column kernels, shredder, unit encoding, segments, build, its write buffer (`sift_col_wbuf*`), maintenance, column source, verify | `core/`; `bm25/` (maintenance, merge policy and kernel infrastructure it reuses) |
| `agg/` | SiftAgg, the aggregate pushdown: planner, spec, executor, aggregation kernels, the column source seam (`sift_colsrc.h`) with its providers, a BM25 match set as the row selection, Top-N, the `sift_colstub` access method | `core/`, `col/`, `bm25/` |
| `table/` | the `sift_table` table access method (docs/COL-TAM.md) | `core/`, `col/`, `agg/` (its column source), `bm25/` (maintenance) |

A file goes to the area that owns it. What two areas share goes to `core/` when it needs nothing of either; otherwise it stays with its owner
and the other area includes it (`col/` uses the maintenance and merge policy of `bm25/`) instead of keeping a copy. The portable core
(`src/portable.list`) is mostly `bm25/`, with the shared pieces of `core/`; the column track's list (`src/portable-col.list`) is `col/` and
the aggregation kernels of `agg/`.

---

## MODULE MAP

Prefixes: every exported symbol is `sift_<module>_*` unless it is an AM or SQL entry point. Headers live next to their sources in `src/<area>/` (Source layout above).

| Module | Files | Scope | Depends on |
|---|---|---|---|
| core | `sift.h`, `sift_handler.c` | Constants, options, GUCs, catalog OID cache, `_PG_init`, AM handler. | — |
| cpu | `sift_cpu.h/.c` | Feature detection, `sift.simd` GUC, dispatch rebind registry. | core |
| codec | `sift_bitmap.h`, `sift_bitmap*.c`, `docs/CODEC.md` | Term blob encoding (LIST/GROUPED), group cursors and AND/OR/ANDNOT cursor trees, set and count kernels, its own ISA dispatch. | — |
| kernels | `sift_kernels.h/.c` | Scoring SIMD kernels + scalar references | cpu |
| analyze | `sift_analyze.h/.c` | Tokenizer, folding, stemming, term hashing | core |
| query | `sift_query.h/.c` | SiftQL parse, analyze, deparse, document evaluation | analyze |
| store | `sift_page.h`, `sift_store.h/.c` | Page formats, metapage and directory IO, extent alloc and free, byte streams, WAL helpers | core |
| dict | `sift_dict.h/.c` | Dictionary writer, reader, term matchers | store, query |
| posting | `sift_posting.h/.c` | Term blob writer and reader: chunks, tf stream, bounds, sparse and inline | store, codec, kernels |
| segment | `sift_segment.h/.c` | Segment writer and reader: docmap, liveness, sections; streaming merge input | posting, dict |
| wbuf | `sift_wbuf.h/.c` | `aminsert`, lanes, seal, write buffer reader, write buffer delete | store, analyze |
| build | `sift_build.h/.c` | `SiftInvBuf`, `ambuild` (serial and parallel), `ambuildempty` | segment, analyze |
| maint | `sift_maint.h/.c` | Promote, merge, recycle, orphan sweep, `ambulkdelete`, `amvacuumcleanup`, worker, `sift.promote` / `sift.merge` / `sift.segments` / `sift.verify` | segment, wbuf, build |
| visibility | `sift_visibility.h/.c` | VM masks, heap checks, fetch | core |
| eval | `sift_eval.h/.c` | Bind, iterate, rank, count, estimate | segment, wbuf, query, dict, kernels, visibility |
| amscan | `sift_amscan.c` (`sift_am.h`) | `ambeginscan`, `amrescan`, `amgetbitmap`, `amendscan` | eval |
| planner | `sift_planner.h`, `sift_planner.c`, `sift_cscan.c` | Hooks, paths, costs, `amcostestimate`, SiftScan / SiftCount exec, EXPLAIN | eval |
| sql | `sift_sql.h`, `sift_funcs.c` | `sift.match`, `match_idx`, support, `matchsel`, highlight, tokenize, `parse_query`, score stubs | query, analyze, planner |

### Cross-module contracts (binding)

1. **SQL entry points.** `PG_FUNCTION_INFO_V1` appears **only** in `sift_handler.c`. Implementers define `Datum fn(PG_FUNCTION_ARGS)`, declared in `sift_sql.h` (`PGDLLEXPORT`).
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
6. **Codec usage** (`sift_bitmap.h`):
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
   - Only `wbuf` writes lane pages.
   - Only `store` frees or recycles extents.

---

## Shared core and hosts

The codec, dictionary, segment writer and reader, scoring, SiftQL parser, analyzer and evaluator described above are host-independent C. The files named in
`src/portable.list` (the **portable core**) are compiled by every host: the PostgreSQL extension, the SQLite extension (`sqlite/`) and the MariaDB extension
(`mariadb/`). Everything else in this document (the index access method, WAL, MVCC, planner integration, VACUUM) belongs to the PostgreSQL host and does not
apply to the others; the SQLite host's own design is in [sqlite/DESIGN.md](sqlite/DESIGN.md), the MariaDB one in [mariadb/README.md](mariadb/README.md#layout-and-shared-core).

**What is shared.** Besides the codec and the evaluator: the segment format and its writer/reader (`sift_segment.c`), the tiered merge policy
(`sift_merge.c`: which segments merge, from the directory and the sizes), the sorted runs and their merge into a segment (`sift_runs.c`: the inverted buffer `SiftInvBuf`, `sift_run_from_segment`, `sift_runs_write`; promotion and merge of PostgreSQL, merge of SQLite), the deletion of one document (`sift_seg_delete_key`, which VACUUM's `sift_seg_bulkdelete`
shares with the SQLite DELETE: the dirty flag, then the liveness bits), the byte stream's extent bookkeeping and padding (`sift_store.h`, `sift_stream.c`), the
analyzer-options validation and hash (`sift_analyze.c`), the layout constants and error texts. **What a host owns**: where bytes live (page store), transactions and
crash safety, memory and error plumbing, the way a query reaches the evaluator (planner and custom scan, xBestIndex), the write path that feeds the writer
(heap build and write buffers, the in-memory buffer), background work, and option syntax (reloptions, `CREATE VIRTUAL TABLE` arguments).

**The host interface.** A portable file includes the C library, compiler intrinsics, the other files of the list and `src/core/sift_host.h`, nothing else
(`scripts/check-portable.sh`, a stage of `scripts/ci.sh` and the first step of `sqlite/test.sh`). `sift_host.h` is a contract, not an abstraction layer on the hot
path: its vocabulary is that of PostgreSQL's backend API (`palloc`, `MemoryContext`, `ereport` that does not return, `StringInfo`, `List`, `HTAB`, `pg_mb*`,
`pg_reg*`, atomics and spin locks for `sift_parallel.c`, `BufFile` for the builder's runs), plus `sift_host_enum_option` for the one process setting (`sift.simd`).
The page/extent store is the link-time interface declared in `sift_store.h` (`ReadBuffer`/`ReleaseBuffer`, `sift_wt_*`, `sift_sw_*`, `sift_sr_*`, `sift_dir_*`):
`sift_store.c` implements it over shared buffers and generic WAL, `sqlite/sqstore.c` over rows of a shadow table and a bounded cache. The PostgreSQL build includes the
real headers (identical calls, identical speed); the SQLite one provides the vocabulary in `sqlite/host/host.h` over `malloc`, `setjmp` and SQLite rows, and the MariaDB one
(`mariadb/host/host.h`, `SIFT_HOST_MARIADB`) uses the same plain-C vocabulary (the file `sqlite/host/host.c` is compiled into both) over segment files mapped into the address space. Optional
collaborators the core calls but a host may not have (write buffers, visibility, TID bitmap filters) are declared in portable headers and implemented by the
host; `sqlite/sqstubs.c` is the SQLite side of them.

**Adding a host.** (1) Provide a header with the vocabulary above and add its branch to `sift_host.h` (a `SIFT_HOST_<NAME>` define). (2) Implement `sift_store.h`
over the host's storage, and the optional collaborators it does not need as stubs that fail. (3) Build the files of `portable.list` (the Makefile reads it) and write the
host's front end: option parsing, the write path (feed `sift_segw_*` in key order: `sqlite/sqbuild.c` is the smallest example), and the merge driver
(`sift_merge_pick` chooses, the host rewrites). (4) Run the equivalence tests: scores must be bit-equal to PostgreSQL's evaluator on the same corpus
(`sqlite/bench/compare_pg.py`, `mariadb/tools/compare.py`). `mariadb/host/` is the example of a host whose store is a directory of mapped files, and of one that keeps its own
regular expression matcher (`host_regex.c`) to match PostgreSQL's advanced regular expressions where the C library's would not.

**Still two copies.** The SQLite in-memory write buffer (`SqInv` in `sqlite/sqbuild.c`, any key order, deletes) against `SiftInvBuf` (ascending keys) and the
highlight/snippet/facet code are not unified; why, and what it would take, is in [sqlite/DESIGN.md](sqlite/DESIGN.md#what-is-not-shared-yet). The heap build, parallel workers
and the leader (`src/bm25/sift_build.c`) stay with the PostgreSQL host.
