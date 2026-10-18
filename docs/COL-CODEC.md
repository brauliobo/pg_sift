# Chunk codec interface (`src/sift_colcodec.[ch]`)

The codec compresses and reads **one column chunk**: the values of one leaf for the rows (or, for a child of a group, the elements) of one unit
(`docs/COL.md` 2.2). It serves both users of the column engine, the index AM `sift_col` and the table AM `sift_table`: it sees keys and bytes, never
pages, a heap, a snapshot or a relation. Why these encodings, the measurements and the cost rule are in `docs/perf/COMPRESSION.md`; this file is the
contract the engine (L1, `sift_col_enc.[ch]`) is written against.

Files: `sift_colcodec.[ch]` (writer, reader, policy, string and bit blobs), `sift_colcodec_kern.c` (scalar kernels = the specification, dispatch),
`sift_colcodec_avx2.c`, `sift_colcodec_avx512.c`, `sift_colcodec_vec.h` (the unpacking written once with vector extensions), `sift_colcodec_int.h` (layout
helpers shared by those), `sift_colz.[ch]` (the LZ4 / zstd interface and its PostgreSQL implementation), `sift_colcodec_test.c` (`sift._test_cc_*`),
`tools/colcodec/` (standalone test, benchmark and the research harness).

Portability: the codec includes `sift_host.h`, the C library and its own headers only, and reaches LZ4 and zstd through `sift_colz.h`, a link-time interface
like the page store (`sift_colz.c` in PostgreSQL). It is therefore portable in the sense of `src/portable.list` but **not listed there yet**: SQLite and
MariaDB compile every listed file and have no `sift_colz` implementation. When the portable column engine starts calling the codec, list the codec files
and add a `colz.c` to `sqlite/host` and `mariadb/host` (`sift_colz_available()` returning false and the three other functions never reached is enough:
`auto` then writes lightweight chunks only, and a chunk that needs a layer fails with `ERRCODE_FEATURE_NOT_SUPPORTED`).

## 1. What the codec owns, what the engine keeps

| the engine keeps | the codec does |
|---|---|
| NULL bitmap, overflow bitmap (`SIFT_COLC_NULLS`, `SIFT_COLC_OVF`) in the chunk header | gets the NULL bits only to choose fillers for the free slots |
| zone maps (`ZONES` arrays: min, max, sum, nnull, nvals, set, flags), never inside a chunk | returns `min`/`max` of the non-NULL keys from its statistics pass (`SiftCcInfo`) so the writer need not scan again; nothing else |
| the key domain (`INT` as is, `F4`/`F8` order keys, `STR` dictionary codes), dictionaries (first-seen order) | `kind` tells it a key is a float (for decimal detection); codes are plain integers |
| unit record, row directory, chunk offsets and lengths | one self-describing blob per chunk; the engine stores its length |
| the choice of policy per leaf (the `compression` option) | parses it (`sift_cc_policy_parse`, `sift_cc_policy_for_column`) and applies it |

**Chunk header.** The engine's chunk header (`u8 enc, u8 bits, u8 flags, u8 pad, u32 n, i64 base`, then the bitmaps, then the payload) is kept. A chunk
written by the codec has `enc = SIFT_COLENC_CC` (4); `bits` and `base` are 0 (the blob holds its own frame of reference and `min`/`max`); the payload
is the blob. **The blob must start 8-byte aligned and its stored length is a multiple of 8** (the writer produces such lengths; `sift_cc_open` refuses
others): align the payload in the chunk, or copy it to a palloc'd buffer. The engine's `CONST`, `FOR` and `PACK` encodings (minor 0) stay readable as
written; the writer stops producing `FOR` and `PACK` because the codec's FORBYTE (same bytes as `FOR`) and FORBIT (vertical layout, SIMD-friendly,
replaces `PACK`) are supersets, and `RLE` (3, reserved) is the codec's RLE node. `sift_cchunk_parse` must accept `enc = 4`; the decode,
`sift_ctest_run` and `sift_cchunk_decode(_sel)` paths dispatch on `enc` to `sift_cc_open` + `sift_cc_filter` / `sift_cc_decode(_sel)` for it.

## 2. Writing

```c
SiftCcPolicy pol;  const char *err = sift_cc_policy_for_column(reloption, colname, &pol);   /* NULL = ok; else ereport the message */
StringInfoData out;  SiftCcInfo info;
sift_cc_encode(&out, SIFT_CC_INT|F4|F8, keys, n, nulls, &pol, &info);    /* appends one blob; keys[] int64, nulls[] 64-bit words or NULL */
```

- `keys[i]` for a NULL slot is irrelevant (the writer replaces it by the previous non-NULL key, a leading run by the first). Decode returns that filler:
  **the value of a NULL slot is unspecified**; the engine masks with its NULL bitmap (filters return bits for the filler too, the engine ANDs the non-NULL bits).
- `info.min`/`info.max` feed the zone; `info.bytes`, `info.tag`/`info.inner` feed `sift.col_index_info` (encodings per column) and EXPLAIN.
- The policy string is the engine's reloption `compression` (one value for the index/table, or `default; col=policy; ...`, see section 6). `none` gives
  frame-of-reference arrays at 1, 2, 4 or 8 bytes: the engine's own `range[]` kernels filter them in place after subtracting the base.
- The result is a pure function of (kind, keys, nulls, policy) and of the compression library versions; it does not depend on the SIMD level or on
  the process (a replica rebuilding a merge produces the same bytes it would on any CPU). Tested: the bytes are compared at every level.
- An explicit `lz4` / `zstd` policy on a build without that library is `ERRCODE_FEATURE_NOT_SUPPORTED` at the first encode (with a hint naming `SIFT_NO_LZ4` /
  `SIFT_NO_ZSTD`); `auto` writes with what the build has.
- Memory: `StringInfo`, one `palloc` arena per call proportional to `n` (about 40 bytes per value at the peak of the cascade search). `n <= SIFT_CC_MAXROWS` (16 Mi).

**Other blobs.** `sift_cc_encode_strs` / `sift_cc_decode_strs` store a dictionary (lengths cascade + bytes wrapped under the cold rule; `off[n+1]` and
`bytes` come back in the shape of `SiftCDict`, `bytes` NUL-terminated). `sift_cc_encode_bits` / `sift_cc_decode_bits` store a bit vector (NULL or liveness bits
of a unit: constant, raw, or the positions of the minority bit as an integer cascade). The unit record's `rowstart[]`, `maxoff[]` and `SBDIR` arrays are
integer arrays: `sift_cc_encode(SIFT_CC_INT, ...)` makes them 4 to 20 times smaller with one call each (cumulative offsets are delta chunks); not done by
the codec itself. Both blobs start 8-aligned like chunks.

## 3. Reading

```c
SiftCc *c = sift_cc_open(mcxt, payload, len, verify);   /* ERROR on a bad blob; verify = check word too (first read from storage, col_verify) */
sift_cc_filter(c, lo, hi, bits);        /* key in [lo, hi], signed; (n + 63) / 64 words; the engine ANDs the non-NULL bits (and NRANGE complements) */
sift_cc_decode(c, int64 *dst);  sift_cc_decode_sel(c, idx, m, dst);  sift_cc_get(c, row);  sift_cc_sum(c, bits, &sum);
```

- Buffers: `dst` of `sift_cc_decode` holds `n + SIFT_CC_PAD / 8` int64, of `sift_cc_decode_off32` `SIFT_CC_CAP32(n)` u32 (`sift_colcodec_int.h`: whole rows of 32
  plus the pad, the kernels write whole vectors); `bits` holds `(n + 63) / 64` words, bits past `n` are zero. Input arrays of the kernels are readable
  `SIFT_CC_PAD` bytes past their end.
- `sift_cc_open` validates everything it can in O(size of the headers) plus one pass over the small arrays (run lengths sum to `n`, dictionary sorted,
  exception positions ascending, code widths, decimal digit bounds) and decompresses wrapper nodes into its own memory context under `mcxt`. After it,
  every operation is memory safe on the bytes seen (tested by the corruption fuzz under ASan/UBSan). A bit flip inside packed words that keeps all
  structure valid yields other values, not a crash: that is what `verify` (the 32-bit check word) and the page checksums are for. `sift_cc_close` frees it.
- A scan that is pruned by the zone map never opens a chunk. A chunk is opened once per unit per query; `sift_cc_filter` may be called several times (one per
  predicate on the leaf) and `sift_cc_decode` once for the projection.
- `sift_cc_filter` on FORBIT is the fused unpack + range kernel (no values written), on FORBYTE of 1, 2, 4 and 8 bytes the in-place range kernel, on CONST and RLE it
  touches no value array, on DICT it maps `[lo, hi]` to a code range by binary search of the sorted dictionary and filters the codes, on ALP it maps the
  key range to a digit range by the decode function (monotone) and filters the digits, then tests the exceptions; DELTA and the odd FORBYTE widths decode
  into a scratch array and run the range kernel. A range outside `[min, max]` returns at once. `NRANGE`, `NONNULL` and LUT tests (dictionary-code sets)
  stay the engine's (`lut[]` kernels need the codes: `sift_cc_decode_off32` returns them as `u32` offsets from `min` in row order).
- `sift_cc_decode_off32` is the cheapest decode (no 64-bit widening) when `max - min < 2^32` (header flag, it returns false otherwise), which is every
  dictionary-code chunk and most value chunks.
- `sift_cc_get` is O(1) for FORBIT, FORBYTE, CONST, DICT; O(log runs) RLE; up to 1 023 additions DELTA; O(log exceptions) ALP. Wrapper nodes are unwrapped by
  `sift_cc_open`, so `get` on an opened chunk costs the same. The visibility fallback calls it on an opened chunk.
- Errors: `ERRCODE_DATA_CORRUPTED` "invalid column chunk: ..." (the engine may wrap it as `ERRCODE_INDEX_CORRUPTED` with its context);
  `ERRCODE_FEATURE_NOT_SUPPORTED` for an algorithm id or tag this build does not know or provide (a build without zstd reading a zstd chunk, or a newer
  format version).

## 4. Kernels and dispatch

`SiftCcKernels` (`sift_colcodec.h`) follows `docs/KERNELS.md` and `docs/COL.md` section 8: the scalar table (`sift_colcodec_kern.c`) is the specification,
the AVX2 and AVX-512 tables are bit-identical, `sift_cc_kernels_for(level)` never returns NULL (a level the CPU lacks falls back downwards), `sift_cck` is
the active table and `sift_cc_kernels_init()` registers the rebind with `sift_cpu_register` (called from `_PG_init`, so `sift.simd` selects the level).
The slots: `unpack32`, `unpack_range32` (fused), `range[4]` (u8 to u64), `widen_add64`, `prefix32/64`, `gather32/64`, `max32`, `alp_keys_f8/f4`,
`rle_expand32`. The vertical bit-packing is written once (`sift_colcodec_vec.h`) and compiled at 256 and 512 bits under the level's target options.

**Overlap to resolve at integration** (DRY): `SiftCcKernels.range[4]` is the same operation as `SiftColKernels.range[4]` and `gather32` resembles
`SiftColKernels.gather_u32`; the codec carries its own only because `sift_col_kern.h` is not on the base it was written against. When the core is merged,
`range[]` calls in `sift_colcodec.c` go through `sift_colk->range[]` and the codec's copies (scalar, AVX2, AVX-512) are deleted. `unpack` of the core
(sequential `PACK` layout, bits 1..31) and the codec's `unpack32` (vertical layout, bits 0..32) are different layouts of the same idea; with `PACK` retired
the core's slot can be removed. `sift_cc_f8_key` equals `sift_col_f8_key`; the tool test asserts it once both are on one branch and the codec can switch to
the core's then.

## 5. Integration patch (the chunk writer; nothing of the chunk format is rewritten)

1. `sift_col_enc.h`: `#define SIFT_COLENC_CC 4`; `sift_cchunk_parse` accepts it (payload = blob aligned to 8, `bits`/`base` ignored).
2. Writer (`sift_cunit_encode`, per leaf): after gathering `keys[]` and the NULL bits, `sift_cc_encode(out, kind, keys, n, nulls, &pol, &info)`; the chunk
   header gets `enc = 4`, the existing flags and bitmaps; zone `min`/`max` may be taken from `info`. Keep the shortcut of the all-NULL leaf (a `CONST` chunk
   with the bitmap) and `n == 0`.
3. Readers: `sift_ctest_run` -> `sift_cc_open` + `sift_cc_filter` + AND non-NULL; `sift_cchunk_decode` -> `sift_cc_decode` (NULL slots are unspecified:
   mask them); `sift_cchunk_decode_sel` -> `sift_cc_decode_sel`; `sift_cchunk_zone` (verification C4) -> decode and recompute.
4. Options: register the string reloption `compression` (default `auto`) next to `paths`; parse it once with `sift_cc_policy_for_column` per leaf at
   writer creation (`SiftColDef` path or attribute name is the key); a change of the option applies to chunks written afterwards (merge rewrites).
5. Dictionaries: `sift_cdict_serialize` output goes through `sift_cc_encode_strs` when the dictionary exceeds a few hundred bytes; the DICTS section
   records which form.
6. `sift.col_index_info` reports the encodings per leaf from `SiftCcInfo.tag`. (`sift_cc_kernels_init()` is already called from `_PG_init` by this branch.)
7. Portable hosts: list the codec in `portable.list` and add the `colz.c` stubs (header of this file).

This branch depends on nothing of the engine. The shared files it touches are `Makefile` (library probes), `src/sift_handler.c` (one line in `_PG_init`),
`pg_sift_test--1.0.sql`, `CHANGELOG.md` and the packaging recipes (build dependencies on the LZ4 and zstd development packages).

## 6. The `compression` option

`auto` (default) | `none` | `lz4[:N]` | `zstd[:N]` | encodings `for`, `delta`, `rle`, `dict`, `alp`, `scale` joined by `+`, optionally ending in an algorithm (`for+zstd:9`).
Per column: `compression = 'auto; note=zstd:9; qty=for'` (the first entry without `=` is the default, then `column=policy` pairs; a column without an entry
takes the default, no default means `auto`). Semantics and the rule `auto` applies are in `docs/perf/COMPRESSION.md` 5.3 and 5.4. `none` is also what the
table AM can use for a hot, write-heavy row group. Build options: `make SIFT_NO_LZ4=1` / `SIFT_NO_ZSTD=1` leave a library out (the Makefile otherwise links
each one it finds; the packages depend on both).

## 7. Tests and tools

- `make -C tools/colcodec SANITIZE=1 test [N=2]`: round trips of 20 data shapes (constants, ramps, 64-bit extremes, runs, low cardinality, outliers, decimal
  floats with NaN, infinities and -0.0, float4, ranges at 2^32, microsecond timestamps, steps of 2^40) at the sizes around every block boundary, with and without NULL slots, under 18 policies;
  decode, `get`, `decode_sel`, `decode_off32`, filters and sums against the plain values; every operation and the written bytes at every SIMD level; every
  kernel at every level against the scalar table on exact-size buffers; string lists and bit vectors; the corruption fuzz (about 50 000 damaged blobs).
- `make -C tools/colcodec bench`: ratios per policy on the generated tables and per-level speeds (`docs/perf/COMPRESSION.md` section 7).
- Regression tests `colcodec_basic` (chooser, round trips, filters, errors of the option and of damaged chunks) and `colcodec_simd` (digests equal at every
  `sift.simd` level, per-level equivalence, in-backend corruption fuzz) with the `sift._test_cc_*` functions.
