# Safety findings

Found by the fuzz harnesses of `test/fuzz/` (scripts/fuzz.sh, targets `colchunk`, `bm25`, `bm25-raw`, `shred`) on the v3 snapshot
`f0d985d`, and by two read-only audits of the decoders. Line numbers are of that snapshot. The Status columns were checked against 3.0.2 (a
repro replayed on the fixed code, or the source read); the conversion of the decoders to `SiftSpan` (phase 2, docs/SAFETY.md) is not done. A finding
that crashed a harness is CONFIRMED with a repro; an audit-only one is SUSPECT (not run).

Replay: `nice make -j4 -C test/fuzz OUT=/tmp/x all`, write the hex with `xxd -r -p > f`, run `/tmp/x/TARGET f`. For the UBSan
findings add `UBSAN_OPTIONS=suppressions=/dev/null` (scripts/fuzz.sh passes test/fuzz/ubsan.supp, which hides F-BM25-1).

The default `bm25` target runs the checked mode (`sift_blob_check` first, `FZ_MODE=1`) and is clean. The F-BM25 memory findings were in the
raw mode (`bm25-raw`, opt-in): the query path `sift_post_open` with `verify=false` decodes blobs that `sift_blob_check` never saw. They are fixed
(2.0.2, in 3.0.0); `bm25-raw` is clean since.

## Confirmed

| Id | Severity | Where | Effect | Status at 3.0.2 |
|---|---|---|---|---|
| F-COL-1 | high | src/col/kern.c:54 `lut_u32_scalar`, fed by src/col/ctest.c `chunk_lut` / `cc_lut` | heap over-read: stored FOR32/PACK codes index the LUT without a range check against its size | fixed: 44c17cc (regress inputs 0c032a8) |
| F-BM25-2 | high | src/bm25/bitmap.c:440,452 `gr_seek` (via `sift_cur_blob`) | heap over-read and stack write: the 4-byte group entries give `pg` up to 65535 into `b.pages[4]` and `o` into `c->buf` | fixed: 463951f (2.0.2, in 3.0.0); repro clean |
| F-BM25-3 | high | src/bm25/bitmap.c:463,480 `ld_upto8` / `ef_decode`, from `sift_cur_gids` (src/bm25/posting.c:899 `sift_post_bounds`) | `memcpy` with a negative size (`end` before `p`) | fixed: 463951f (2.0.2, in 3.0.0); repro clean |
| F-BM25-4 | medium | src/bm25/bitmap_kern.h:38,123,230 `sift_ld8` / `popcount_scalar` / `count_mask_scalar` | over-read: a count taken from the blob is not tied to the bytes present | fixed: 463951f (2.0.2, in 3.0.0) |
| F-BM25-5 | medium | src/bm25/kernels_avx512.c:183 `nibbles64`, from src/bm25/posting.c:969 `sift_post_group_tf` | SEGV: the tf nibble stream length (from the dictionary sidecar) is not checked against the blob; only `sift_post_check` does | fixed: 7f59d1a (3.0.0; adad91a in 2.0.3) |
| F-BM25-6 | low | src/bm25/bitmap.c:610 | undefined shift: `blob[1]` (the Rice parameter) is used as a shift count unchecked | fixed: 463951f (2.0.2, in 3.0.0); repro clean |
| F-BM25-1 | low | src/core/varint.h:27 `sift_get_varint` | shift by 70 on a run of continuation bytes; ends only because 64 zero pad bytes follow a blob | fixed: 463951f, 0ddff29 (2.0.2, in 3.0.0); repro clean, no caller of the unbounded `sift_get_varint` is left |
| F-COL-2 | low | src/col/chunk.c, src/col/batch.c | `elog(ERROR)` (XX000, internal error) where ERRCODE_DATA_CORRUPTED belongs: `sift_cchunk_strs`, `unit_emit`, `sift_cunit_decode` | fixed for `unit_emit` and `sift_cunit_decode` (group leaf count): ERRCODE_DATA_CORRUPTED, inputs `colchunk-unit-dict-code` and `colchunk-unit-group-count` in test/fuzz/regress (replayed with `FZ_STRICT=1`). `sift_cchunk_strs`: not reachable from stored bytes (`unit_parse` ties STRS to VAR leaves and ISTR to STR leaves, every caller checks the encoding first), the `elog` stays as a caller precondition |

Repros (the target is chosen by the first byte for `bm25`):

    F-COL-1  colchunk  03000000000120000001000000000000000000000000000100
    F-BM25-2 bm25      0008000000020000001400000001000000a0000000002800000000000000140000003d009600ac00b600c6003f018c010002160002160232029602a602b502110315034104800481049204be04
    F-BM25-3 bm25      02030000002700000009d19c0000002a000000070000006700000000000001092701fd0a00fb2e08a3157e153f2b01030302000132029a02f4ff0c01010202ea02ce027e27010401d0014dfb02000116c802a6028202a0018403ea01
    F-BM25-6 bm25      12ff01ff0101ff     (also 965501000081b7, ea01eaeaea01ea: the first violation of each, the memory errors need the longer inputs)
    F-BM25-1 bm25      00, 104 bytes f0, then 00000000010503ab01d90380491629   (120 bytes)
    F-BM25-4/5: 1.5 KB and 1.3 KB inputs; `scripts/fuzz.sh --scale 10 bm25-raw` found both within minutes on the snapshot

Replayed on the snapshot (`f0d985d`, ASan and UBSan, no suppressions) F-BM25-1, -2, -3 and -6 each stop with the sanitizer report named in the table;
on 3.0.2 (`a9fa19f`) all four inputs run clean, and `bm25-raw` ran 2,370,421 executions in 181 s (libFuzzer, seed 1, seeds of the writers, no
suppressions) without a finding.

F-BM25-1 stopped a fuzz run at the first hit, so scripts/fuzz.sh suppresses it (test/fuzz/ubsan.supp). The fix has landed; the entry there is now
unneeded.

F-COL-1 is fixed: `chunk_lut` gives a table of at most 16 bits a bit for every code the width can hold, and checks the wider codes (FOR32, PACK
above 16 bits, the codec's offsets in `cc_lut`) with `codes_below` before the LUT kernels read them (ERRCODE_DATA_CORRUPTED). The inputs of the
three paths (FOR32, PACK, CC with a valid check word) are in test/fuzz/regress/, replayed by `make -C test/fuzz regress` and first in every
`colchunk` run; scripts/fuzz.sh no longer skips the LUT check. The same check guards the code lanes the aggregate executor
indexes its dictionary tables with (`eval_lut` in src/agg/pred.c, the group maps; P1-6): `sift_cchunk_decode_codes` in `sift_col_read_lane` and
`sift_ccodes_below` in `read_positions` raise ERRCODE_DATA_CORRUPTED for a code above the dictionary size (the tables have one entry more, for NULL).
Regress input: test/fuzz/regress/colchunk-codes-for32 (`colchunk` checks that no code above the dictionary is ever returned).

## Suspect (audit only, no repro run)

| Where | Corruption to try | Effect | Status at 3.0.2 |
|---|---|---|---|
| src/col/scan_unit.c `unit_group`, src/col/batch.c `sift_cunit_decode` | group counts {0xFFFFFFF0, 0x20} with a child chunk n=0x10 | `expand_parents` writes about 16 GB past `parent` (uint32 sum wraps, only the total is checked) | open (source read: the counts are summed in uint32) |
| (fixed, P1-6) src/col/scan_read.c `read_positions` STR | stored code 0x00FFFFFF, 3-entry dictionary, GROUP BY | `map[code]`, `d->off[code]` read out of bounds in src/agg/agg.c | fixed: P1-6, 1c2f4e7 and 39cb7bb |
| src/bm25/posting.c:767 `post_attach` | sidecar varint 1, blob ng 0xFFFFFF | `bloblen - ALIGN4(codec_len)` underflows, `gs[i].flags` read past the blob | fixed in 2.0.2 and 3.0.0 (463951f, 7f59d1a: the codec length and `ng` are checked against the blob at attach) |
| src/bm25/segment.c `load_sb` | extra bits in a DOCMAP offset mask | `walk_docs` writes past `keys` (sift.verify itself overflows before reporting) | not re-checked |
| src/bm25/bitmap.c `sift_chunk_pagemask` | DOCMAP chunk byte0 0xB0, entry 00 00 FF FF | writes `pm[1023]` | open (source read: the page index is not bounded) |
| src/table/wbuf.c `sift_tbl_delta_page` | delta header npages=60000 | `seg->page[1024]` overflow on the first fetch | not re-checked; the function is now `sift_tbl_wbuf_page`, which copies the header's `npages` without a bound |
| src/table/wbuf.c `sift_tbl_page_rec` | slot[0]=0xFFF0 in a delta page | read of `rec->len` outside the page; `sift_tbl_rec_flag` and VACUUM write there | not re-checked |
| src/table/tomb.c `sift_tbl_tomb_sync` | tombstone page nent=0xFFFF | reads 2 MB past the page; a `next` cycle loops forever | not re-checked |
| src/col/codec_parse.c `parse_node` / `sift_cc_open` | 40-byte tree of CONST children, n=16M | 64-128 MB allocations before the row count is compared | not re-checked |
| src/col/seg.c `sift_cseg_unit` | last chunk PACK | `unpack` reads up to 31 bytes past `unitbuf` (no `SIFT_COL_PAD`) | not re-checked |

## Found by the PostgreSQL 17 container of the 3.0 release gate (open at 3.0.2)

Both came from `scripts/fuzz.sh` run in the `postgres:17-bookworm` container (gcc 12.2 and clang 14) while the release gate was being
brought up. The gate's `pg17` job no longer runs the standalone harnesses (they do not touch the server); the `fuzz` job runs them with the
host compiler (clang 23, gcc 16), where neither finding appears.

- **Pointer rebased below its array, src/bm25/bitmap.c:570** (`c->ls.blk - c->ls.bi`, the window of a decoded block indexed with
  absolute positions). clang 14's UBSan reports `index 18446744073709551360 out of bounds for type 'sift_tid[256]'` in the `bm25` target.
  It is undefined behavior in C (a pointer outside its array), benign with the compilers of the host. Reproduced with clang 14.0.6 (bookworm
  container, `bm25` target, the seed corpus alone); fixed: `ls_seek` indexes the window with `i - bi` and keeps the pointer inside the
  array. The input is `test/fuzz/regress/bm25-list-window` (it trips clang 14 only; 90 s of `bm25` under clang 14 are clean since).
- **AVX-512 `range_u32` returns set bits for values outside the range under gcc 12 -O1 with UBSan `pointer-overflow`**
  (theory: a toolchain problem, not proven). The `colchunk` target reports `FINDING: mode 0: SIMD level 2 decodes differently from
  scalar`; the input is `docs/safety/colchunk-gcc12-ubsan-pointer-overflow.bin` (26909 bytes, sha1 26292c50...). Replay, in the
  container with `build-essential postgresql-server-dev-17 liblz4-dev libzstd-dev`:

      make -s -C test/fuzz OUT=/tmp/o /tmp/o/colchunk
      /tmp/o/colchunk docs/safety/colchunk-gcc12-ubsan-pointer-overflow.bin      # FINDING (gcc 12.2, the sanitizers of test/fuzz/Makefile at -O1)
      make -s -C test/fuzz OUT=/tmp/p "SAN=-fsanitize=address,undefined,float-cast-overflow -fno-sanitize=pointer-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer" /tmp/p/colchunk
      /tmp/p/colchunk docs/safety/colchunk-gcc12-ubsan-pointer-overflow.bin      # clean

  What was seen (on the first artifact of this kind, `FZ_TRACE=1` and prints in `nd_filter`): decode and `decode_off32` agree at every
  level, the `filter` step differs; the column is a DELTA node (n = 5000, W = 19, off32), filtered by decoding to a `uint32` array
  and calling `sift_colk->range[2]` (`range_u32_avx512`). The kernel marked values far outside [lo, lo + span] as matches, and
  calling it again on the same array gave a different answer (the first call correct, the next three wrong, 312 bits set), so it reads
  something that is not in its arguments. It happens at -O1 with UBSan or with `pointer-overflow` alone; not at -O0 or -O2, without
  sanitizers, with ASan alone, or with `-fno-sanitize=pointer-overflow`. A 20-line copy of the masked-load pattern
  (`_mm512_maskz_loadu_epi32` + `_mm512_mask_cmple_epu32_mask` in the `BITS512` shape) did not reproduce it. The installcheck SIMD tests
  (`col_kernels_simd`, `col_enc`) pass in that container. The host compiler replays this file clean (clang 23 libFuzzer with ASan and
  UBSan, -O1; ran by hand: the gate's `fuzz` job does not replay it, the file is not in `test/fuzz/regress`). Open at 3.0.2 (planned for 3.0.1, not done): run
  the replay under valgrind and MSan on the masked-load path of `range32v` / `BITS512` (an over-read of the array tail, or an uninitialised
  lane), decide whether it is a gcc 12 miscompile (then `-fno-sanitize=pointer-overflow` for gcc < 13 in test/fuzz/Makefile) or a
  kernel bug, and move the file to `test/fuzz/regress/colchunk-*` so the fuzz job replays it first.

## Other tools

- MSan (clang 23, `make -C test/fuzz CC=clang SAN='-fsanitize=memory ...' DEFS= COLZ=`): `shred` reports inside libpgcommon's `snprintf`
  (uninstrumented, false positive); `colchunk` reports a `memcmp` at src/col/dict.c `cdict_probe` during harness setup, from
  `sift_cdict_intern`: not triaged, may be a harness artifact. `bm25` is clean in checked mode.
- gcc `-Wclobbered` (SIFT_HARDEN=pkg warnings): src/bm25/step.c:107 and src/bm25/pushdown.c:444, locals changed after `PG_TRY`; check
  that they are `volatile` where read in the handler.
- scripts/static.sh: 120 findings (93 clang-tidy, 27 scan-build, 0 from gcc -fanalyzer), accepted as the baseline in scripts/static.baseline.
  54 are `bugprone-implicit-widening-of-multiplication-result` (int product widened to size_t: overflow candidates when the factors are
  stored counts), the rest analyzer reports (8 null dereference, 6 array bound, 4 bitwise shift, 4 call-and-message, 27 scan-build); none
  triaged as a bug yet.
