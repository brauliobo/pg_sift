# BM25 index size: where the bytes go, against ClickHouse, and the plan

Research for 3.1 on the size of the BM25 index (`CREATE INDEX .. USING sift`). Labels: **MEASURED** (reproducible with the commands in section 2),
**ESTIMATE** (reasoned or simulated from the text, not read from an index). All byte counts are exact file sizes; sift sizes are
`pg_relation_size` of the index (one segment, built serially, nothing deleted) and `sift._test_segment_parts` (a testing build), ClickHouse sizes are
`system.data_skipping_indices` (`data_compressed_bytes + marks_bytes`, which equals the sum of the `skp_idx_*` files of the active parts on disk).

## 1. Result in one page

- **MEASURED, baseline (origin/main, 3.0).** On 1,000,000 product titles the default BM25 index is 11.27 MiB and 7.88 MiB with `positions = off`;
  ClickHouse's text index (`text(tokenizer = 'splitByNonAlpha')`, the one `bench/sql/store/ch.sql` makes) is 5.74 MiB. On the 50,000 articles of the
  slim tier it is 31.13 MiB (12.95 MiB with `positions = off`) against 11.94 MiB (12.84 MiB without the lowercase preprocessor sift's analyzer applies).
- **The two do not hold the same information.** ClickHouse's default text index is a set of row ids per token: it answers "has token", "has all
  tokens", "has any token". It stores no term frequency, no document length and no score bound, so it cannot rank, and no positions, so a phrase is
  checked on the column. sift's index holds all of those. Where ClickHouse does store positions (`support_phrase_search = 1`, experimental in 26.8) its
  index is 40.8 MiB on the titles and 122.6 MiB on the articles: 3.6 and 3.9 times sift's 11.27 and 31.13 MiB for the same phrase answers.
  Only the tuned ClickHouse index with `posting_list_codec = 'bitpacking'` (3.15 MiB titles, 9.34 MiB articles) is smaller than sift's matching data.
- **Where sift's bytes go (titles, default, 11.80 MB):** the dl rows of the docmap 17.0%, the GROUPED containers of the 75 terms with 8,192 postings
  or more 39.5%, the position stream 30.0%, the LIST blobs 5.7%, the tf and bound sidecar 3.2%, liveness pages 1.9%, the rest under 3%.
  **Articles, default (32.63 MB):** positions 57.8%, LIST blobs 19.2%, dictionary 13.5% (a third of it the inline postings of the 255,791 terms
  with at most two postings), tf stream 4.7%, liveness 1.1%.
- **Nothing is wasted in the layout.** One segment built serially and the same data built as four segments and merged give the same bytes after the
  merge (11,796,480); padding between the codec blob and its sidecar is 86 bytes (titles) and 85,635 bytes (articles, 0.26%); the page headers and the
  unused end of the last page are 0.6% and 0.9%. A merge leaves the segments it replaced as free blocks inside the file until the next ones reuse them
  (section 3.4): the file, not the segment, is what `pg_relation_size` shows.
- **The largest avoidable bytes** are in the representation, not the layout (section 4): 2 bytes per document for the length of a title that is
  3 tokens long; 16 bits per posting in the array containers of frequent terms on pages with many small rows (the same postings take 7.6 bits as
  Elias-Fano lists, but those are 2 to 15 times slower to query); 5 bytes per inline posting and 3 to 5 bytes of header per dictionary entry; a
  position record is 1 byte for the 3 tokens of a title and a 2-byte varint for a position inside an article.

## 2. Data and method

| | titles | articles |
|---|---|---|
| rows | 1,000,000 `orders` of the online-store dataset (`bench/sql/store/gen.sql`, `title` column: two adjectives and a noun, 3.1 tokens) | 50,000 Simple English Wikipedia articles of the slim tier (`bench/sql/load.sql`, `LIMIT 50000` by `hashint8(src_id)`), 71 MB of text, 234 tokens on average |
| heap | 13,105 pages, 76 rows per page, whole `orders` row (9 columns) | 4,904 pages, 10 rows per page |
| terms / postings | 170 / 3,057,633 | 333,293 / 5,551,834 |
| sift | `CREATE INDEX .. USING sift (col)` and `WITH (positions = off)`, `max_parallel_maintenance_workers = 0` | the same |
| ClickHouse 26.8.2.7 | the `orders` table of `bench/sql/store/ch.sql` (partitioned by month, 36 parts), `OPTIMIZE FINAL` | `id UInt64, body String`, one part, `ORDER BY id`; case-folded variant `preprocessor = lower(body)` |

Commands (a throwaway cluster, `scripts/pgtest.sh`; a local `clickhouse-server` with its own data directory and ports):

```sh
scripts/bm25-size.sh --table orders --column title        # the part tables below (both builds), in the database that holds the table
scripts/bm25-size.sh --table docs --column body
# ClickHouse, per variant, then:
#   SELECT table, data_compressed_bytes + marks_bytes FROM system.data_skipping_indices WHERE name = 'title_tokens'
#   text(tokenizer = 'splitByNonAlpha')                                       the benchmark's index
#   text(tokenizer = 'splitByNonAlpha', posting_list_codec = 'bitpacking')    its smallest posting lists
#   text(tokenizer = 'splitByNonAlpha', support_phrase_search = 1)            positions (SETTINGS allow_experimental_text_index_phrase_search = 1)
```

ClickHouse's `pfor` codec is not in 26.8.2.7 (`Unknown posting list codec`); the benchmark of `docs/BENCHMARKS.md` ran 26.9.19.9.

## 3. Baseline (MEASURED)

### 3.1 Against ClickHouse

| index | titles (MiB) | articles (MiB) | holds |
|---|---:|---:|---|
| sift, default | 11.27 | 31.13 | postings, tf, dl, bounds, liveness, **positions** (phrases, BM25) |
| sift, `positions = off` | 7.88 | 12.95 | postings, tf, dl, bounds, liveness (BM25, boolean; phrases by heap recheck) |
| sift, matching data only (dictionary + codec blobs + the docmap's present docs) | 5.13 | 10.72 | what `hasAllTokens` needs |
| sift, every term LIST (`sift.grouped_min_df` very large) | 8.83 | n/a | slower, section 3.3 |
| ClickHouse `text`, default | 5.74 | 12.84 raw tokens, 11.94 lowercased | token to row ids |
| ClickHouse `text`, `posting_list_codec = 'bitpacking'` | 3.15 | 9.34 lowercased | the same, smaller |
| ClickHouse `text`, `support_phrase_search = 1` | 40.77 | 122.58 lowercased | the same plus positions |
| ClickHouse `text`, positions and bitpacking | 38.19 | 119.97 lowercased | |
| Elasticsearch (`docs/BENCHMARKS.md`, 50,000 articles, stored source included) | n/a | 71.7 | not re-measured here |

ClickHouse's dictionary is tiny on the titles (98,004 bytes for 36 parts) and its posting lists are 98% of the index; on the articles raw
tokens double the dictionary (case-sensitive) which is why the lowercased variant is the fair one against sift's analyzer.

### 3.2 sift parts

Bytes of each part, per posting (`B/post`) and per term (`B/term`); a posting is a (term, document) pair, inline ones included.
`bits/post` for the parts that scale with the number of documents instead (dl rows, present docs, liveness) is per posting too, so divide by
postings per document (3.06 on the titles, 111 on the articles) to read them per document: dl rows are 2.00 bytes per document in both.

**Titles, 1,000,000 documents, default (11,796,480 bytes):**

| part | bytes | % | bits/post |
|---|---:|---:|---:|
| docmap: dl rows | 2,000,000 | 17.0 | 5.23 |
| docmap: present docs | 211,688 | 1.8 | 0.55 |
| postings: GROUPED containers + directory | 4,702,950 | 39.9 | 12.31 |
| postings: LIST codec | 674,864 | 5.7 | 1.77 |
| postings: group scores + block bounds | 108,832 | 0.9 | 0.28 |
| postings: tf L1 bits + nibbles | 268,174 | 2.3 | 0.70 |
| postings: position checkpoints | 382,540 | 3.2 | 1.00 |
| postings: position records | 3,154,415 | 26.7 | 8.25 |
| dictionary (170 terms) | 3,268 | 0.0 | 0.01 |
| live pages | 221,184 | 1.9 | 0.58 |
| page headers, segment header, unused end | 67,643 | 0.6 | 0.18 |

**Articles, 50,000 documents, default (32,628,736 bytes; `positions = off`: 13,557,760):**

| part | bytes | % default | % off |
|---|---:|---:|---:|
| postings: LIST codec (3 to 8,191 postings) | 6,278,632 | 19.2 | 46.3 |
| postings: GROUPED containers + directory | 672,522 | 2.1 | 5.0 |
| postings: tf L1 bits, nibbles, escapes | 1,520,335 | 4.7 | 11.2 |
| postings: group scores, block bounds, alignment | 113,635 | 0.3 | 0.8 |
| postings: position records + checkpoints | 17,896,371 | 54.8 | n/a |
| dictionary: term suffixes (shared prefixes taken out, 2 length bytes each) | 1,578,275 | 4.8 | 11.6 |
| dictionary: entries (df, flags, bound, lengths) | 1,339,606 / 1,230,802 | 4.1 | 9.1 |
| dictionary: inline keys and tf (255,791 terms with at most 2 postings) | 1,262,396 | 3.9 | 9.3 |
| dictionary: inline positions | 977,078 | 3.0 | n/a |
| dictionary: block headers, index, root | 238,651 / 237,082 | 0.7 | 1.7 |
| docmap | 116,664 | 0.4 | 0.9 |
| live pages | 352,256 | 1.1 | 2.6 |
| page headers, segment header, unused end | 282,315 / 195,161 | 0.9 | 1.4 |

The full tables with every part, B/posting and B/term are produced by `scripts/bm25-size.sh`; the ones of the baseline are in section 7.

### 3.3 What the representation costs (MEASURED)

- **GROUPED against LIST on the titles.** 75 terms have 8,192 or more postings (1.7 M and 0.8 M postings in the two top df classes). As GROUPED they take
  16.2 bits per posting (array containers: 2 bytes per posting, `page << 8 | off - 1`), as LIST (Elias-Fano over `blk << 7 | off - 1`) 7.6 bits: the
  whole index is 8.83 MiB instead of 11.27 (-22%). **Queries pay for it**, user-space instructions per execution (`qperf`, 100 executions, the
  backend's `perf stat`), GROUPED against all-LIST: `count(*) 'wireless headphones'` 1.6 M against 25.1 M (15x), `smart AND premium AND black` 8.0 M against
  15.3 M (1.9x), `smart OR premium` 6.9 M against 13.0 M (1.9x), top-10 `smart OR premium OR wireless` 30.4 M against 59.2 M (1.9x), top-10 `smart AND premium`
  18.5 M against 24.7 M (1.3x), top-10 `wireless AND headphones` 13.8 M against 37.4 M (2.7x), phrase 13.9 M against 37.5 M (2.7x). Lowering the
  GROUPED threshold is the wrong fix: **the containers have to shrink, the kind has to stay** (section 5.3).
- **Build in four segments.** A build with `build_segments = 4` is 10.93 MB (a term of 25 thousand postings has 6 thousand in each segment, so it is LIST),
  the merge of the four is 11.80 MB, identical to the serial single-segment build; the smaller size is the same trade as above, paid in queries.
- **Positions.** The record of a posting is `first << 1 | multi` as a LEB128 varint, then for repeated terms `n - 2` and `n - 1` gaps. On the titles it
  is 1.02 bytes per posting (the ordinal of the term in a 3-token title: one byte holds 3 bits of information); on the articles 3.4 bytes per posting
  and 12.7 bits per occurrence. A simulation over the tokens of the articles (`possim.py`, the sum of today's records and checkpoints is 17,985,964 bytes against 17,896,371
  read from the index) splits them: first positions 47.4%, repeat gaps 38.9%, counts 8.8%, checkpoints 4.9%. Coding the first positions in blocks of 32
  bit-packed at their widest saves 2.0% of that (an article is 234 tokens: a first position carries about 10.8 bits whatever the code); Rice-coded gaps
  (a parameter for each block) 19% of the gaps; counts as Elias-gamma 52% of the counts; all together 14% of the stream, 7.7% of the index. On the titles
  the same blocks are 13 bytes per 32 postings instead of 32 (-59%).

### 3.4 Layout checks (MEASURED)

- Page fill: every page of a segment's stream is full except the last; a stream page has 8,152 bytes of payload in 8,192 (0.5%).
- Alignment: a blob starts 4-byte aligned, which costs 1.5 bytes on average per blob; 85,635 bytes on 77,502 blobs of the articles.
- Merge: the one-segment build and the merged four are byte-identical. After `sift.merge(.., true)` of four segments the file is 22.9 MB because the replaced segments' blocks
  (1,354 of 2,797) are free but in front of the live segment, so `VACUUM` cannot cut them off; the next build or merge of the index reuses them. This is
  space management (`vacuum_truncate`, DESIGN.md (i)), not format, and is not changed here.
- Liveness pages (`live pages`): 27 pages on the titles (a row of the offsets of the documents that are live for every present page, 16 bytes per page of 76 rows)
  and 43 on the articles, of which 310 KB are the table of dead postings per non-inline term, 4 bytes each, all zero until a VACUUM.

## 4. Candidates

| # | candidate | needs a format change | titles | articles | decision |
|---|---|---|---|---|---|
| 1 | padding, page fill, merge policy | no | 0 | 0 | nothing to take (3.4) |
| 2 | GROUPED threshold / kind by size | no | -2.4 MiB, queries 1.3x to 15x | 0 | rejected (3.3) |
| 3 | dictionary entries (1 byte of lengths, df with the flags, inline keys by block and offset) | yes (dictionary block flag) | 0 | -0.93 MB (-2.9% default, -6.9% off) | **done (5.2)** |
| 4 | dl rows of 4 or 8 bits where the largest dl of the SB allows | yes (docmap record flag) | -1.50 MB (-12.7%) | 0 | **done (5.1)** |
| 5 | packed container for GROUPED (bit-packed offsets, page mask, end-of-page bits) | yes (container type) | -1.99 MB (-16.9%) | -0.02 MB | **done (5.3)** |
| 6 | positions in blocks of 32: bit-packed first positions, bit-packed counts and gaps | yes (term flag) | -1.9 MB (-23% of the new default, est.) | -0.4 to -2.5 MB (est.) | designed (5.4), **not done** |
| 7 | tf stream: L1 bits only for groups with a tf above 1 | yes | -0.2 MB | -0.2 MB (est.) | not done (entropy of 20% ones is 0.72 bit: 28% of 0.67 MB) |
| 8 | dead-postings table allocated at the first VACUUM | yes (liveness run) | -0.2 MB | -0.3 MB (-2.3% off) | not done (5.6) |
| 9 | LIST blocks: no `u - 1` varint in single-block lists | yes | 0 | -0.3 MB (est.) | not done |
| 10 | position-less phrase for stop-like terms | semantics | | | rejected: the term's positions are what decides a phrase |
| 11 | zstd layer on cold segments | yes | | | not done: decode cost on every query path; every part above is read by queries |

## 5. Design: segment format 3

### 5.0 Compatibility (the format policy)

- **Segment format 3** (`SIFT_SEGFMT_PACKED`), metapage minor version 3 (`SIFT_FORMAT_MINOR` 2 to 3). A segment is written in format 3 by the PostgreSQL
  host (`SIFT_WRITE_SEGFMT`); the SQLite and MariaDB hosts keep writing format 1 and read all three. The directory entry carries the format of every segment; the
  metapage's minor version is raised to the largest format of a published segment (already how format 2 does it), and a fresh index starts at 1.3.
- **One build reads and writes side by side.** Every format 3 part is announced where it starts, so a segment of format 2 (or 1) is read with no
  switch: a dictionary block (the high bit of its term count), a docmap record (the high bits of its document count), a GROUPED container (its type
  byte, values the old ones never use) and a term's position stream (a term flag). A segment is a mix of old and new parts only through a merge that copies
  nothing as it is: merges and promotions re-encode every posting through the runs, so every segment a build writes is wholly format 3, and an index
  holds format 1, 2 and 3 segments side by side until its old ones are merged or rewritten (VACUUM rewrites the mostly dead ones, `sift.merge(index, true)` the
  smallest run; REINDEX is never required to keep an index working, only to shrink it at once).
- **An older library refuses the index**: the metapage says 1.3 and the 3.0 library's `check_meta` raises `sift index "NAME" has on-disk format 1.3, this
  build reads formats 1.0 to 1.2` with the hint `REINDEX the index.` (REINDEX with the old library writes format 2 again, since it is a new build). Nothing is
  misread. `ALTER EXTENSION pg_sift UPDATE` changes no SQL object (the library is replaced, the index is read as above), and `test/t/extension_lifecycle.sh`
  and the new `test/t/compat_30_fixture.sh` check both with an index written by the 3.0 library.
- **Test hook.** In a testing build the GUC `sift.test_segfmt` (2 or 3) selects the format a build, promotion or merge writes, which is how the regression
  tests make format 2 and mixed indexes and how the size and speed of both are compared in one build.

### 5.1 Docmap rows of 4 or 8 bits (`SIFT_SEGFMT_PACKED`)

A DOCMAP record (per SB) is `u32 ndocs, u32 chunk_len, chunk, pad4, dl rows`. In format 3 `ndocs` keeps its low 30 bits and bits 30-31 are the width of every dl row of
the record: 0 = 16 bits (as before), 1 = 8 bits, 2 = 4 bits (low nibble first). The writer takes the narrowest width whose maximum holds the largest dl of the SB
(255 and 15). Rows stay one per present page and as long as the page's highest present offset; a 4-bit row is `ceil(maxoff / 2)` bytes, a record is padded to 4 as before.
The reader expands the row of a page to `uint16` (16 slots per SSE2 step) the first time `sift_seg_page` is asked for it in the loaded SB, so every kernel sees the rows it saw;
a 16-bit record is used where it is, copy free. `sift_seg_page` takes the row as optional (callers that only want the present offsets pass NULL) and the ranker asks for
the rows of the pages that have result documents only, one call per block (`sift_seg_block_dl`). Titles: 76 rows of 4 bits are 38 bytes against 152; articles are 16-bit throughout.

### 5.2 Dictionary blocks (`SIFT_SEGFMT_PACKED`)

A DICT block is `u16 block bytes, u8 nterms, varint post_base, entries`. In format 3 bit 7 of `nterms` marks the new entry (a segment can hold both; DICTIDX
and the root array are unchanged):

```
u8 hdr                low nibble: shared prefix length (15: u8 shared follows), high nibble: suffix length - 1 (15: u8 suffix length - 1 follows)
suffix
u8 fl                 low nibble: flags (SPARSE, INLINE, ALL_TF1, POS), high nibble: df (1..14; 15: varint df - 15 follows)
u8 max_bound
inline term:  key0 as varint(blk << 9 | off - 1), df = 2: varint of the difference of the two such values; tf bytes unless ALL_TF1; position records as before
blob term:    varint post_len, varint sidecar length, varint nnib unless ALL_TF1, varint pos_len if POS, as before
```

Saves one byte of lengths and one of df per term, and about 2 bytes per inline key (a key `blk << 16 | off` is 5 bytes as a varint once the table is longer than 4,096 pages, `blk << 9 | off - 1`
is 3 up to 2,097,152 blocks).

### 5.3 Packed container for GROUPED terms (`SIFT_SEGFMT_PACKED`)

A GROUPED term has one container per group of 256 pages, typed by the low byte of its `key[]` entry: 0x01-0x40 BITMAP (width w), 0x81-0xC0 ARRAY (w, u16 or u32 entries).
Format 3 adds type `0xE0 | k` PACKED (`k` = index of w in 1, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64) for a group of `n` postings (known from `base[]`), chosen where an ARRAY would be and
is larger:

```
u8 ob                  bits of an offset: nbits(max offset - 1), at least 1
u8 pages[32]           the pages of the group that have postings (bit p = page p)
offsets                n values of ob bits (off - 1), little endian bit order, in posting order, padded to a byte
page ends              n bits, bit k = 1 when posting k is the last of its page (a page of c postings is c - 1 zeros and a one)
```

33 + ceil(n * ob / 8) + ceil(n / 8) bytes against 2n (ARRAY), about 9 bits per posting where the ARRAY has 16. The cursor decodes it page by page straight into the bitmap rows of the
decoded group (`sift_grp`), the form an ARRAY builds, so every kernel, bound and rank function is untouched; it is cheaper than the ARRAY build because the page is known for a run of
postings (no per-posting page compare). A BITMAP is still chosen where it is smaller than the ARRAY (dense groups, zero-copy) and is never repacked. `sift.verify` checks the sizes, the
page ends, the offsets inside the width and ascending on a page. For the titles: 3,900 groups hold 2,534,639 postings; 4,232,618 of the 4,654,650 bytes of containers are ARRAY and become 2.16 MB.

### 5.4 Position blocks (term flag `SIFT_TERM_POS2`)

A term's position stream (after its tf stream, as in 1.1) holds `u32 off[ceil(df / 32)]` (byte offset of each block of 32 postings in the data, as the checkpoints today) and the blocks. A block holds postings `32b .. 32b + 31`:

```
u8 hdr           bits 0-5: bit width wf of the first positions (0-32), bit 6: some posting of the block occurs more than once
[u8 multi[ceil(k / 8)]]   bit i: posting i occurs more than once (only if bit 6)
first[k]         k values at wf bits, little endian bit order, padded to a byte
for each multi posting, in order:  varint n - 2, then n - 1 varints gap - 1     (as in a record today)
```

Records keep their meaning: first, then `n - 1` gaps (positions strictly increase). `sift_post_positions(rank)` finds the block through `off[]`, skips the multi extras of the postings before
it in the block and decodes the one it wants. The streaming reader the merge uses (`sift_post_pos_reader`) re-expresses each block's postings as the LEB128 records of format 2, which is the interface of the runs and of the
writer, so no other module changes. Terms of at most two postings keep their records inline, unchanged.

### 5.5 Not in this change

- Position blocks (5.4): designed, not implemented. They would take the titles' positions from 3.54 MB to about 1.6 MB, but only 2% of the articles' (the first positions of a posting are 47% of that stream and are close to random: a fixed width per block saves about 0.4 MB of 18.9 MB;
  a Rice code on the gaps and a gamma code on the counts would take 14% of the stream, 7.7% of the index, and cost a decode on every phrase). ClickHouse's positions are 10 times larger than sift's, so the target of the work (an index as small as ClickHouse's for the same information) does not need them.
- Items 7 to 9 of section 4 (a few tenths of a megabyte each).
- The SQLite and MariaDB writers: they stay in format 1 (a host that deletes one document at a time, and whose segments are written by the portable core, would need its own gate). They read format 3.

### 5.6 Test plan

- Round trip: new regression tests build the same corpus with `sift.test_segfmt = 2` and `3` (and mixed), compare `sift._test_postings` (every posting with tf and dl) and `sift.verify(.., true)`.
- Differential: `scripts/difftest.sh` (reference SQL against sift, with VACUUM, promotion and merge between rounds) on the new default, with and without positions; the coverage-guided `bm25` fuzzer replays term blobs of the new containers.
- Upgrade: `test/fixtures/compat30` is an index written by the 3.0 library (format 2, two segments, one dirty, a write buffer); `test/t/compat_30_fixture.sh` reads it, mixes it with format 3 segments (promotion), merges them, vacuums, and checks that the 3.0 results hold.
- Crash and replica: `test/t/index_random_crash.sh`, the standby and WAL-consistency TAP tests run on indexes of format 3 (every part is written through the same stream), with a mixed index in the replica test.
- Cost: `bench/steps/cpu_gate.sh` against origin/main on the rank, and and or cells (ratio at most 1.02), build time (10%), `qperf` on the titles and the articles.

## 6. Results

Segment format 3 (sections 5.1 to 5.3) on the data of section 3, one segment per index, `pg_relation_size`; before is origin/main (3.0).

### 6.1 Bytes and the comparison with ClickHouse

| index | titles before | titles after | articles before | articles after | holds |
|---|---:|---:|---:|---:|---|
| sift, default (MiB) | 11.27 | **7.93** (-29.6%) | 31.13 | **30.21** (-3.0%) | postings, tf, dl, bounds, liveness, positions |
| sift, `positions = off` (MiB) | 7.88 | **4.54** (-42.4%) | 12.95 | **12.02** (-7.2%) | the same without positions |
| bytes per posting, default | 3.86 | 2.71 | 5.88 | 5.70 | |
| bytes per posting, `positions = off` | 2.70 | 1.55 | 2.44 | 2.27 | |
| ClickHouse `text`, default (MiB) | 5.74 | | 12.84 (11.94 lowercased) | | token to row ids |
| ClickHouse `text`, bitpacking (MiB) | 3.15 | | 9.34 lowercased | | the same |
| ClickHouse `text`, `support_phrase_search` (MiB) | 40.77 | | 122.58 lowercased | | + positions |

- With positions, sift holds what ClickHouse's phrase index holds and more (tf, dl, bounds, liveness) in 19% (titles) and 25% (articles) of its bytes.
- Without positions the titles are 21% smaller than ClickHouse's default index and 44% larger than its bitpacked one, the articles 6% smaller than the raw-token default and 0.7% larger than the lowercased one.
  ClickHouse's index has no tf, dl, score bounds or liveness: it answers `hasAllTokens` and nothing that ranks. The data a boolean match needs from sift (dictionary, posting containers, present documents) is 3.2 MiB on the titles
  and about 9.8 MiB on the articles, against 5.74 / 3.15 and 11.94 / 9.34 for ClickHouse's default / bitpacked index.
- Where sift stays larger and why: the posting lists keep the ctid of a document (heap page and offset) as their key, about 1 to 1.2 bits per posting of key-space slack that a dense document id does not have (docs/CODEC.md section 1, item 7);
  the score data (tf 0.7 bit per posting on the titles, 1.8 on the articles, bounds) is the price of ranking and early termination; the positions (8 to 25 bits per posting) are what a phrase needs. ClickHouse's own positions cost 10 times more.

### 6.2 Parts, before and after

Bytes of each part of the segment, bits per posting before and after (`scripts/bm25-size.sh`). A posting is a (term, document) pair, inline ones included (titles 3,057,633, articles 5,551,834).

#### orders.title, default

| part | before | after | bits/posting before | after |
|---|---:|---:|---:|---:|
| dict: block headers | 42 | 40 | 0.00 | 0.00 |
| dict: entries | 2,024 | 2,024 | 0.01 | 0.01 |
| dict: index and root | 104 | 104 | 0.00 | 0.00 |
| dict: term suffixes | 1,098 | 928 | 0.00 | 0.00 |
| docmap: SB directory | 836 | 836 | 0.00 | 0.00 |
| docmap: dl rows | 2,000,000 | 502,892 | 5.23 | 1.32 |
| docmap: present docs | 211,688 | 211,724 | 0.55 | 0.55 |
| live pages | 221,184 | 221,184 | 0.58 | 0.58 |
| postings: GROUPED containers | 4,654,650 | 2,666,649 | 12.18 | 6.98 |
| postings: GROUPED directory | 48,300 | 48,300 | 0.13 | 0.13 |
| postings: LIST codec | 674,864 | 674,864 | 1.77 | 1.77 |
| postings: alignment | 86 | 111 | 0.00 | 0.00 |
| postings: block bounds | 62,032 | 62,032 | 0.16 | 0.16 |
| postings: group scores | 46,800 | 46,800 | 0.12 | 0.12 |
| postings: position checkpoints | 382,540 | 382,540 | 1.00 | 1.00 |
| postings: position records | 3,154,415 | 3,154,415 | 8.25 | 8.25 |
| postings: tf L1 bits | 243,970 | 243,970 | 0.64 | 0.64 |
| postings: tf nibbles | 24,204 | 24,204 | 0.06 | 0.06 |
| other (page headers, segment header, unused end) | 67,643 | 54,879 | 0.18 | 0.14 |
| **segment total** | **11,796,480** | **8,298,496** (-29.7%) | 30.86 | 21.71 |

#### orders.title, positions = off

| part | before | after | bits/posting before | after |
|---|---:|---:|---:|---:|
| dict: block headers | 42 | 39 | 0.00 | 0.00 |
| dict: entries | 1,472 | 1,452 | 0.00 | 0.00 |
| dict: index and root | 104 | 104 | 0.00 | 0.00 |
| dict: term suffixes | 1,098 | 928 | 0.00 | 0.00 |
| docmap: SB directory | 836 | 836 | 0.00 | 0.00 |
| docmap: dl rows | 2,000,000 | 502,892 | 5.23 | 1.32 |
| docmap: present docs | 211,688 | 211,724 | 0.55 | 0.55 |
| live pages | 221,184 | 221,184 | 0.58 | 0.58 |
| postings: GROUPED containers | 4,654,650 | 2,666,649 | 12.18 | 6.98 |
| postings: GROUPED directory | 48,300 | 48,300 | 0.13 | 0.13 |
| postings: LIST codec | 674,864 | 674,864 | 1.77 | 1.77 |
| postings: alignment | 86 | 111 | 0.00 | 0.00 |
| postings: block bounds | 62,032 | 62,032 | 0.16 | 0.16 |
| postings: group scores | 46,800 | 46,800 | 0.12 | 0.12 |
| postings: tf L1 bits | 243,970 | 243,970 | 0.64 | 0.64 |
| postings: tf nibbles | 24,204 | 24,204 | 0.06 | 0.06 |
| other (page headers, segment header, unused end) | 49,822 | 37,079 | 0.13 | 0.10 |
| **segment total** | **8,241,152** | **4,743,168** (-42.4%) | 21.56 | 12.41 |

#### docs.body, default

| part | before | after | bits/posting before | after |
|---|---:|---:|---:|---:|
| dict: block headers | 71,667 | 71,666 | 0.10 | 0.10 |
| dict: entries | 1,339,606 | 1,030,183 | 1.93 | 1.48 |
| dict: index and root | 166,984 | 166,984 | 0.24 | 0.24 |
| dict: inline keys and tf | 1,262,396 | 969,183 | 1.82 | 1.40 |
| dict: inline positions | 977,078 | 977,078 | 1.41 | 1.41 |
| dict: term suffixes | 1,578,275 | 1,245,931 | 2.27 | 1.80 |
| docmap: SB directory | 324 | 324 | 0.00 | 0.00 |
| docmap: dl rows | 100,000 | 100,000 | 0.14 | 0.14 |
| docmap: present docs | 16,340 | 16,340 | 0.02 | 0.02 |
| live pages | 352,256 | 352,256 | 0.51 | 0.51 |
| postings: GROUPED containers | 659,522 | 634,997 | 0.95 | 0.92 |
| postings: GROUPED directory | 13,000 | 13,000 | 0.02 | 0.02 |
| postings: LIST codec | 6,278,632 | 6,278,632 | 9.05 | 9.05 |
| postings: alignment | 85,635 | 85,648 | 0.12 | 0.12 |
| postings: block bounds | 16,000 | 16,000 | 0.02 | 0.02 |
| postings: group scores | 12,000 | 12,000 | 0.02 | 0.02 |
| postings: position checkpoints | 875,112 | 875,112 | 1.26 | 1.26 |
| postings: position records | 17,021,259 | 17,021,259 | 24.53 | 24.53 |
| postings: tf L1 bits | 670,491 | 670,491 | 0.97 | 0.97 |
| postings: tf escapes | 48,855 | 48,855 | 0.07 | 0.07 |
| postings: tf nibbles | 800,989 | 800,989 | 1.15 | 1.15 |
| other (page headers, segment header, unused end) | 282,315 | 275,152 | 0.41 | 0.40 |
| **segment total** | **32,628,736** | **31,662,080** (-3.0%) | 47.02 | 45.62 |

#### docs.body, nopos (positions = off)

| part | before | after | bits/posting before | after |
|---|---:|---:|---:|---:|
| dict: block headers | 70,098 | 70,088 | 0.10 | 0.10 |
| dict: entries | 1,230,802 | 921,376 | 1.77 | 1.33 |
| dict: index and root | 166,984 | 166,984 | 0.24 | 0.24 |
| dict: inline keys and tf | 1,262,396 | 969,183 | 1.82 | 1.40 |
| dict: term suffixes | 1,578,275 | 1,245,931 | 2.27 | 1.80 |
| docmap: SB directory | 324 | 324 | 0.00 | 0.00 |
| docmap: dl rows | 100,000 | 100,000 | 0.14 | 0.14 |
| docmap: present docs | 16,340 | 16,340 | 0.02 | 0.02 |
| live pages | 352,256 | 352,256 | 0.51 | 0.51 |
| postings: GROUPED containers | 659,522 | 634,997 | 0.95 | 0.92 |
| postings: GROUPED directory | 13,000 | 13,000 | 0.02 | 0.02 |
| postings: LIST codec | 6,278,632 | 6,278,632 | 9.05 | 9.05 |
| postings: alignment | 85,635 | 85,648 | 0.12 | 0.12 |
| postings: block bounds | 16,000 | 16,000 | 0.02 | 0.02 |
| postings: group scores | 12,000 | 12,000 | 0.02 | 0.02 |
| postings: tf L1 bits | 670,491 | 670,491 | 0.97 | 0.97 |
| postings: tf escapes | 48,855 | 48,855 | 0.07 | 0.07 |
| postings: tf nibbles | 800,989 | 800,989 | 1.15 | 1.15 |
| other (page headers, segment header, unused end) | 195,161 | 188,010 | 0.28 | 0.27 |
| **segment total** | **13,557,760** | **12,591,104** (-7.1%) | 19.54 | 18.14 |

What changed, by part:

- `docmap: dl rows` (5.1): 5.23 to 1.32 bits per posting on the titles (4 bits per slot where the largest length of the superblock is at most 15); the articles have lengths above 255 and keep 16 bits.
- `postings: GROUPED containers` (5.3): 12.18 to 6.98 bits per posting on the titles (-2.0 MB), 0.95 to 0.92 on the articles (their groups are mostly dense bitmaps, 10 documents per page).
- `dict` (5.2): the articles' 4.2 MB of dictionary entries, terms and inline keys lose 0.93 MB (suffixes 3.74 instead of 4.73 B per term, entries 3.09 instead of 4.02, inline keys 2.91 instead of 3.79).
- Unchanged: LIST codec (Elias-Fano, 9.05 bits per posting on the articles), tf, bounds, scores, liveness, positions (5.4, below).

### 6.3 What it costs

User-space instructions per execution (`perf stat`, backend of a started cluster, 100 runs, parallel workers off), titles, origin/main against format 3 (both indexes in the cluster built from the same data):

| query | before kinstr | after kinstr | ratio |
|---|---:|---:|---:|
| count, 2 terms (AND) | 1,628 | 1,539 | 0.945 |
| count, `smart AND premium AND black` | 8,048 | 6,705 | 0.833 |
| count, `smart OR premium` | 6,886 | 5,663 | 0.822 |
| top-10, 3 terms OR | 30,444 | 30,584 | 1.005 |
| top-10, `smart AND premium` | 18,521 | 17,072 | 0.922 |
| top-10, `wireless AND headphones` | 13,820 | 13,914 | 1.007 |
| top-10, phrase | 13,882 | 13,977 | 1.007 |

Counts get faster (the packed container decodes page by page straight into the bitmap rows, which is cheaper than the array decode it replaces). The ranked cells stay within +0.7%: the narrow docmap rows
are expanded only for the pages that hold candidates, with one call per block.

`bench/steps/cpu_gate.sh` against origin/main (7 rounds, geometric mean, 95% interval): every rank, and and or cell is at or below 1.017 (`idle1_or` 1.017 [1.016, 1.018]; `idle60_*` and `vac_*` 0.988 to 1.000).
Maintenance: `create` 1.006, `merge` 1.003, `promote` 1.012, `vacuum_*` 0.992 to 1.005, insert/bulk/copy/update/delete 0.996 to 1.006. No cell has an interval entirely above 1.02; `session` (1.016 [0.994, 1.039]) is uncertain by noise.

Build time (`CREATE INDEX`, serial, median of 5, two rounds, titles): default 509 / 471 ms before, 506 / 469 ms after; `positions = off` 433 / 429 ms before, 465 / 410 ms after: no measurable change.
Articles, default: 1.98 / 1.83 s after (the cost gate's `create` cell, 873 M instructions, is 1.006 of origin/main).

Not fixed in this change: a ranked difftest case that fails in the same way on origin/main (a fuzzy or regular-expression term whose score differs by a few percent from the SQL reference); it reproduces with the unchanged library.

## 7. Baseline part tables (origin/main, before 3.1)

`scripts/bm25-size.sh` output; each block is one index (the second of each pair is `positions = off`). A posting is a (term, document) pair, inline ones included (titles 3,057,633; articles 5,551,834).
### orders.title, default

| part | bytes | B/posting | bits/posting | B/term |
|---|---:|---:|---:|---:|
| dict: block headers | 42 | 0.000 | 0.00 | 0.25 |
| dict: entries | 2,024 | 0.001 | 0.01 | 11.91 |
| dict: index and root | 104 | 0.000 | 0.00 | 0.61 |
| dict: inline keys and tf | 0 | 0.000 | 0.00 | 0.00 |
| dict: inline positions | 0 | 0.000 | 0.00 | 0.00 |
| dict: term suffixes | 1,098 | 0.000 | 0.00 | 6.46 |
| docmap: SB directory | 836 | 0.000 | 0.00 | 4.92 |
| docmap: dl rows | 2,000,000 | 0.654 | 5.23 | 11764.71 |
| docmap: present docs | 211,688 | 0.069 | 0.55 | 1245.22 |
| live pages | 221,184 | 0.072 | 0.58 | 1301.08 |
| postings: GROUPED containers | 4,654,650 | 1.522 | 12.18 | 27380.29 |
| postings: GROUPED directory | 48,300 | 0.016 | 0.13 | 284.12 |
| postings: LIST codec | 674,864 | 0.221 | 1.77 | 3969.79 |
| postings: alignment | 86 | 0.000 | 0.00 | 0.51 |
| postings: block bounds | 62,032 | 0.020 | 0.16 | 364.89 |
| postings: group scores | 46,800 | 0.015 | 0.12 | 275.29 |
| postings: position checkpoints | 382,540 | 0.125 | 1.00 | 2250.24 |
| postings: position records | 3,154,415 | 1.032 | 8.25 | 18555.38 |
| postings: tf L1 bits | 243,970 | 0.080 | 0.64 | 1435.12 |
| postings: tf escapes | 0 | 0.000 | 0.00 | 0.00 |
| postings: tf nibbles | 24,204 | 0.008 | 0.06 | 142.38 |
| other (page headers, segment header, unused end) | 67,643 | 0.022 | 0.18 | 397.90 |
| segment total | 11,796,480 | 3.858 | 30.86 | 69391.06 |
1000000 documents, 170 terms, 3057633 postings in the blobs and inline; relation 11812864 bytes (11.27 MiB)

### orders.title, positions = off

| part | bytes | B/posting | bits/posting | B/term |
|---|---:|---:|---:|---:|
| dict: block headers | 42 | 0.000 | 0.00 | 0.25 |
| dict: entries | 1,472 | 0.000 | 0.00 | 8.66 |
| dict: index and root | 104 | 0.000 | 0.00 | 0.61 |
| dict: inline keys and tf | 0 | 0.000 | 0.00 | 0.00 |
| dict: inline positions | 0 | 0.000 | 0.00 | 0.00 |
| dict: term suffixes | 1,098 | 0.000 | 0.00 | 6.46 |
| docmap: SB directory | 836 | 0.000 | 0.00 | 4.92 |
| docmap: dl rows | 2,000,000 | 0.654 | 5.23 | 11764.71 |
| docmap: present docs | 211,688 | 0.069 | 0.55 | 1245.22 |
| live pages | 221,184 | 0.072 | 0.58 | 1301.08 |
| postings: GROUPED containers | 4,654,650 | 1.522 | 12.18 | 27380.29 |
| postings: GROUPED directory | 48,300 | 0.016 | 0.13 | 284.12 |
| postings: LIST codec | 674,864 | 0.221 | 1.77 | 3969.79 |
| postings: alignment | 86 | 0.000 | 0.00 | 0.51 |
| postings: block bounds | 62,032 | 0.020 | 0.16 | 364.89 |
| postings: group scores | 46,800 | 0.015 | 0.12 | 275.29 |
| postings: position checkpoints | 0 | 0.000 | 0.00 | 0.00 |
| postings: position records | 0 | 0.000 | 0.00 | 0.00 |
| postings: tf L1 bits | 243,970 | 0.080 | 0.64 | 1435.12 |
| postings: tf escapes | 0 | 0.000 | 0.00 | 0.00 |
| postings: tf nibbles | 24,204 | 0.008 | 0.06 | 142.38 |
| other (page headers, segment header, unused end) | 49,822 | 0.016 | 0.13 | 293.07 |
| segment total | 8,241,152 | 2.695 | 21.56 | 48477.36 |
1000000 documents, 170 terms, 3057633 postings in the blobs and inline; relation 8257536 bytes (7.88 MiB)
### docs.body, default

| part | bytes | B/posting | bits/posting | B/term |
|---|---:|---:|---:|---:|
| dict: block headers | 71,667 | 0.013 | 0.10 | 0.22 |
| dict: entries | 1,339,606 | 0.241 | 1.93 | 4.02 |
| dict: index and root | 166,984 | 0.030 | 0.24 | 0.50 |
| dict: inline keys and tf | 1,262,396 | 0.227 | 1.82 | 3.79 |
| dict: inline positions | 977,078 | 0.176 | 1.41 | 2.93 |
| dict: term suffixes | 1,578,275 | 0.284 | 2.27 | 4.74 |
| docmap: SB directory | 324 | 0.000 | 0.00 | 0.00 |
| docmap: dl rows | 100,000 | 0.018 | 0.14 | 0.30 |
| docmap: present docs | 16,340 | 0.003 | 0.02 | 0.05 |
| live pages | 352,256 | 0.063 | 0.51 | 1.06 |
| postings: GROUPED containers | 659,522 | 0.119 | 0.95 | 1.98 |
| postings: GROUPED directory | 13,000 | 0.002 | 0.02 | 0.04 |
| postings: LIST codec | 6,278,632 | 1.131 | 9.05 | 18.84 |
| postings: alignment | 85,635 | 0.015 | 0.12 | 0.26 |
| postings: block bounds | 16,000 | 0.003 | 0.02 | 0.05 |
| postings: group scores | 12,000 | 0.002 | 0.02 | 0.04 |
| postings: position checkpoints | 875,112 | 0.158 | 1.26 | 2.63 |
| postings: position records | 17,021,259 | 3.066 | 24.53 | 51.07 |
| postings: tf L1 bits | 670,491 | 0.121 | 0.97 | 2.01 |
| postings: tf escapes | 48,855 | 0.009 | 0.07 | 0.15 |
| postings: tf nibbles | 800,989 | 0.144 | 1.15 | 2.40 |
| other (page headers, segment header, unused end) | 282,315 | 0.051 | 0.41 | 0.85 |
| segment total | 32,628,736 | 5.877 | 47.02 | 97.90 |
50000 documents, 333293 terms, 5551834 postings in the blobs and inline; relation 32645120 bytes (31.13 MiB)

### docs.body, nopos (positions = off)

| part | bytes | B/posting | bits/posting | B/term |
|---|---:|---:|---:|---:|
| dict: block headers | 70,098 | 0.013 | 0.10 | 0.21 |
| dict: entries | 1,230,802 | 0.222 | 1.77 | 3.69 |
| dict: index and root | 166,984 | 0.030 | 0.24 | 0.50 |
| dict: inline keys and tf | 1,262,396 | 0.227 | 1.82 | 3.79 |
| dict: inline positions | 0 | 0.000 | 0.00 | 0.00 |
| dict: term suffixes | 1,578,275 | 0.284 | 2.27 | 4.74 |
| docmap: SB directory | 324 | 0.000 | 0.00 | 0.00 |
| docmap: dl rows | 100,000 | 0.018 | 0.14 | 0.30 |
| docmap: present docs | 16,340 | 0.003 | 0.02 | 0.05 |
| live pages | 352,256 | 0.063 | 0.51 | 1.06 |
| postings: GROUPED containers | 659,522 | 0.119 | 0.95 | 1.98 |
| postings: GROUPED directory | 13,000 | 0.002 | 0.02 | 0.04 |
| postings: LIST codec | 6,278,632 | 1.131 | 9.05 | 18.84 |
| postings: alignment | 85,635 | 0.015 | 0.12 | 0.26 |
| postings: block bounds | 16,000 | 0.003 | 0.02 | 0.05 |
| postings: group scores | 12,000 | 0.002 | 0.02 | 0.04 |
| postings: position checkpoints | 0 | 0.000 | 0.00 | 0.00 |
| postings: position records | 0 | 0.000 | 0.00 | 0.00 |
| postings: tf L1 bits | 670,491 | 0.121 | 0.97 | 2.01 |
| postings: tf escapes | 48,855 | 0.009 | 0.07 | 0.15 |
| postings: tf nibbles | 800,989 | 0.144 | 1.15 | 2.40 |
| other (page headers, segment header, unused end) | 195,161 | 0.035 | 0.28 | 0.59 |
| segment total | 13,557,760 | 2.442 | 19.54 | 40.68 |
50000 documents, 333293 terms, 5551834 postings in the blobs and inline; relation 13574144 bytes (12.95 MiB)
