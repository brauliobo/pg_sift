# pg_sift codec

The `codec` module (`src/bm25/sift_bitmap.h`, `src/bm25/sift_bitmap*.c`, `src/bm25/sift_bitmap_kern.h`) encodes posting sets keyed by heap ctid and runs the set kernels. It is plain C11 plus intrinsics with no PostgreSQL dependency. It does no allocation, and its only global state is the dispatch table. The standalone harness lives in `tools/codec/`: tests, corpus tools and benchmarks.

All numbers below were measured on a 3.04 M-row English text corpus (title plus body) on a Ryzen 9 9700X. That machine was shared and heavily loaded (load average about 38 on 16 threads), so:
- every timing is the best of 5 to 9 trials, pinned to one core;
- absolute speeds are pessimistic, and ratios measured in the same run are the reliable part.

## 1. Recommendation (summary for the architect)

1. **Two representations per term, switched by df. Not "bitmaps everywhere".**
   - **LIST** is Elias-Fano blocks over the ctid key, for `df < 8192` (`SIFT_GROUPED_MIN_DF`, was 65536; §4).
   - **GROUPED** uses 256-page groups with a bitmap or array container per group, for frequent terms.
   - Singletons (df = 1) stay inline in the dictionary. Keep df ≤ 2 inline too, as DESIGN.md says: a LIST blob for df = 2 costs about 13 B, the same as two raw keys.
   - On our data, EF lists are smaller than two-level bitmaps in **every** df class: 10.0 against 57 bits per posting overall (§5.1).
   - Bitmaps pay for themselves only through speed, and only on frequent terms: 5 to 65× faster AND/count than lists (§5.3).
   - The `df ≥ 64K → GROUPED` policy costs +20 % size over all-LIST: 201 MB against 167 MB. It covers 326 terms that hold 51 % of all postings.
2. **Offset bitmaps must be variable width.** Use `w ∈ {1, 2, 4, 8, 16, 24, …, 64}` bytes per group, from the largest offset in that group.
   - A literal fixed 512-bit mask per page costs 412 bits per posting on our data, against 57 at tight width. We have 8.8 tuples per page, so w is 1 B for 58 % of groups and 2 B for 40 %.
   - The decoded *working* form should stay packed (w bytes per page), not canonical 512-bit masks. AND-count on canonical masks is 21.6× slower at w = 2 and 8× slower at w = 16 (§5.4).
3. **AVX-512 is worth a dedicated path, but not for the 256-bit page-mask AND.** The cost is aligning per-page offset bitmaps between two terms.
   - The AVX-512 kernels expand packed bitmaps into page lanes with `VPEXPAND`, using the page mask as the lane mask. They combine the lanes, then compact back with `VPCOMPRESS`: 8 to 64 pages per instruction.
   - In cache this runs 2.7× (narrow) to 3.2× (wide) faster than AVX2. On memory-resident sets the gain shrinks to about 1.0 to 1.5× (noisy), because per-group latency dominates (§5.3, §5.5).
   - AVX2 has no expand, so it runs the scalar BMI page walk. AVX2 ≈ scalar+POPCNT is an honest finding.
4. **tf: use DESIGN.md's L1 + nibble + escape stream, not fixed nibbles.**
   - It costs 1.74 against 4.01 bits per posting (§5.6) and it is exact.
   - Nibble tf saturating at 15 gave recall@10 = 1.000 against exact tf on 240 queries, so exactness costs nothing in quality either way.
   - The codec stores nibbles for its own benchmarks only. The posting module owns the real tf stream.
5. **Block-max bounds: size them in tuples, not pages.**
   - With 256-page group bounds alone, top-k visits 39 to 95 % of groups.
   - With 16-page block bounds it scores 4 % of the candidates on real data. OR(hi,mid) top-10 is then 0.98 ms against 5.8 ms exhaustive (§5.7).
   - On the dense100 scenario, 16 pages are 1,600 tuples and prune poorly: 33 % scored. 4-page blocks get 11 %.
   - **Rule:** pick the block as `clamp(256 / tuples_per_page, 1, 16)` pages, so a block holds at most about 256 tuples: 16 pages (140 tuples) on real data, 2 pages on dense100.
   - Store bounds as u8 per chunk plus a nibble per block relative to it. That is 9 B per group at 16 blocks, and 1 + 128 / 2 B at 2-page blocks, so keep per-block nibbles only for chunks with ≥ 32 postings.
   - Best-first (BMP) order visits about 40 % fewer units than in-order DAAT with the same bounds (simulation, §5.7). The architect's BMP choice holds.
6. **Facets by bitmap popcount are not a general win** (§5.8).
   - They beat per-row doc values only for low-cardinality facets on broad queries: source, 56 values, query df ≥ 64K, 1.26 against 1.44 ms.
   - Doc values (a u16 per tuple slot) are 4 to 30× faster for 245 values or selective queries.
   - Recommend a planner choice by `|q| × nvalues`, and doc values in the docmap for facet columns.
7. **Where ctid bitmaps lose, stated plainly:**
   - On narrow pages (8.8 tuples), CRoaring on dense doc ordinals is 1.1 to 2.9× faster than our bitmaps on 2-term counts, because its containers span 29× more rows. At 100 tuples per page, sift wins nearly every AND/OR/ANDNOT count by 1.6 to 3×.
   - Doc-id EF is 12 % smaller than ctid EF: 8.8 against 10.0 bits per posting.
   - Ctid-keyed Roaring (`blk*512+off`) is 2 to 16× slower than our bitmaps.
   - ctid keying buys "no doc-id map, no renumbering, VM/page alignment". It costs about 1 to 1.2 bits per posting of key-space slack, plus the dense-id SIMD advantage Roaring enjoys.

## 2. The data (`tools/codec/stats`)

| | real | dense32 | dense100 |
|---|---|---|---|
| heap pages | 346,958 | 95,028 | 30,409 |
| tuples / page | 8.76 (35.6 % 4–7, 61.8 % 8–15, 2.6 % 16–31) | 32 | 100 |
| docs / terms / postings | 3,040,892 / 1,201,223 / 134,463,291 | same | same |

Real ctids come from a `COPY` of `ctid` from the clustered, wide (jsonb) table. The dense scenarios remap doc *i* to `(i / tpp, i % tpp + 1)`.

Postings by df class (real data):

| df | terms | postings % | postings / touched page | postings / touched group | groups / term |
|---|---|---|---|---|---|
| 1 | 733,364 | 0.55 | 1.00 | 1.0 | 1 |
| 2–3 | 202,080 | 0.35 | 1.01 | 1.04 | 2.2 |
| 4–15 | 155,012 | 0.83 | 1.01 | 1.05 | 6.9 |
| 16–127 | 78,632 | 2.54 | 1.02 | 1.16 | 38 |
| 128–1K | 23,451 | 6.13 | 1.05 | 1.55 | 227 |
| 1K–8K | 6,745 | 13.86 | 1.10 | 3.49 | 792 |
| 8K–64K | 1,613 | 25.02 | 1.17 | 16.7 | 1,247 |
| 64K–512K | 300 | 34.75 | 1.57 | 116 | 1,345 |
| ≥ 512K | 26 | 15.97 | 2.94 | 609 | 1,356 |

- tf distribution: 1 = 79.4 %, 2 = 12.1 %, 3 = 4.0 %, 4–7 = 3.8 %, 8–15 = 0.7 %, > 15 = 0.1 %.
- Pages where a term covers every tuple: under 1 % of postings except in the top class (4.5 %). Run containers would not pay, so they were dropped.

Even the 26 most frequent terms (df ≥ 17 % of rows) put fewer than 3 postings on a touched page of 8.8 tuples. This density is the root of every size result below.

## 3. Formats

All integers are little endian. A blob starts 4-byte aligned. The key is `blk << 16 | off`, with `off` in 1..512.

### 3.1 Term blob

```
byte 0        kind | flags << 4         kind: 1 = LIST, 2 = GROUPED; flags bit 0 = tf present

LIST     u8 ob                          offset bits: ob = bits(max(off) - 1)
         varint n
         [u8 ub]                        term bound (max per-posting bound), only with tf
         ceil(n / 256) Elias-Fano blocks, each over key x = blk << ob | (off - 1):
           varint x0 - prev_last        prev_last = last key of the previous block (0 first)
           varint u - 1                 u = last - x0 + 1
           lows:  cnt * l bits, l = floor(log2(u / cnt))
           highs: cnt + ((u - 1) >> l) bits, bit (v_i >> l) + i set (v_i = x_i - x0)
         [tf nibbles]

GROUPED  u8 0[3], u32 n, u32 ng
         u32 key[ng]                    gid << 8 | ctype; gid = blk >> 8
         u32 poff[ng + 1]               payload offsets
         u32 base[ng + 1]               posting rank of each group (tf index, exact group counts)
         payload: per group one container
           ctype & 0x80 == 0  BITMAP: u64 pages[4], popcount(pages) * w bytes (bit off-1)
           ctype & 0x80       ARRAY:  u16 (page << 8 | off - 1) if w <= 32, else u32 (page << 16 | off - 1)
           w = ctype & 0x7f   in {1, 2, 4, 8, 16, 24, ..., 64} = width for max(off) in the group
         [tf] u8 ub[ng]; u8 blk[ng][8] (16 nibbles q = ceil(15 * blockmax / ub)); n tf nibbles
```

- **Position stream** (flag `SIFT_TERM_POS`, format 1.1, segments written with `positions = on`; DESIGN.md (g)): the last `pos_len` bytes of the term blob, after the tf nibbles, unaligned, for word terms with df > 2. Terms with df <= 2 keep their records (up to 256 bytes) in the dictionary entry instead.
  ```
  u32 ckpt[ceil(df / 32)]     byte offset in the records of every 32nd posting (rank order)
  record per posting          LEB128 varints: first << 1 | (n > 1) [, n - 2, then n - 1 gaps minus one]
  ```
  `first` and the gaps are token ordinals of the term in the document, `n` is the true occurrence count (tf saturates at 255). A posting of rank r is found through `ckpt[r / 32]` and a skip over at most 31 records; the groups of a GROUPED term and the blocks of a LIST term are not touched, so queries without a phrase read no position bytes.
- **Container choice:** ARRAY when `entries * (2 or 4) < 32 + npages * w`, else BITMAP.
- **Term kind choice:**
  - `sift_encode` picks the smaller of LIST and GROUPED, with a `bias` factor, by default. Shipped policy: `kind = df >= 8192 ? GROUPED : LIST` (65536 in the first analysis), see §4.
  - The encoder reports exact sizes when `out == NULL`, so pages can be planned.

### 3.2 Chunk (DESIGN.md contract 6)

One term × one 256-page group, standalone: `u32 (count << 8 | ctype)` followed by the GROUPED container payload.

- `sift_chunk_count` is O(1).
- `sift_chunk_pagemask` is O(1) for bitmap chunks. For array chunks it is O(entries); arrays are chosen only when smaller than the bitmap, at most 2 B per entry.
- `sift_chunk_offsets` decodes only the selected pages into canonical `u64[8]` offset masks.
- `sift_chunk_bound(npages)` bounds AUTO/BITMAP chunks.
- The posting module can keep its own chunk directory `{sb, off, count, sb_bound, flags}` and use chunks directly. Equivalently, it can use the GROUPED blob, which is the same containers plus a directory.

### 3.3 Decoded group (`sift_grp`): the working form

```
{ gid, w, base, n, pages[4], off }    off = popcount(pages) bitmaps of w bytes in page order
```

- BITMAP containers are viewed zero-copy. ARRAY containers and LIST runs are built into a caller buffer (`SIFT_CUR_BUF` = 32 KB per cursor).
- **Invariant:** every set page has a non-zero bitmap.
- Kernels accept mixed widths. The AND result has `w = min`, OR has `w = max`, and ANDNOT keeps the left operand's width.

## 4. Switching rules and thresholds (from §5)

| Decision | Rule | Evidence |
|---|---|---|
| inline | df ≤ 2 in the dictionary entry (raw keys) | LIST blob for df 2–3 ≈ 53 bits/posting incl. header |
| LIST vs GROUPED | GROUPED iff df ≥ 8K (64K in the analysis below; measured on 218K Wikipedia articles: +10 % index, ranked AND 22 % and count of ORs 27 % faster) | §5.1 policies; §5.3 speed |
| container | ARRAY iff `2·n < 32 + npages·w` (u16 entries) | auto: 7,598 array / 3,478 bitmap groups on real |
| bitmap width | `w = sift_w_for(max off in group)` | real: w1 58 %, w2 40 %, w4 2 % |
| bounds | u8 per chunk + a nibble per block, only for chunks with ≥ 32 postings; block = `clamp(256/tpp,1,16)` pages (the codec implements fixed 16-page blocks) | §5.7 |
| tf | L1 bit + nibble + escape (DESIGN.md (a).2) | §5.6 |
| prefetch | software-prefetch the payload 4 groups ahead in sequential scans | up to +55 % on memory-resident counts in one A/B; exact distance not tunable on the loaded box |

## 5. Measurements

Reproduce with `cd tools/codec && nice make -j4 && make test`, then `make data`, then `bench /tmp/sift-codec <tpp> size|lists|ops|rank|facets|canon` under `flock /tmp/sift-perf.lock taskset -c N`.

### 5.1 Bits per posting by df class

Singletons are excluded: they cost 6 B inline. Columns:
- **list**: EF LIST.
- **grouped**: auto containers, per-group directory included.
- **bitmap**: two-level bitmaps at tight width.
- **tin512**: page mask plus fixed 64 B offset mask per page.
- **ef-docid**: Elias-Fano over dense doc ordinals with no headers, the lower reference for doc-id-mapped engines.
- **pol64K**: GROUPED for df ≥ 64K, else LIST.

**Real data (8.8 tuples/page)**

| df | list | grouped | bitmap | tin512 | ef-docid | pol8K | pol64K |
|---|---|---|---|---|---|---|---|
| 2–3 | 52.97 | 177.9 | 417.4 | 846 | 22.6 | 52.97 | 52.97 |
| 4–15 | 31.62 | 129.1 | 364.5 | 841 | 21.0 | 31.62 | 31.62 |
| 16–127 | 20.41 | 102.7 | 317.0 | 807 | 18.3 | 20.41 | 20.41 |
| 128–1K | 16.22 | 77.9 | 236.3 | 714 | 15.3 | 16.22 | 16.22 |
| 1K–8K | 13.20 | 42.8 | 110.9 | 568 | 12.3 | 13.20 | 13.20 |
| 8K–64K | 10.44 | 20.5 | 33.0 | 457 | 9.4 | 20.50 | 10.44 |
| 64K–512K | 7.66 | 12.7 | 14.0 | 329 | 6.5 | 12.74 | 12.74 |
| ≥ 512K | 5.60 | 7.17 | 7.21 | 175 | 4.4 | 7.17 | 7.17 |
| **all** | **10.01** | 25.9 | 57.0 | 412 | 8.8 | 14.57 | **12.04** |
| total MB | 167 | — | 953 | 6,884 | 148 | 244 | 201 |

**dense32 / dense100 (all > 1)**

| | list | grouped | bitmap | tin512 | pol64K |
|---|---|---|---|---|---|
| dense32 | 8.20 | 19.7 | 44.3 | 288 | 9.82 |
| dense100 | 8.56 | 18.2 | 62.7 | 204 | 11.05 |

For the dense scenarios, the frequent classes are where bitmaps compete:

| ≥ 512K class | list | bitmap |
|---|---|---|
| dense32 | 3.69 | 3.56 |
| dense100 | 4.05 | 4.61 |

**Key-list alternatives** (real, all > 1, bits per posting including term headers):

| encoding | bits/posting |
|---|---|
| Elias-Fano (chosen) | 10.05 |
| PFor-128 (Lucene 9 style) | 10.33 |
| FOR-128 (Lucene 8 style) | 11.35 |
| the original bb+ob delta packing | 11.21 |
| EF over doc ordinals | 8.17 |

Under dense100 ctid EF is only 5 % above doc-id EF (8.62 against 8.17). In dense32 the LIST blob (8.20) even beats the global doc-id EF estimate (8.82): ob = 5 bits exactly covers 32 tuples, and per-block universes adapt to local density.

**Published figures of 1 / 7 / 25 bits per posting do not hold on this corpus.**
- *Frequent* terms: two-level bitmaps give 7.2 (real), 4.6 (dense100) and 3.6 (dense32).
  - About 1 bit per posting needs most tuples of each touched page to contain the term. Our densest class averages 2.94 of 8.8, and the entropy bound for a term at 17 % of rows is about 3.9 bits per posting.
- *Medium* (8K–64K): bitmaps cost 27 to 61 bits, while EF costs 8.9 to 10.4.
- *Rare* (16–127): EF costs 19.5 to 20.4 bits, the same order as the published ~25.
- Those figures are plausible for long documents (Stack Exchange, many terms per row) on dense tables. They are not a property of the format.

### 5.2 Representation of the frequent terms (pol64K)

On real data, auto picked GROUPED for only 0–19 % of terms per class, because LIST is smaller everywhere. The pol64K policy forces GROUPED on 326 terms. Those terms hold 51 % of postings and dominate query time, so the speed is spent where it matters.

### 5.3 Operation throughput

Input Mpostings/s: the sum of the operands' df divided by time, over 64 queries per class. Classes:
- hi: df ≥ 64K
- mid: 1K–64K
- rare: 16–1K

Each cell is the best of 9 trials, maxed over two runs. Column notes:
- **EF list** and **grouped**: best ISA. **grouped** uses auto containers; **bitmap** forces BITMAP containers (fixed-mask style, tight widths).
- **roaring doc-id**: CRoaring 5.2.3 on doc ordinals, including the doc→ctid map for materializing ops. **roaring ctid**: keys `blk*512+off-1`.
- **FOR doc-id**: Lucene-style FOR-128 with skip entries, led by the shortest list.
- **u64 array**: uncompressed sorted keys, galloping.

OR-count and ANDNOT-count use `|a|+|b|−|a∧b|` from stored counts (all engines can do this; Roaring does).

**Real data (8.8 tuples/page)**

| op | class | EF list | grouped | bitmap scalar | bitmap avx2 | bitmap avx512 | avx512/avx2 | roaring doc-id | roaring ctid | FOR doc-id | u64 array |
|---|---|---|---|---|---|---|---|---|---|---|---|
| decode | hi&hi | 106 | 291 | 337 | 330 | 346 | 1.05 | 209 | 315 | 193 | 2052 |
| decode | mid&mid | 80 | 101 | 143 | 136 | 137 | 1.01 | 131 | 51 | 126 | 8007 |
| and | hi&hi | 133 | 658 | 869 | 1037 | 1154 | 1.11 | 1322 | 504 | 177 | 381 |
| and | hi&mid | 177 | 983 | 2375 | 2730 | 2429 | 0.89 | 2908 | 656 | 832 | 940 |
| and | mid&mid | 130 | 237 | 671 | 667 | 640 | 0.96 | 1617 | 187 | 443 | 479 |
| and | hi&rare | 1460 | 15945 | 64225 | 78010 | 81304 | 1.04 | 88039 | 43879 | 32184 | 51580 |
| and | hi&hi&mid | 187 | 1280 | 3361 | 3960 | 3823 | 0.97 | 2713 | 571 | 1566 | 367 |
| and-count | hi&hi | 146 | 971 | 1314 | 2421 | 2872 | 1.19 | 3687 | 746 | 310 | 377 |
| and-count | hi&mid | 198 | 1076 | 2687 | 2307 | 2510 | 1.09 | 4653 | 759 | 1066 | 872 |
| and-count | mid&mid | 155 | 255 | 683 | 766 | 863 | 1.13 | 2119 | 274 | 501 | 561 |
| and-count | hi&rare | 1419 | 16777 | 81014 | 94529 | 95654 | 1.01 | 103630 | 58795 | 36143 | 53162 |
| and-count | hi&hi&mid | 181 | 1291 | 2925 | 2984 | 3583 | 1.20 | 2160 | 437 | 1227 | 316 |
| or | hi&hi | 100 | 215 | 193 | 205 | 283 | 1.38 | 171 | 220 | 111 | 642 |
| or | hi&mid | 122 | 241 | 240 | 286 | 310 | 1.08 | 220 | 282 | 126 | 607 |
| or | mid&mid | 60 | 77 | 89 | 90 | 102 | 1.13 | 124 | 63 | 65 | 573 |
| or | hi&rare | 106 | 281 | 342 | 322 | 336 | 1.04 | 291 | 352 | 166 | 621 |
| or-count | hi&hi | 148 | 848 | 1305 | 2044 | 3075 | 1.50 | 4117 | 861 | 247 | 644 |
| or-count | hi&mid | 176 | 1035 | 2889 | 2143 | 2684 | 1.25 | 5053 | 904 | 335 | 586 |
| or-count | mid&mid | 126 | 237 | 585 | 615 | 463 | 0.75 | 893 | 125 | 300 | 565 |
| or-count | hi&rare | 1397 | 11910 | 63555 | 65521 | 59721 | 0.91 | 92638 | 4087 | 380 | 646 |
| andnot-cnt | hi&hi | 149 | 867 | 1346 | 2015 | 3279 | 1.63 | 3634 | 801 | 252 | 379 |
| andnot-cnt | hi&mid | 180 | 1096 | 3231 | 3777 | 3255 | 0.86 | 4343 | 954 | 1112 | 944 |
| andnot-cnt | mid&mid | 136 | 244 | 648 | 693 | 653 | 0.94 | 2027 | 262 | 445 | 534 |
| andnot-cnt | hi&rare | 1290 | 13600 | 63230 | 69162 | 67532 | 0.98 | 78262 | 3428 | 28926 | 50817 |

**dense100 (100 tuples/page)**

| op | class | EF list | grouped | bitmap scalar | bitmap avx2 | bitmap avx512 | avx512/avx2 | roaring doc-id | roaring ctid | FOR doc-id | u64 array |
|---|---|---|---|---|---|---|---|---|---|---|---|
| decode | hi&hi | 122 | 249 | 368 | 387 | 394 | 1.02 | 169 | 670 | 175 | 1648 |
| decode | mid&mid | 75 | 97 | 173 | 170 | 151 | 0.89 | 120 | 73 | 121 | 7819 |
| and | hi&hi | 165 | 498 | 1438 | 899 | 1804 | 2.01 | 1251 | 1359 | 154 | 338 |
| and | hi&mid | 190 | 741 | 2670 | 5566 | 5482 | 0.98 | 2529 | 1122 | 809 | 901 |
| and | mid&mid | 115 | 186 | 1271 | 1539 | 1011 | 0.66 | 1594 | 235 | 410 | 516 |
| and | hi&rare | 455 | 1917 | 56749 | 57409 | 69313 | 1.21 | 86386 | 52956 | 28126 | 30319 |
| and | hi&hi&mid | 184 | 759 | 5550 | 7320 | 5865 | 0.80 | 2213 | 1139 | 1142 | 336 |
| and-count | hi&hi | 173 | 645 | 2984 | 5183 | 7814 | 1.51 | 2717 | 1671 | 276 | 365 |
| and-count | hi&mid | 196 | 722 | 6414 | 10469 | 6649 | 0.64 | 3600 | 1506 | 1013 | 769 |
| and-count | mid&mid | 120 | 186 | 1293 | 1718 | 1009 | 0.59 | 1028 | 179 | 431 | 483 |
| and-count | hi&rare | 430 | 1835 | 31237 | 67687 | 28575 | 0.42 | 93410 | 58526 | 23072 | 49168 |
| and-count | hi&hi&mid | 180 | 717 | 6210 | 6530 | 4311 | 0.66 | 2343 | 1249 | 1371 | 362 |
| or | hi&hi | 126 | 291 | 399 | 409 | 510 | 1.25 | 192 | 595 | 111 | 648 |
| or | hi&mid | 124 | 270 | 357 | 403 | 406 | 1.01 | 195 | 455 | 83 | 647 |
| or | mid&mid | 68 | 80 | 105 | 113 | 129 | 1.14 | 103 | 126 | 59 | 572 |
| or | hi&rare | 133 | 323 | 452 | 473 | 481 | 1.02 | 185 | 659 | 110 | 652 |
| or-count | hi&hi | 189 | 630 | 2923 | 4998 | 6127 | 1.23 | 3770 | 1818 | 235 | 686 |
| or-count | hi&mid | 197 | 851 | 6730 | 10034 | 6883 | 0.69 | 5150 | 1995 | 382 | 633 |
| or-count | mid&mid | 126 | 201 | 1464 | 1734 | 699 | 0.40 | 613 | 300 | 308 | 436 |
| or-count | hi&rare | 471 | 2012 | 60347 | 67970 | 55261 | 0.81 | 38279 | 5806 | 385 | 655 |
| andnot-cnt | hi&hi | 189 | 621 | 2847 | 5281 | 5440 | 1.03 | 3942 | 1718 | 308 | 383 |
| andnot-cnt | hi&mid | 198 | 761 | 2660 | 9834 | 7116 | 0.72 | 2939 | 1646 | 1130 | 832 |
| andnot-cnt | mid&mid | 123 | 189 | 1167 | 1406 | 1058 | 0.75 | 1710 | 314 | 452 | 492 |
| andnot-cnt | hi&rare | 460 | 2030 | 55388 | 30010 | 53627 | 1.79 | 6949 | 3256 | 3616 | 10567 |

Reading the tables:
- **Bitmaps against EF lists:** bitmaps are 5 to 65× faster on every AND/count. They are 2 to 3× faster on materialized OR, which is output-bound (writing ctids). This is the reason for the df ≥ 64K GROUPED policy.
- **Against doc-id Roaring, dense100:** sift bitmaps win almost every row:
  - AND-count 1.7 to 2.9×, and 3-term AND 3.3×;
  - OR-count 1.6 to 2.8×;
  - ANDNOT-count up to 8×.
  - Roaring wins hi&rare AND probes (1.2 to 1.4×) and ties mid&mid AND/ANDNOT.
- **Against doc-id Roaring, real data (8.8 tuples/page):**
  - Roaring wins 2-term counts by 1.1 to 2.9×. The gap is largest for mid&mid, where groups are sparse. Its containers span 65,536 dense ids, while a sift group spans 2,243 slots, so sift pays far more per-group overhead.
  - Sift wins 3-term AND (1.5 to 1.7×) and materialized OR for frequent terms (1.2 to 1.7×), because it emits ctids directly with no id→ctid map.
- **Against ctid-keyed Roaring and Lucene-style FOR:** sift bitmaps win almost everywhere, by 2 to 16×.
- **ISA effects:**
  - AVX-512 vs AVX2 on these memory-resident sets is 0.4 to 2.6×, mostly about 1.0 to 1.5×. The spread is noise from the shared machine, and the gain is limited by per-group latency.
  - In cache, AVX-512 is consistently 2.7 to 3.2× faster (§5.4, §5.5). Scalar (baseline x86-64, no POPCNT) runs at 0.5 to 1.1× AVX2 on counts.

### 5.4 Packed w-byte bitmaps against canonical 512-bit masks

AND-count of two df ≥ 64K terms, chunk form, in cache, Mpostings/s:

| | packed scalar | packed avx2 | packed avx512 | canon512 avx512 | packed/canon |
|---|---|---|---|---|---|
| real (w = 1–2) | 1,124 | 2,141 | 5,706 | 264 | 21.6× |
| dense100 (w = 16) | 3,087 | 6,893 | 19,046–22,411 | 2,391–2,618 | 8.0–8.6× |

Canonical masks (contract 6) are provided by `sift_chunk_offsets` and `sift_offmask_*` for the scoring kernels. Set operations should run on the packed form.

### 5.5 Memory bandwidth

| measurement | GB/s | Mpostings/s |
|---|---|---|
| single-thread sequential read (AVX-512 popcount over 512 MB) | 23.3–26.9 | — |
| AND-count, 2 frequent terms, bitmap blobs in L2 (868 KB): AVX-512 | 9.5 | 5,386 |
| same, AVX2 | 3.3 | 1,860 |
| same, scalar | 1.7 | 978 |
| AND-count over 64 query pairs (≈ 200 MB, memory-resident): AVX-512 | ≈ 3.5 | ≈ 2,500–3,000 |

- Bitmap blobs of frequent terms average about 1.4 B per posting.
- Memory-resident scans reach about 15 % of one core's streaming bandwidth. They are bound by per-group latency (directory seek, page-mask alignment, about 100 to 300 B of payload per group), not by DRAM bandwidth.
- Software prefetch of the following groups' payload helped by up to +55 % in one A/B, but the gain was not reproducible on the loaded machine.
- Parallel scans over SB morsels (DESIGN.md item 9, `sift.parallel_scan`) were expected to scale nearly linearly until bandwidth is reached; at 218,058 articles the Gather start-up dominates and only unordered scans with a heavy residual qual gained, which is why they are off by default.

### 5.6 tf side stream

Bits per posting:

| df | nibble (4-bit, sat. 15) | L1 + nibble + escape, empty if all tf = 1 |
|---|---|---|
| 2–3 | 4.54 | 0.30 |
| 128–1K | 4.01 | 0.76 |
| 8K–64K | 4.00 | 1.50 |
| ≥ 512K | 4.00 | 3.05 |
| all | 4.01 | **1.74** |

- Quality: nibble-saturated tf gave recall@10 = 1.000 against exact-tf BM25 on all 240 ranking queries, real and dense.
- Decoding cost (profile of fused top-k): computing ranks (`sift_grp_ranks`, a popcount prefix over the term's packed bitmaps) plus gathering tf takes about a third of top-k CPU time. The `vdivps` BM25 kernel takes about 2 %.

### 5.7 Ranked top-10 BM25 (k1 = 1.2, b = 0.75, exact float32, AVX-512)

`sift_topk`:
- runs document-at-a-time over groups;
- skips a group if `Σ w·ub/255 < θ`;
- masks out 16-page blocks whose nibble bounds cannot reach θ, **before** decoding offsets or tf;
- verifies, against exhaustive scoring, that results are bit-identical.

| family (48 queries) | candidates | grouped ms/q | exhaustive ms/q | speedup | candidates scored |
|---|---|---|---|---|---|
| real or(hi,mid) | 240 K | 0.98 | 5.81 | 6.0× | 4.0 % |
| real or(hi,mid,mid) | 188 K | 1.36 | 17.7 | 13.1× | 7.5 % |
| real and(hi,mid) | 624 | 0.30 | 0.40 | 1.3× | 25.6 % |
| real or(hi,hi,hi) | 539 K | 22.7 | 21.1 | 0.9× | 50.4 % |
| dense100 or(hi,mid) | 240 K | 1.64 | 5.85 | 3.6× | 30.1 % |
| dense100 or(hi,hi,hi) | 539 K | 21.5 | 11.4 | 0.5× | 96.3 % |

- With LIST terms ("auto"), top-k is 2 to 7× slower: lists carry only a term bound, so nothing prunes. Terms that rank need GROUPED, or at least per-chunk bounds.
- Offline simulation: docs scored for k = 10 at bound granularity G pages, DAAT order against best-first (BMP):

| | G=256 daat/bmp | G=64 | G=16 | G=4 |
|---|---|---|---|---|
| real or(h,m) | 40.6 / 30.8 % | 14.7 / 9.6 | 4.3 / 2.5 | 1.2 / 0.6 |
| real or(h,m,m) | 62.7 / 55.1 | 24.7 / 18.5 | 7.3 / 4.6 | 1.9 / 1.0 |
| real or(h,h,h) | 96.2 / 94.5 | 80.6 / 74.7 | 45.5 / 38.0 | 12.9 / 9.4 |
| dense100 or(h,m) | 89.8 / 84.5 | 66.6 / 56.9 | 33.3 / 24.3 | 11.2 / 7.0 |
| dense100 or(h,m,m) | 99.8 / 99.1 | 89.9 / 85.6 | 52.7 / 44.9 | 19.3 / 13.8 |

- Exhaustive scoring runs at about 10 to 20 ns per (doc, term) on the loaded box. The AND (`and2`), rank and tf-gather steps cost more than the `vdivps` BM25 kernel. That is why or(h,h,h) does not speed up: the bounds keep 50 % of documents.
- Next step, not done here: fold the block mask into the OR kernel, so masked pages are never merged.

### 5.8 Facet counts

ms per query: 48 queries per row, value bitmaps forced BITMAP. The query column is the query term's df class.

| facet (values) | query | sift bitmap popcount | doc values (u16/slot) | CRoaring doc ids |
|---|---|---|---|---|
| source (56) | df ≥ 64K (avg 216 K) | 1.26 | 1.44 | 0.53 |
| source (56) | 1K–64K (avg 5.8 K) | 0.55 | 0.14 | 0.16 |
| country (245) | df ≥ 64K | 6.59 | 1.45 | 2.83 |
| country (245) | 1K–64K | 3.70 | 0.13 | 1.08 |

## 6. API summary (`src/bm25/sift_bitmap.h`)

- **Dispatch:**
  - `sift_init()` picks the best of scalar (baseline x86-64), AVX2 (avx2, bmi, bmi2, popcnt) and AVX-512 (F/BW/VL/VBMI2/VPOPCNTDQ). It uses `__builtin_cpu_supports` and function-level target pragmas only.
  - `sift_set_isa()` caps the level, for the `sift.simd` GUC.
- **Encode:** `sift_encode(tids, tf, ub, n, opts, out)` with exact-size mode. Also `sift_keylist_encode/decode` and `sift_chunk_encode/count/pagemask/offsets/grp`.
- **Cursors (operator trees):**
  - Sources are `sift_cur_blob` and `sift_cur_tids` (raw or inline keys). Operators are `sift_cur_and` (k-way), `sift_cur_or` (k-way) and `sift_cur_andnot`.
  - Every cursor implements `seek(gid)`, so AND leapfrogs at group level, then by page mask, before decoding offsets.
  - `sift_count` uses the AND-count and OR-count (inclusion-exclusion) kernels without materializing. `sift_tids` emits heap order.
- **Group kernels:** `sift_grp_and/or/andnot`, with an optional page mask on AND for VM/filter/bound masks. Also `sift_grp_and_count`, `sift_grp_count`, `sift_grp_count_mask` (count on all-visible pages) and `sift_grp_tids` (mask, e.g. `~allvisible`, for heap checks).
- **Ranking:**
  - `sift_grp_ranks`: posting ranks for side streams.
  - `sift_grp_score`: fused exact BM25 over a result group, ranks → tf → K(dl) gather → `w·tf/(tf+K)`, bit-identical across ISAs.
  - `sift_topk`: group and 16-page block bounds, plus an optional filter cursor ANDed in the same pass.
- **Facets:** `sift_facets(q, values[], nv, counts)`.
- **Canonical masks:** `sift_offmask_and/or/andnot/popcount`.

## 7. Tests and caveats

- `make test` runs randomized property tests at every supported ISA against sorted-array references. It covers:
  - decode, 2- and 3-way AND/OR/ANDNOT and nested trees;
  - counts, seeks, page-masked count/tids, ranks/tf, group bounds;
  - the chunk API, key lists and canonical mask ops;
  - top-k (bit-exact against exhaustive float scoring, with and without filter) and facets.
  - Edge cases: empty sets, single postings, full pages up to offset 512, blocks up to 2^32 − 1, identical operands, and mixed widths.
  - Latest run: 2000 iterations, about 13 M checks per ISA, 0 failures.
- Not fuzzed yet; libFuzzer is planned for M6. Inputs are trusted index bytes: the decoders validate nothing.
- The machine was shared and overloaded. Treat absolute ns figures as upper bounds. The prefetch distance could not be tuned reliably.
- The corpus has short documents (63 tokens on average). Long-document corpora move the frequent classes toward bitmaps. Re-run `bench size` on the target data before freezing the df threshold.
- The CRoaring comparison uses v5.2.3 with its own runtime dispatch (AVX2/AVX-512). The Lucene-style baseline is my C implementation of FOR-128 with skip entries and a shortest-list-led conjunction, not Lucene itself.
