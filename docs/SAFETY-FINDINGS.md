# Safety findings

Found by the fuzz harnesses of `test/fuzz/` (scripts/fuzz.sh, targets `colchunk`, `bm25`, `bm25-raw`, `shred`) on the v3 snapshot
`f0d985d`, and by two read-only audits of the decoders. Nothing here is fixed: converting the code is phase 2 (docs/SAFETY.md). Line
numbers are of that snapshot. A finding that crashed a harness is CONFIRMED with a repro; an audit-only one is SUSPECT (not run).

Replay: `nice make -j4 -C test/fuzz OUT=/tmp/x all`, write the hex with `xxd -r -p > f`, run `/tmp/x/TARGET f`. For the UBSan
findings add `UBSAN_OPTIONS=suppressions=/dev/null` (scripts/fuzz.sh passes test/fuzz/ubsan.supp, which hides F-BM25-1).

The default `bm25` target runs the checked mode (`sift_blob_check` first, `FZ_MODE=1`) and is clean. The F-BM25 memory findings are in the
raw mode (`bm25-raw`, opt-in): the query path `sift_post_open` with `verify=false` decodes blobs that `sift_blob_check` never saw.

## Confirmed

| Id | Severity | Where | Effect |
|---|---|---|---|
| F-COL-1 | high | src/sift_col_kern.c:54 `lut_u32_scalar`, fed by src/sift_col_enc.c `chunk_lut` / `cc_lut` | heap over-read: stored FOR32/PACK codes index the LUT without a range check against its size |
| F-BM25-2 | high | src/sift_bitmap.c:440,452 `gr_seek` (via `sift_cur_blob`) | heap over-read and stack write: the 4-byte group entries give `pg` up to 65535 into `b.pages[4]` and `o` into `c->buf` |
| F-BM25-3 | high | src/sift_bitmap.c:463,480 `ld_upto8` / `ef_decode`, from `sift_cur_gids` (src/sift_posting.c:899 `sift_post_bounds`) | `memcpy` with a negative size (`end` before `p`) |
| F-BM25-4 | medium | src/sift_bitmap_kern.h:38,123,230 `sift_ld8` / `popcount_scalar` / `count_mask_scalar` | over-read: a count taken from the blob is not tied to the bytes present |
| F-BM25-5 | medium | src/sift_kernels_avx512.c:183 `nibbles64`, from src/sift_posting.c:969 `sift_post_group_tf` | SEGV: the tf nibble stream length (from the dictionary sidecar) is not checked against the blob; only `sift_post_check` does |
| F-BM25-6 | low | src/sift_bitmap.c:610 | undefined shift: `blob[1]` (the Rice parameter) is used as a shift count unchecked |
| F-BM25-1 | low | src/sift_varint.h:27 `sift_get_varint` | shift by 70 on a run of continuation bytes; ends only because 64 zero pad bytes follow a blob |
| F-COL-2 | low | src/sift_col_enc.c:920, 2025, 2071 | `elog(ERROR)` (XX000, internal error) where ERRCODE_DATA_CORRUPTED belongs: `sift_cchunk_strs`, `unit_emit`, `sift_cunit_decode` |

Repros (the target is chosen by the first byte for `bm25`):

    F-COL-1  colchunk  03000000000120000001000000000000000000000000000100
    F-BM25-2 bm25      0008000000020000001400000001000000a0000000002800000000000000140000003d009600ac00b600c6003f018c010002160002160232029602a602b502110315034104800481049204be04
    F-BM25-3 bm25      02030000002700000009d19c0000002a000000070000006700000000000001092701fd0a00fb2e08a3157e153f2b01030302000132029a02f4ff0c01010202ea02ce027e27010401d0014dfb02000116c802a6028202a0018403ea01
    F-BM25-6 bm25      12ff01ff0101ff     (also 965501000081b7, ea01eaeaea01ea: the first violation of each, the memory errors need the longer inputs)
    F-BM25-1 bm25      00, 104 bytes f0, then 00000000010503ab01d90380491629   (120 bytes)
    F-BM25-4/5: 1.5 KB and 1.3 KB inputs; `scripts/fuzz.sh --scale 10 bm25-raw` finds both within minutes

F-BM25-1 stops a fuzz run at the first hit, so scripts/fuzz.sh suppresses it (test/fuzz/ubsan.supp); delete the entry when the fix lands.

F-COL-1 is fixed: `chunk_lut` gives a table of at most 16 bits a bit for every code the width can hold, and checks the wider codes (FOR32, PACK
above 16 bits, the codec's offsets in `cc_lut`) with `codes_below` before the LUT kernels read them (ERRCODE_DATA_CORRUPTED). The inputs of the
three paths (FOR32, PACK, CC with a valid check word) are in test/fuzz/regress/, replayed by `make -C test/fuzz regress` and first in every
`colchunk` run; scripts/fuzz.sh no longer skips the LUT check. Not the same finding: `eval_lut` in src/sift_agg.c feeds the LUT kernel with the
codes of a source vector; see the `read_positions` row of the suspect list.

## Suspect (audit only, no repro run)

| Where | Corruption to try | Effect |
|---|---|---|
| src/sift_col_scan.c `unit_group`, src/sift_col_enc.c `sift_cunit_decode` | group counts {0xFFFFFFF0, 0x20} with a child chunk n=0x10 | `expand_parents` writes about 16 GB past `parent` (uint32 sum wraps, only the total is checked) |
| src/sift_col_scan.c `read_positions` STR | stored code 0x00FFFFFF, 3-entry dictionary, GROUP BY | `map[code]`, `d->off[code]` read out of bounds in src/sift_agg.c |
| src/sift_posting.c:767 `post_attach` | sidecar varint 1, blob ng 0xFFFFFF | `bloblen - ALIGN4(codec_len)` underflows, `gs[i].flags` read past the blob |
| src/sift_segment.c `load_sb` | extra bits in a DOCMAP offset mask | `walk_docs` writes past `keys` (sift.verify itself overflows before reporting) |
| src/sift_bitmap.c `sift_chunk_pagemask` | DOCMAP chunk byte0 0xB0, entry 00 00 FF FF | writes `pm[1023]` |
| src/sift_table_store.c `sift_tbl_delta_page` | delta header npages=60000 | `seg->page[1024]` overflow on the first fetch |
| src/sift_table_store.c `sift_tbl_page_rec` | slot[0]=0xFFF0 in a delta page | read of `rec->len` outside the page; `sift_tbl_rec_flag` and VACUUM write there |
| src/sift_table_store.c `sift_tbl_tomb_sync` | tombstone page nent=0xFFFF | reads 2 MB past the page; a `next` cycle loops forever |
| src/sift_colcodec.c `parse_node` / `sift_cc_open` | 40-byte tree of CONST children, n=16M | 64-128 MB allocations before the row count is compared |
| src/sift_col_seg.c `sift_cseg_unit` | last chunk PACK | `unpack` reads up to 31 bytes past `unitbuf` (no `SIFT_COL_PAD`) |

## Other tools

- MSan (clang 23, `make -C test/fuzz CC=clang SAN='-fsanitize=memory ...' DEFS= COLZ=`): `shred` reports inside libpgcommon's `snprintf`
  (uninstrumented, false positive); `colchunk` reports a `memcmp` at src/sift_col_enc.c:254 `cdict_probe` during harness setup, from
  `sift_cdict_intern`: not triaged, may be a harness artifact. `bm25` is clean in checked mode.
- gcc `-Wclobbered` (SIFT_HARDEN=pkg warnings): src/sift_maint.c:253 and src/sift_pushdown.c:444, locals changed after `PG_TRY`; check
  that they are `volatile` where read in the handler.
- scripts/static.sh: 120 findings (93 clang-tidy, 27 scan-build, 0 from gcc -fanalyzer), accepted as the baseline in scripts/static.baseline.
  54 are `bugprone-implicit-widening-of-multiplication-result` (int product widened to size_t: overflow candidates when the factors are
  stored counts), the rest analyzer reports (8 null dereference, 6 array bound, 4 bitwise shift, 4 call-and-message, 27 scan-build); none
  triaged as a bug yet.
