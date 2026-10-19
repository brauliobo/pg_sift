# Scoring kernels

`src/bm25/sift_kernels.h` is the API; the scalar table in `src/bm25/sift_kernels.c` is the specification. `src/bm25/sift_kernels_avx2.c` and `src/bm25/sift_kernels_avx512.c` must match it bit for bit, including the slots they must leave unchanged. Dispatch follows `sift.simd` through the cpu module's rebind registry; `sift_kernels_for(level)` returns a level's table for tests.

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

# Aggregation operators (sift_aggkern)

`src/agg/sift_aggkern.h` is the contract of every slot, the scalar table in `src/agg/sift_aggkern.c` is the specification, `src/agg/sift_aggkern_avx2.c` and `src/agg/sift_aggkern_avx512.c` must match it bit for bit, the order of the float sum included. Same pattern as the scoring kernels above: `sift_aggk` is the active table and follows `sift.simd` through the cpu module's rebind registry (`sift_aggkern_init`, idempotent), `sift_aggkern_for(level)` returns a level's table for tests (never NULL, a level the CPU lacks falls back downwards), and a slot a level does not speed up points at the scalar function.

The operators work on decoded vectors (at most `SIFT_AGG_VEC` = 4096 rows or array elements of int32, int64, double or uint32 dictionary codes) and `uint64` bitmaps, not on the packed chunks of the column index. Only what is specific to the executor is in this table. The generic operations are slots of the column kernels (`SiftColKernels`, `sift_colk`, docs/COL.md section 8), one implementation each: the integer comparisons and `BETWEEN` are range tests (`range[SIFT_COLW_32]` / `range[SIFT_COLW_64]`: `x op c` is one wrapping range `[lo, hi]`, `x <> c` is `[c + 1, c - 1]`), a null flag array to a bitmap is the u8 range `[1, 255]` (`sift_colk_nonzero`), a dictionary predicate is `lut[SIFT_COLW_32]` over a bit set with one bit per code, `and_w` / `or_w` / `andnot_w` / `popcount_w` are `bits_and` / `bits_or` / `bits_andnot` / `bits_count`, `mask_sel` is `bits_to_idx`, and the gathers are `gather_u32` and `for_i64_sel[SIFT_COLW_64]` with base 0. Their contract is the column table's: a bitmap they write is padded to `SIFT_COL_NWORDS(n)` words (the executor's vectors have `SIFT_AGG_PADW` words of slack), a selection vector has `SIFT_COL_IDXPAD` entries of room after its last position. What stays here is different in kind: `cmp_f64` / `between_f64` (PostgreSQL's float8 order, not the integer keys of the column table), `lut_i32` (a bound check fused with a byte LUT indexed by the value minus a base), `gather_bits` (the row mask of the elements of an array column), the group id, reduction and `count_changes` kernels.

## Conventions

- A bitmap writer of this table writes `ceil(n / 64)` whole words and leaves the bits `>= n` zero; a reader expects them zero. The column kernels write `SIFT_COL_NWORDS(n)` words (a multiple of 8), the bits `>= n` zero too. NULL rows are the caller's: a predicate is ANDed with the not-null bitmap and NULL rows are cleared from a mask before a reduction.
- **Padding.** Every input array (columns, codes, parents, selection vectors, LUTs, maps, bitmaps) is `SIFT_SIMD_ALIGN` aligned and has `SIFT_AGG_PAD` (64) readable bytes after its last element, contents irrelevant: a level loads whole registers at the tail, and the gathers of `lut_i32` read a dword at a byte-indexed LUT. Outputs are written exactly. A lane the data does not name is never dereferenced: the tail of a gather loads its indexes under a lane mask.
- **Scalar loops are the specification.** A bitmap is the bits of `COND(i)` for the rows; a reduction visits the selected rows in row order.

## Semantics

| Operator | Result |
|---|---|
| `cmp_f64` | bit `i` = `v[i] op c`, `op` one of `= <> < <= > >=` |
| `between_f64` | bit `i` = `lo <= v[i] <= hi`; `lo > hi` selects nothing |
| `lut_i32` | bit `i` = `(uint32) (v[i] - base) < nlut && lut[(uint32) (v[i] - base)] != 0` |
| `gather_bits` | bit `e` of `out` (`m` bits) = bit `parent[e]` of `rowmask`: the row mask of the elements of an array column |
| `gid_code`, `gid_i32` | `gid[i] += map[key] * mult` in `uint32` arithmetic; `key` = `code[i]`, or `(uint32) (v[i] - base)` |
| `sum_i32` | exact `int64` sum of the selected rows |
| `sum_f64` | eight accumulators, see below |
| `minmax_*` | minimum and maximum of the selected rows; false (outputs untouched) when none is selected |
| `count_gid`, `sum_gid_*` | `cnt[gid[i]] += 1`, `acc[gid[i]] += v[i]` over the selected rows in row order |
| `count_changes` | the number of selected rows whose value differs from the previous selected row's (the one before the first is `*prev`); `*prev` ends at the last selected value |

**Doubles follow PostgreSQL's float8 order**: `-Inf < finite < +Inf < NaN`, NaN equals NaN, `-0` equals `+0`. SIMD compares against a constant that is not NaN map to one predicate each: `=` `EQ_OQ`, `<>` `NEQ_UQ`, `<` `LT_OQ`, `<=` `LE_OQ`, `>` `NLE_UQ`, `>=` `NLT_UQ` (an ordered predicate is false for a NaN row, "not <=" and "not <" are true: NaN is the greatest). In `between_f64` a NaN row fails the upper bound, so `>= lo` is the plain ordered `GE_OQ`. A NaN constant (a bound or the right side of a comparison) is rare and takes the scalar function in every level. Float compares of the scalar table are `f8_cmp`, PostgreSQL's `float8_cmp_internal`.

**The float sum has one order for every level** (a replica on another CPU answers the same): eight accumulators `a[0..7]` start at `+0.0`; selected row `i` is added to `a[i % 8]` in row order; the result is `((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7))`. Unselected rows add nothing; the SIMD levels add `+0.0` for them, which is the same thing because an accumulator is never `-0.0` (it starts at `+0.0`, and a round-to-nearest sum is `-0` only for `-0 + -0`). A NaN result (a NaN row, or `Inf - Inf`) is always the canonical quiet NaN `0x7FF8000000000000`: the payload that an x86 add keeps depends on the operand order the compiler picked, so no level may promise it. Both vector levels keep the eight accumulators in two ymm registers.

**min / max of doubles** use the same order and normalize the result: a zero result is `+0` (PostgreSQL's `float8smaller` keeps the first-seen zero; the two differ only for a column that holds both signs of zero), a NaN result is the canonical NaN. The minimum is NaN only if every selected row is NaN, the maximum if any is. The vector levels exploit that `MINPD` / `MAXPD` return their second operand for a NaN: with the running value second, a NaN row never enters it, NaN rows are noted on the side, and "every row was NaN" is `lo > hi` (the running minimum never left `+Inf`, the maximum never left `-Inf`).

## Design choices (measured)

AMD Ryzen 7 9700X (Zen 5), single thread pinned to one core, GCC 16, `-O2 -march=x86-64 -ffp-contract=off`, best of 200 batches, under heavy unrelated host load (load average 17 to 40 on 16 threads, other agents' builds and benchmarks; the perf lock was not taken for these short runs): treat absolute numbers as lower bounds. The same code measured twice varies by about 10 to 15 %, so read 0.9x to 1.1x as "equal". Ns per call over one vector of 4096 rows.

- **Where the factor comes from.** Predicates and the bitmap kernels are compare-and-mask-extract: the scalar loop is 0.4 to 0.8 ns per row, AVX2 handles 4 to 32 rows per instruction (MOVMSK), AVX-512 writes the compare mask straight into a mask register. Reductions go 3x to 25x faster unmasked, 2x to 10x under a 50 % mask.
- **Gathers do not beat scalar loads on Zen 5.** `VPGATHERDD` / `VPGATHERQQ` take about as long as the loads they replace. Measured in four runs: AVX2 `gather_u32` 0.8x to 1.1x, AVX2 `gid_code` / `gid_i32` 0.9x to 1.2x, AVX2 and AVX-512 `gather_u64` 0.6x to 1.5x: those slots are the scalar functions. AVX-512 `gather_u32` (1.2x to 1.7x in those runs, 0.8x in the latest: it depends on the sibling thread; now `SiftColKernels.gather_u32`) and `gid_code` / `gid_i32` (1.2x to 1.4x) are kept. `lut` (the column table's), `lut_i32` and `gather_bits` are gathers too but replace a load plus a test plus a store of a bit, hence their 2x. A CPU with fast gathers (recent Intel server parts) would do better; not measured here.
- **`count_gid`, `sum_gid_*`** are per-group scatters: row order is the contract (a float group sum is sequential), the only exact speedups would need the group count, which the contract does not carry. Every level shares the scalar loop (the 0.8x to 1.1x in the table is noise).
- **`bits_to_idx`** (the selection vector of a bitmap; the column table's slot, it was this table's `mask_sel`). One expansion per nonzero word costs about 4 ns (AVX2: eight byte-table stores; AVX-512: one `VPCOMPRESSB` and masked stores of the positions), a bit-by-bit walk about 0.45 ns per set bit plus a mispredicted loop exit. A sparse word is faster walked: up to 8 bits on AVX2 (`SIFT_COLK_SEL_SPARSE_AVX2`), up to 2 on AVX-512 (`SIFT_COLK_SEL_SPARSE_AVX512`), where the compress beats the walk from a handful of bits on (10 % mask: 74 ns compressed against 136 ns with an 8 bit walk). The AVX2 level stores whole vectors, so the output has `SIFT_COL_IDXPAD` entries of room after the last position; the AVX-512 level is exact. `VPCOMPRESSB` of the 64 bit positions is 15 % faster than four `VPCOMPRESSD` of 16 row numbers on dense words. Designs measured on masks of 1 / 10 / 50 / 100 % density (scalar walk: 50 / 193 / 944 / 1900; the candidates of `tools/aggkern/alt.c` are in the history of this file):

| bits_to_idx design | 1 % | 10 % | 50 % | 100 % |
|---|---|---|---|---|
| AVX2, byte table for every nonzero word | 123 | 248 | 262 | 260 |
| AVX2, words with <= 2 / 4 / 8 / 16 bits walked | 45 / 35 / 35 / 35 | 256 / 214 / 168 / 154 | 245 / 246 / 265 / 280 | 248 / 252 / 239 / 242 |
| AVX-512, `VPCOMPRESSD`, every nonzero word | 124 | 285 | 289 | 283 |
| AVX-512, `VPCOMPRESSD`, words with <= 2 / 4 / 8 / 16 bits walked | 35 / 33 / 34 / 34 | 275 / 216 / 151 / 178 | 275 / 284 / 286 / 290 | 281 / 276 / 283 / 278 |
| AVX-512, `VPCOMPRESSB`, every nonzero word | 111 | 249 | 248 | 250 |
| **kept, AVX2: byte table, <= 8 bits walked** | 41 | 156 | 219 | 216 |
| **kept, AVX-512: `VPCOMPRESSB`, exact masked stores, <= 2 bits walked** | 42 | 74 | 94 | 256 |

- **Float sum.** The eight accumulators are eight chains of `n / 8` dependent adds, so the sum is latency bound whatever the level: 4096 rows are 512 adds of 2 cycles. Plain adds on two ymm accumulators reach that bound (AVX2 236, AVX-512 225 ns, 7x scalar). A single zmm accumulator with a merging masked add (the first AVX-512 version) took 393 ns, a merging masked add on ymm 421 ns: the masked forms have a longer latency. A zeroing masked load (`VMOVUPD ymm{k}{z}`) is slower than a plain load, so the unmasked path uses plain loads and only a mask or the tail uses the masked ones. Selecting the lanes of a 50 % mask, AVX-512, ns: zeroing masked load 427, plain load + zeroing move 566, plain load + AND with a 16-entry lane table 406; the same table is what AVX2 uses (a broadcast + AND + compare per nibble was 528).
- **Reductions need independent accumulators.** `min` / `max` over int64 and double are a compare, blend or merging min per vector on one chain: the first versions ran at the latency of that chain (all rows, ns: AVX2 `minmax_i64` 820, AVX2 `minmax_f64` 808, AVX-512 `minmax_f64` 511). Four accumulator pairs over 16 (AVX2) or 32 (AVX-512) rows a step brought them to 290, 460 and 233. An unmasked vector needs no mask handling at all (a plain `VPMINSQ`), only the tail and masked vectors do.
- **`count_changes`.** The first version carried the last selected value in a vector from one group of rows to the next (permute, align, compare), a dependency chain of several instructions per group: 535 ns (AVX2) and 639 ns (AVX-512) for all rows, 756 / 618 ns at 30 %. Now the selected values are compacted into a buffer behind the previous value (`VPERMD` with the position table, `VPCOMPRESSD`) and every value is compared with its predecessor there, and without a mask the rows are compared with themselves shifted by one: 436 / 203 ns all rows, 605 / 371 ns at 30 %, 579 / 349 ns at 50 %.
- **Where the SIMD level is not much faster than scalar:** a sparse mask in `bits_to_idx` and `count_changes` (the walk is proportional to the selected rows, the vector code to the words), the gathers above, `count_gid` / `sum_gid_*`.

## Throughput

Ns per call, one vector of 4096 rows (a bitmap of 64 words), same host and conditions as above; "50 %" is a random mask selecting half of the rows, "all rows" is `mask == NULL`. The slots that are the scalar function at a level (see above) show the noise of the measurement. The operators that moved to the column table (range tests, `lut`, bitmap operations, `bits_to_idx`, gathers) are in docs/COL.md section 8.

| operator | scalar | avx2 | x | avx512 | x |
|---|---|---|---|---|---|
| cmp_f64 < | 3338 | 305 | 10.9x | 181 | 18.4x |
| cmp_f64 > | 2637 | 265 | 9.9x | 182 | 14.5x |
| between_f64 | 4616 | 426 | 10.8x | 314 | 14.7x |
| lut_i32 (1000 entries) | 2802 | 1269 | 2.2x | 1154 | 2.4x |
| gather_bits (4 elements per row) | 2201 | 1242 | 1.8x | 1135 | 1.9x |
| gid_code | 1624 | 1820 | 0.9x | 1361 | 1.2x |
| gid_i32 | 1844 | 1958 | 0.9x | 1338 | 1.4x |
| sum_i32 all rows | 1083 | 333 | 3.3x | 157 | 6.9x |
| sum_i32 50 % | 895 | 334 | 2.7x | 168 | 5.3x |
| sum_f64 all rows | 1612 | 236 | 6.8x | 225 | 7.2x |
| sum_f64 50 % | 958 | 466 | 2.1x | 350 | 2.7x |
| minmax_i32 all rows | 2780 | 212 | 13.1x | 108 | 25.7x |
| minmax_i32 50 % | 1362 | 294 | 4.6x | 137 | 9.9x |
| minmax_i64 all rows | 2759 | 290 | 9.5x | 225 | 12.3x |
| minmax_i64 50 % | 1369 | 633 | 2.2x | 314 | 4.4x |
| minmax_f64 all rows | 3728 | 460 | 8.1x | 233 | 16.0x |
| minmax_f64 50 % | 2107 | 873 | 2.4x | 535 | 3.9x |
| count_gid all rows (64 groups) | 1262 | 1114 | 1.1x | 1392 | 0.9x |
| count_gid 50 % | 1095 | 1296 | 0.8x | 1153 | 0.9x |
| sum_gid_i32 50 % | 1202 | 1102 | 1.1x | 1137 | 1.1x |
| sum_gid_f64 50 % | 1250 | 1201 | 1.0x | 1188 | 1.1x |
| count_changes all rows (10 % change) | 1810 | 436 | 4.2x | 203 | 8.9x |
| count_changes 30 % | 716 | 605 | 1.2x | 371 | 1.9x |
| count_changes 50 % | 1334 | 579 | 2.3x | 349 | 3.8x |

`tools/aggkern/bench` prints this table (`make bench`).

## Verification

- `tools/aggkern/test`: every slot of every level against scalar, bit for bit (`make test N=20`, N thousand random cases per thread plus a grid of every length 0..300 and every multiple of 64 and its neighbours up to 4096). A case (`src/agg/sift_aggkern_cases.h`) draws the data per type: int32 and int64 small, full range and extremes (`INT_MIN`, `INT_MAX`, around 2^31 and 2^32); doubles from normal values, small integers with both zeros, `NaN` (several payloads, signalling), `Inf`, subnormals and the extremes; dictionary codes uniform, in runs, sorted and at the top of the LUT; masks empty, full, 1 % to 99 %, in runs, one bit, alternating; thresholds at the data, at its neighbours and at the type's bounds. The padding after every input is random garbage and every output is poisoned before the call. The inputs are allocated at their exact size, so the ASan build (`make SANITIZE=1 test`, with UBSan) catches a read past the padding or a write past an output. The scalar table is also checked against values written out by hand (float order, zeros, NaN, the sum order, `count_changes`).
- `tools/aggkern/bench`: the table above (`make bench`) and the rejected float sum selections of `tools/aggkern/alt.c`. The moved operators are benchmarked by `tools/kernels` (`make bench-col`) and tested by the `col_kernels_simd` regression test.
- `aggkern_simd` regression test (`test/sql/aggkern_simd.sql`, needs `SIFT_TESTING`): the same random cases inside the backend; the digest of every operator's output is the same at `sift.simd` = scalar, avx2, avx512 and auto, and every level the CPU supports equals scalar case by case.

# Overlaps between the kernel families

The families are `SiftColKernels` (`sift_colk`, the generic table: bit vectors, selections, integer columns), `SiftAggKernels` (`sift_aggk`, the executor's operators), `SiftShredKernels` (`sift_shred_kern`), `SiftCcKernels` (`sift_cck`, the chunk codec) and the scoring and bitmap tables of the BM25 index. A slot is shared when two families need the same function; two slots that compute the same thing from different inputs are not an overlap until one can be written with the other without losing speed. The rule: the shared slot may be at most 3 percent slower than the specialised one at every level, otherwise the specialised one stays. `tools/kernels` `make bench-shred` measures the compositions (best of 300 batches, 4096 elements and 4 Mi elements, pinned core, the composition gets the padding it needs, which the shredder's arrays do not have); ratio = composition / specialised kernel, 4096 elements, 90 percent `VALUE`, one run on a busy host (the verdict does not depend on the noise: the ratios that decide are 1.1 or more).

| specialised kernel | composition from the shared slots | scalar | AVX2 | AVX-512 | decision |
|---|---|---|---|---|---|
| `SiftShredKernels.state_bits` | `range[SIFT_COLW_8]` twice (`state != VALUE`, `state == OVERFLOW`) | 1.09 | 1.49 | 0.71 | kept: scalar and AVX2 lose; at 4 Mi elements all levels lose (1.07, 1.47, 1.38) |
| `SiftShredKernels.state_counts` | `range[SIFT_COLW_8]` and `bits_count` per state | 0.99 (100 % `VALUE`: 6.1) | 1.39 | 0.73 | kept: no histogram slot, three passes over the bytes |
| `SiftShredKernels.minmax_i64` | `range[SIFT_COLW_8]` and `SiftAggKernels.minmax_i64` under that bitmap | 1.53 | 0.61 | 1.03 | kept: scalar loses 1.5x (the bitmap walk against one pass) |
| `SiftShredKernels.minmax_f64` | the same with `minmax_f64` | 2.13 | 1.14 | 1.26 | kept: loses at every level |
| `SiftColKernels.expand_parents` | `SiftCcKernels.rle_expand32` of 0, 1, 2, ... after turning the ends into lengths (runs of 4) | 1.16 | 0.57 | 0.93 | kept: scalar loses 4 to 28 percent (runs of 64 to 1) |
| `SiftCcKernels.rle_expand32` | ends from the lengths, `expand_parents`, `gather_u32` of the values | 2.29 | 5.88 | 4.42 | kept: the gather is a second pass (scalar at AVX2) |

What is shared already: `expand_parents` (the shredder, the column scan and the aggregation executor call the column table's), the range tests, LUTs, `bits_*`, `bits_to_idx`, `gather_u32` and `for_i64*` (the codec and the executor call `sift_colk`), the position table of `bits_to_idx` (also `count_changes`), `sift_col_f8_key` (the codec's `alp_keys_*` and the encoder), `bits_count` (the bitmap module's popcount at each level).

Not an overlap, or not one to remove now:

- `SiftColKernels.unpack` and `SiftCcKernels.unpack32` are different layouts (sequential `PACK`, vertical `FORBIT`); `PACK` is still the in-memory form (`sift_col_enc.c`), so the core slot goes only when it is retired (COL-CODEC.md). `unpack_range32` is the fused unpack and range test of the vertical layout, which the core's slots cannot compose.
- `SiftCcKernels.max32` is an unsigned maximum without a mask; `SiftAggKernels.minmax_i32` is signed and masked.
- `vop` and `and2` of the BM25 bitmap module (`sift_bitmap_kern.h`) and `bits_and`, `bits_or`, `bits_andnot` of the column table are the same words operation (`o = a op b` against `dst op= src`). The bitmap module is in `portable.list` (the SQLite and MariaDB hosts build it) and the column table is in `portable-col.list`, so the BM25 index cannot call `sift_colk` until the column track moves to `portable.list`; the column table's `bits_count` already uses the bitmap module's popcount in the other direction.
- `expand_parents` stores a register of the row ordinal at the start of each row and the next row overwrites the tail, like `rle_expand32` (its output has `SIFT_COL_PAD`): the contract is room for `SIFT_COL_PAD` bytes past `end[np - 1]`, which the aggregation executor, the column scan and the shredder test allocate. `bench shred`, ns for 4096 elements, best of two runs on a busy host, before and after: runs of 1 AVX2 3016 and 1833, AVX-512 3052 and 1958; runs of 4 970 and 457, 726 and 513; runs of 16 259 and 178, 178 and 117; runs of 64 120 and 103, 137 and 77; scalar is unchanged. The comparison with `rle_expand32` above was made with the old, exact kernel.
