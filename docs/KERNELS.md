# Scoring kernels

`src/sift_kernels.h` is the API; the scalar table in `src/sift_kernels.c` is the specification. `src/sift_kernels_avx2.c` and `src/sift_kernels_avx512.c` must match it bit for bit, including the slots they must leave unchanged. Dispatch follows `sift.simd` through the cpu module's rebind registry; `sift_kernels_for(level)` returns a level's table for tests.

## Formulas and evaluation order

All arithmetic is IEEE float32, round to nearest, no FMA (`-ffp-contract=off`), in exactly this order:

| Step | Expression | Where |
|---|---|---|
| per query | `c0 = k1 * (1 - b)`, `c1 = (k1 * b) / avgdl` | `sift_kern_bm25_init` |
| per term | `idf = (float) ln(1 + (N - df + 0.5) / (df + 0.5))` (double) | `sift_kern_idf` |
| per term | `w = (boost * idf) * (k1 + 1)` | `sift_kern_term_weight` |
| per document | `K = c0 + c1 * (float) dl` | `knorm` |
| per pair | `s = w * ((float) tf / ((float) tf + K))` | `bm25_accum` |
| per pair | `score = score + s`, terms in the caller's order | `bm25_accum` |
| dismax | `score = score > other ? score : other` (MAXPS operand order) | `max_merge` |

`K` uses precomputed `c0`, `c1` (one multiply and one add per document) instead of `k1 * (1 - b + b * dl / avgdl)` (a division per document). Both are float approximations of the same real number; this one is cheaper and is what the SQL reference must evaluate. `test/sql/kernels_simd.sql` checks the kernels against the float4 SQL expression `w * (tf / (tf + ((k1 * (1 - b)) + ((k1 * b) / avgdl) * dl)))` on 129 k pairs.

**No NaN or Inf** for valid input: `tf >= 1`, `k1 >= 0`, `0 <= b <= 1`, `avgdl > 0`, finite `w`. Then `tf + K >= 1`, `0 < x <= 1`; with `avgdl >= 2^-48` (one non-empty document among 2^48) and `k1 <= 1000` (reloption maximum), `K <= 1.8e22`.

**Accuracy.** Against the same formula in exact arithmetic (from the same float inputs), one term's float32 score is within 7u (`u = 2^-24`): `K` carries at most 4 roundings, `tf + K` one more, the quotient one, the product one. Exhaustively over tf 0..255, dl 0..65535 and 210 (k1, b, avgdl) combinations including the extremes, the measured maximum is 4.74u. A sum of n terms adds at most `(n - 1)u`.

**tf stream** (DESIGN.md (a).2), term-wide with per-chunk checkpoints `SiftTfPos {rank, nib, esc}`: L1 bit `r` set when posting `r` has `tf > 1`; a nibble `min(tf - 2, 15)` per L1 member, low nibble first; an escape byte `min(tf, 255) - 17` per nibble 15. Decoding saturates the escape (`min(e + 17, 255)`), so corrupt bytes never produce tf 0.

## Bounds

`sift_kern_bound_u8(ref, tf, dl) = clamp(ceil(255 * tf / (tf + K_ref(dl))), 1, 255)`, evaluated in double from the segment's `k1, b, avgdl_ref`. (DESIGN.md (a).5 writes `max w / (k1 + 1)`; the UB formula there and `sift_posting.h` both need `bound / 255 >= x`, which is what this computes.)

At query time `sift_kern_ub_weight(w, avgdl_now, avgdl_ref)` returns `W = w * c / 255 * (1 + 2^-12)` rounded up to float, `c = max(1, avgdl_now / avgdl_ref)`, and `ub_accum` sums `W_t * bound_t`. Pass the same float avgdl values used in `sift_kern_bm25_init`.

Why the block UB is never below a float32 score it covers, for n <= 256 terms:
- exact: `x(avgdl_now) <= c * x(avgdl_ref) <= c * bound / 255` (DESIGN.md (a).5 scaling argument; ceil);
- the float score is at most `sum w_t x_t * (1 + 7u) * (1 + u)^(n-1)`;
- the float UB is at least `sum w_t c bound_t / 255 * (1 + 2^-12) * (1 - u)^n`;
- `(1 + 2^-12)(1 - u)^256 >= (1 + 7u)(1 + u)^255` holds with 8x margin (`2^-12 = 2.4e-4` against `519u = 3.1e-5`).

A plan can have more leaves than a query names (every distinct term of a phrase is one, up to 256 x 128), so `sift_kern_ub_weight` takes the leaf count and raises the slack to `n * 2^-20` above 256 terms (the same `(1 + slack)(1 - u)^n >= (1 + 7u)(1 + u)^(n-1)` argument, again with 8x margin); the rounding of n additions of one term all going the same way is the worst case of the float32 sum.

The harness checks this on 10 M random documents with 1..256 terms, and on up to 32768 copies of one term, `avgdl_now / avgdl_ref` in {0.5, 1, 1.0001, 3}.

**Ties.** Top-k ranks by score descending, then key ascending. A candidate equal to the worst kept score can still enter, so `sift_kern_topk_theta` returns `nextafterf(worst, -Inf)` and `select_gt` keeps `score > theta`, i.e. `score >= worst`. Block pruning must likewise stop only when `UB < theta_score` (strictly), not `UB <= theta_score`, or an equal-score lower ctid in a later block is missed.

## Layout contracts

- Page runs: `npages` consecutive pages per call, mask `uint64[npages * 8]`, float arrays `npages * 512` slots, `SIFT_SIMD_ALIGN` aligned. `select_gt` returns slot numbers `page * 512 + off - 1` (u16, so npages <= 128); its outputs need `npages * 512` entries.
- Padding: tf arrays, tf streams and tf_decode outputs need `SIFT_KERN_PAD` (64) bytes after their last used byte. dl rows need no padding (AVX-512 uses masked loads; AVX2 shifts a window that ends at the highest offset in use).
- `bm25_accum(tf = NULL)` means every tf is 1 (chunks flagged `SIFT_GS_ALL_TF1`): no tf placement, 1.7x faster.
- No allocation, no state; the only globals are the dispatch tables and the const lookup tables.

## Design choices (measured)

Zen 5 (Ryzen 7 9700X), single thread, GCC 16, `-O2 -march=x86-64 -ffp-contract=off`, best of 300 batches under heavy unrelated host load (load average ~47 on 16 threads): treat absolute numbers as lower bounds. A block is 16 pages of 30 tuples; a term matches each tuple with probability d.

bm25_accum, ns per 16-page call (pairs per call in parentheses):

| Design | d = 1 (480) | d = 0.25 (122) | d = 0.03 (10) | 291 tup/pg, d = 0.5 (2319) |
|---|---|---|---|---|
| scalar (reference) | 538 | 187 | 84 | 2591 |
| AVX2: PSHUFB expand table, 8-lane ymm | 166 | 125 | 33 | 1264 |
| **AVX-512: PSHUFB table, 16-lane zmm, chunk bitmap (kept)** | **104** | **101** | 42 | **803** |
| AVX-512 with VPEXPANDB tf placement (first version) | 172 | 156 | 108 | 1633 |
| AVX-512 with VPEXPANDD after widening | 181 | 169 | 86 | 1735 |
| AVX-512VL 8-lane ymm chunks, VPEXPANDB | 317 | 260 | 102 | 2951 |
| compress knorm, dense divide, expand back | 671 | 647 | 631 | 1155 |
| kept kernel, one call per page | 164 | 185 | 116 | 925 |
| kept kernel, `tf = NULL` (all tf 1) | 63 | 63 | 23 | 437 |

- **tf placement:** VPEXPANDB (register form; GCC folds a plain load into the microcoded memory form, so the load is pinned in a register) costs about 20 cycles per 16-lane chunk on Zen 5. Two 8-lane PSHUFB rows from the 2 KB expand table, the second offset by the popcount of the first, are 2x faster.
- **Lane width** (both with VPEXPANDB): 16-lane zmm is 1.7x to 1.8x faster than 8-lane ymm, equal on sparse blocks; the scalar mask walk is amortized over twice the lanes.
- **Compress / dense divide / expand:** fewer divisions on sparse masks, but three passes and the compress/expand traffic lose everywhere except very wide pages.
- **Page runs:** one call per 16-page block instead of per page is 1.15x to 2.8x faster (mask walk and call overhead), hence the `npages` API.
- **Mask walk:** nonzero 16-slot chunks come from VPTESTMW over two pages (AVX-512) and nonzero bytes from PCMPEQB/PMOVMSKB per page (AVX2), visited as one bit loop; the first version tested words one by one and was 2x (tf) to 4x (tf = NULL) slower on sparse blocks.

Other kernels, ns per 16-page call, 30 tuples per page, d = 1 (480 slots) / 291 tuples, d = 0.5 (2319 slots):

| Kernel | scalar | AVX2 | AVX-512 |
|---|---|---|---|
| knorm | 374 / 1728 | 118 / 787 | 50 / 366 |
| max_merge | 268 / 1112 | 88 / 708 | 47 / 401 |
| select_gt, 50 % pass | 390 / 1470 | 114 / 1056 | 74 / 621 |
| select_gt, 1 % pass | 330 / 1541 | 72 / 607 | 56 / 339 |

| Kernel | scalar | AVX2 | AVX-512 |
|---|---|---|---|
| tf_decode, M postings/s, P(tf>1) = 0.25 / 0.75 / 0.02 | 580 / 490 / 1220 | 4970 / 5990 / 6550 | 21500 / 20800 / 23100 |
| ub_accum, 16 bounds (one SB) / 4096, ns | 8.6 / 1597 | 3.7 / 229 | 4.3 / 207 |
| expand (pdep, 8 words) / vm_allvisible (one SB), ns | 122 / 12 | 11 / 4.2 | 11 / 4.6 |

**Memory bandwidth.** Measured on the same loaded host: 26.8 GB/s single-thread DRAM read, 24.6 GB/s with 8 threads (the host's other load shares it). At 4.6 G pairs/s per core (AVX-512, dense block) the scoring input from the index is about 3 bits per pair (≈2 bits tf stream + ≈1 bit codec postings), ≈1.7 GB/s: the kernels are compute-bound, about 15x below a core's DRAM bandwidth, and the floor is IEEE division throughput. tf_decode at 21 G postings/s reads 5.5 to 11 GB/s of stream, within a core's bandwidth.

**Not done:** an FMA-based correctly rounded division (Markstein) could replace `vdivps` bit-exactly, but needs a proof for every input and FMA on the scalar side; intrinsic-level 512-bit vs 256-bit preference on Intel server parts was not measurable here (Zen 5 only).

## Verification

- `tools/kernels/test`: random equivalence of every table against scalar (`make test N=5`), the exhaustive tf x dl grid, double-precision error, bound coverage, tf stream round trips at random positions and piece sizes, top-k against a sort. ASan + UBSan: `make SANITIZE=1 test`.
- `tools/kernels/bench`: throughput per level and the rejected designs (`tools/kernels/alt.c`).
- `kernels_simd` regress test (`test/sql/kernels_simd.sql`, needs `SIFT_TESTING`): the same digest at every `sift.simd` level, per-case equivalence of every supported level, the float4 SQL formula, bound spot checks.
