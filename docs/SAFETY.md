# Memory safety in C for pg_sift

The column engine and the BM25 readers decode bytes from disk. Rust-style safety in C means: every byte read goes through a length-carrying
type (`SiftSpan`, src/sift_span.h), arithmetic on stored sizes is checked, the compiler inserts what it can, and tools find the rest.
Findings so far: docs/SAFETY-FINDINGS.md.

## Threat model

| Input | Example | Layers that catch it |
|---|---|---|
| Corrupt or malicious segment bytes (disk fault, hostile restore) | a length, count, offset or code that points outside its buffer | Span (at the read), `sift_blob_check` / sift.verify (structure, offline), fuzzers (before release), trap UBSan and FORTIFY (at run time, abort instead of exploit) |
| Hostile sizes (OOM) | `n` = 16M rows in a 40-byte header, allocation before validation | Span-first order (validate, then allocate), `MaxAllocSize`, per-open decompression budget |
| Concurrent modification | a page changes between two reads of one header | copy the header once into a local, then parse the copy; buffer locks (not a C-language issue) |
| Own bugs | off-by-one in a SIMD tail, use after free in a memory context | ASan/UBSan CI, fuzzers, Valgrind, PG debug build, static analysis |

A data file an attacker can write also owns the server (PostgreSQL trusts its data directory); the goal is corruption becomes an `ERROR`
(ERRCODE_DATA_CORRUPTED), never a wild read or write. Layers do not replace each other: Span checks what the code asks, ASan sees heap and
stack but not an overrun inside one palloc chunk, trap UBSan sees array bounds of known size and pointer wrap, fuzzers find what nobody thought of.

## Ranking (safety gain / cost)

Instruction overhead by cpu_gate (below); "pkg" means shipped in distribution packages.

| # | Measure | Gain | Cost | Where |
|---|---|---|---|---|
| 1 | `SiftSpan` for every stored offset/length; validate before allocate | closes the P1/P2 class of the audits | ~0 on parse paths, to be measured per converted decoder | src/sift_span.h, phase 2 |
| 2 | Fuzz every decoder, corpus from tests, differential vs scalar | 8 findings, 3 high | CI minutes | test/fuzz, scripts/fuzz.sh |
| 3 | `SIFT_HARDEN=pkg`: stack protector strong, stack clash, CET/branch protection, strict-flex-arrays=3, FORTIFY_SOURCE=3, relro/now | stack smash, libc misuse, ROP | 0.0 to +0.3% | pkg |
| 4 | `-ftrivial-auto-var-init=zero` (`SIFT_HARDEN=init`) | uninitialized stack and padding leaks | +0.1 to +1.9% | CI hardened; pkg optional |
| 5 | Trap UBSan `bounds,shift,nonnull,returns-nonnull,vla-bound` | flexible/array overruns, bad shifts | +1 to +5% (bounds), +2 to +4% (shift set) | CI hardened |
| 6 | `object-size` trap | pointer-to-object overruns | +3 to +8% | CI hardened |
| 7 | `pointer-overflow` trap | pointer wrap | +9 to +36% | CI only, consider dropping |
| 8 | Static analysis (scripts/static.sh) | shallow bugs, widening multiplies | none at run time | CI `static`, non-blocking |
| 9 | Structural verifier (sift.verify) sharing the Span code | offline detection | none at run time | phase 2 |
| 10 | CBMC on Span and the chunk header parser | proof for small code | tool not installed here | later |
| 11 | clang `-fbounds-safety` | compiler-enforced bounds | blocked | not now |

## Measured overhead

`bench/steps/cpu_gate.sh`, user-mode instructions per operation (8 rounds, 20k rows, 95% intervals under 1%), build with and without; wall
time was not used (host load average 20 to 37). BM25 cells are bulk, merge and the query classes count, rank, and, or, phrase. Column-scan
cells are 5 pushdown queries (bench/cpu-query.sh, 100 queries, 5 rounds). The single checks ran on top of `init`.

| Flag set | BM25 cells | Column scan | Verdict |
|---|---|---|---|
| `pkg` | 0.0 to +0.3% (bulk +0.3) | 1.000 to 1.002 | pkg: on |
| `init` | +0.1 to +1.9% (merge +1.9, bulk +1.1) | +0.1 to +0.4% | CI; packages optional |
| `1` (all trap checks) | +13 to +45% (bulk +45, merge +38) | +14.5% to +62% (ts_group +62, filter_count +40) | CI hardened only |
| `bounds` alone | +1.0 to +4.8% | not run | off by default |
| `shift,nonnull,returns-nonnull,vla-bound` | +0 to +3.7% | not run | off by default |
| `object-size` | +2.7 to +8.1% | not run | off by default |
| `pointer-overflow` | +9 to +36% (bulk +36, merge +27) | not run | off by default |
| ASan+UBSan (SANITIZE=1) | not gated | not gated | CI sanitize flavor |

Flags above 1% on hot loops are off by default and on in the `hardened` flavor. `signed-integer-overflow` is not in the trap set: PG and sift
build with `-fwrapv`, so it is defined behavior. The Span itself is not measured yet (no decoder uses it): phase 2 measures each converted
decoder with the same cells; the checks are on header parses, not inner loops. Reproduce: `bench/steps/cpu_gate.sh --env LABEL=SIFT_HARDEN=1`.

## What each knob is

`SIFT_HARDEN=pkg|init|1` (scripts/harden.mk, included by the Makefile) probes every flag with the host compiler and drops unknown ones.
`SIFT_UBSAN_SET="bounds shift"` (or comma list) picks the trap checks. scripts/pgtest.sh passes both. Others: `-Wvla -Walloca -Warray-bounds=2
-Wstringop-overflow=4 -Wformat=2 -Wundef` in `pkg`. `-Wconversion` (474 hits) and `-Wsign-conversion` (1306) are too noisy to enable;
`-Wnull-dereference` (36) and `-Wclobbered` (2, see the findings) are worth a one-time read.
CI (scripts/ci.sh): `hardened` (scripts/hardened.sh: private copy built with `SIFT_HARDEN=1`, 16 decoder regress tests, all pass), `static`,
`span-test` (sqlite/tests/span_test.c with ASan/UBSan, SIMD=none), `fuzz` (scripts/fuzz.sh).

## Tools on this host (gcc 16.2, clang 23.1)

Installed: clang-tidy, scan-build, gcc -fanalyzer, libFuzzer, MSan and UBSan-minimal runtimes, valgrind 3.25, perf. Not installed: cppcheck
(scripts/static.sh skips it), CBMC, ESBMC, Frama-C, CodeQL, AFL++. Coverity Scan accepts open-source projects (needs registration; a decision).
- clang `-fbounds-safety` (`__counted_by`, `__sized_by`, `__indexable`): clang 23 here only has `-Xclang -fexperimental-bounds-safety`, and no
  `<ptrcheck.h>`; a one-function probe did not build. Upstream marks it work in progress; Apple's clang ships it. Not usable for the Debian/RPM/AUR
  toolchains. `__counted_by` on pointers is a source-annotation to keep ready.
- `counted_by` on flexible array members: gcc 15+ and clang 18+; with `-fsanitize=bounds` the access traps (verified with gcc 16). Applies to 5
  sites only (src/sift_page.h x3, src/sift_parallel.h, src/sift_col_enc.c); the pgsql-hackers thread "Add counted_by attribute" is the
  PostgreSQL-side attempt (it found write-order problems under bounds checking). Low value for sift: decoders mostly walk byte buffers.
- PostgreSQL aids: `--enable-cassert` (implies MEMORY_CONTEXT_CHECKING and CLOBBER_FREED_MEMORY, which catch palloc overruns that ASan cannot),
  `-DRANDOMIZE_ALLOCATED_MEMORY`, `-DUSE_VALGRIND` (palloc annotations; needed for useful Valgrind output). CI should run `regress` on an
  assert build with these. pg_buffercache shows pages in use during tests; amcheck is the model for sift.verify.
- MSan: workable for the standalone fuzz targets only if every dependency is built instrumented (zstd/lz4/libpgcommon are not): needs
  `DEFS= COLZ=` and yields false positives in libc wrappers. Valgrind: run the gcc driver build (`FZ_DRIVER=1`, no ASan) for non-ASan hosts.
- MTE (arm64) and CHERI: hardware tag/capability checks; mention only, no toolchain in the distro targets.
- Rust: not recommended now. pgrx is real but would replace the C ABI of the access methods and the portable core shared with SQLite and MariaDB;
  possible later for a new standalone decoder crate behind a C ABI and `cbindgen`, with the Span semantics as its contract.

## Packages

Debian/RPM/AUR builds use `SIFT_HARDEN=pkg` (FORTIFY needs -O2; aarch64 gets `-mbranch-protection` instead of CET). `init` and above stay off.

## Phase 2: conversion list (32 entries)

P1 out-of-bounds from stored data, P2 unbounded loop or allocation, P3 correct hand-written checks to move onto Span, P4 hygiene. Each item:
raw offset arithmetic becomes `sift_span_sub/array/take/read_*`, validate before allocate, raise DATA_CORRUPTED.

| Id | File:function | Gap and replacement |
|---|---|---|
| P1-1 | sift_bitmap.c:sift_cur_blob, gr_seek, ls_seek, ef_hdr, ef_decode, ls_at | query path decodes unchecked blobs (F-BM25-2/3/4): cursor over a Span, per-group sub-span, lazy check_grouped rules |
| P1-2 | sift_bitmap.c:sift_chunk_pagemask/offsets/count (called by sift_segment.c:load_sb) | no length argument; pass `sift_span_sub(rec,8,h[1])`, require w in {1,2,4,8..64}, pg<256 |
| P1-3 | sift_segment.c:load_sb, walk_docs, live_sb_bits | ndocs from mask popcounts never compared with the header; `keys[]` overflow |
| P1-4 | sift_col_scan.c:unit_group, sift_col_read; sift_col_enc.c:sift_cunit_decode | uint32 group-count sum wraps, `expand_parents` writes far past `parent`: uint64 sum, reject outside [0,n], total equals child n |
| P1-5 | sift_col_enc.c:chunk_lut, cc_lut | codes index the LUT unchecked (F-COL-1): `max32(codes) < sbits` after unpack |
| P1-6 | sift_col_scan.c:read_positions (STR) | codes not range-checked against `dict->n` before sift_agg.c indexes `map[code]` |
| P1-7 | sift_table_store.c:sift_tbl_delta_page, link_delta_page | disk `npages` copied into a 1024 array |
| P1-8 | sift_table_store.c:sift_tbl_page_rec and callers (scan, seg, vacuum) | slot offsets unchecked, VACUUM writes there: return a record Span |
| P1-9 | sift_table_store.c:sift_tbl_rec_tuple | `rec->len` unchecked vs page, `t_len`; overflow chain without hop limit |
| P2-1 | sift_posting.c:post_attach, sift_post_group_tf, list_tf_pos | nnib/df/ng/esc length raw (F-BM25-5): carve sidecar Spans at attach |
| P2-2 | sift_posting.c:post_open; sift_dict.c:sift_dict_open; sift_segment.c:docmap, load_sb, walk_docs | allocation before validation (up to 32 GB) |
| P2-3 | sift_eval.c:score_block | offsets not ANDed with `present`: `knorm` reads past the row |
| P2-4 | sift_segment.c:sift_seg_open; sift_dict.c:sift_dict_open; sift_col_seg.c:sift_cseg_open | u64 `off+len` can wrap; section ends not checked against the stream |
| P2-5 | sift_live.c:sift_live_clear | page index and write unchecked (Assert only) |
| P2-6 | sift_table_store.c:sift_tbl_tomb_sync | `nent` unchecked; `next` chain can cycle |
| P2-7 | sift_colcodec.c:sift_cc_open, parse_node | row count compared after the whole tree is built: pass expected n in |
| P2-8 | sift_colcodec.c:parse_node WRAP | decompression total not capped: per-open byte budget |
| P2-9 | sift_colcodec.c:sift_cc_decode_strs | `total` allocated before the lengths are summed |
| P2-10 | sift_col_seg.c:sift_cseg_unit, sift_cseg_unit_head | unit buffer lacks `SIFT_COL_PAD`; `unpack` reads 31 bytes past |
| P2-11 | sift_col_enc.c:sift_cchunk_zone, chunk_scalar | `palloc((n+8)*8)` with header n up to 2^28 |
| P3-1 | sift_colcodec.c:parse_node, parse_child, sift_cc_open, sift_cc_decode_bits | hand-written `need()` checks: move to take/sub/read_u32le |
| P3-2 | sift_col_enc.c:sift_cchunk_parse, unit_parse, sift_cdict_parse, sift_cschema_parse | `nulls`/`ovf`/`nbytes` set before the check; plain memcpy |
| P3-3 | sift_col_seg.c:sift_cseg_open, sift_cseg_dict, zone_arrays | manual arithmetic; one 7-byte over-read at 429/441 |
| P3-4 | sift_colz.c:sift_colz_decompress | `(int)` truncation of clen/rawlen: reject above INT_MAX |
| P3-5 | sift_store.c:sift_sr_get, sr_bytes, sift_sr_init | correct; return a SiftSpan so callers inherit the bound |
| P3-6 | sift_dict.c:block_begin, block_next, find_block | correct; `pos_len` points into the shared buffer: take/read_varint |
| P3-7 | sift_posting.c:pos_next, pos_window, pos_ckpt, BLOB_NG/BLOB_KEYS; sift_segment.c:docmap; sift_store.c:check_dir; sift_mutable.c:rec_at | correct; macros become `sift_span_u32le_at`, rest for uniformity |
| P3-8 | real range checks: nd_get DICT, unit_emit, sift_cc_decode_bits, sparse_valid | plain bound or `max32`, not Span |
| P4-1 | sift_varint.h:sift_get_varint (sift_bitmap.c 86,470,504,608,909) | unbounded, safe only by 64 pad bytes (F-BM25-1): Span `read_varint` |
| P4-2 | sift_varint.h:sift_get_varint64_n, sift_posting.c:get_v32, sift_post_positions | last-byte overflow bits dropped, `want*=2` wraps above 2 GB: reject |
| P4-3 | sift_col_enc.c:sift_cunit_chunk; sift_col_seg.c:sift_cseg_unit_fill | XX000 to DATA_CORRUPTED (F-COL-2); `off+len` guarded by Assert only |
| P4-4 | sift_col_scan.c 584, 621, 871 | scattered `c.n` checks: move into sift_cunit_chunk with the expected count |

Also: extend sift.verify (src/sift_segment_sql.c) to run the same Span parsers and the P1-3 popcount check, cross-check DICTIDX against the
dictionary blocks. Not audited: sift_docdir.c, sift_lane.c, rest of sift_mutable.c, sift_cscan.c, sift_cscan consumers.

## Decisions for the coordinator

1. Drop `pointer-overflow` from the `1` flavor (+9 to +36%, rarely a bug on 64-bit)? One word in scripts/harden.mk.
2. F-BM25-2/3 and F-COL-1 are reachable from a corrupt page on the query path: fix ahead of the full conversion?
3. Add an assert-enabled PostgreSQL build (`--enable-cassert`, `-DRANDOMIZE_ALLOCATED_MEMORY`, Valgrind lane) to CI; register for Coverity Scan?
4. Accept up to 1% instructions on column-scan and BM25 cells per converted decoder?

## Sources

clang bounds-safety https://clang.llvm.org/docs/BoundsSafety.html, UBSan https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html, libFuzzer
https://llvm.org/docs/LibFuzzer.html, gcc counted_by https://gcc.gnu.org/onlinedocs/gcc/Common-Attributes.html, FORTIFY_SOURCE=3
https://developers.redhat.com/articles/2022/09/17/gccs-new-fortification-level, OpenSSF flags
https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html, counted_by in practice
https://opensource.googleblog.com/2024/07/bounds-checking-flexible-array-members.html, PostgreSQL pg_config_manual.h, amcheck
https://www.postgresql.org/docs/current/amcheck.html, Valgrind https://wiki.postgresql.org/wiki/Valgrind, CBMC https://www.cprover.org/cbmc/, Frama-C WP
https://frama-c.com/fc-plugins/wp.html, Coverity Scan https://scan.coverity.com/, MTE https://developer.arm.com/documentation/108035/, CHERI
https://www.cl.cam.ac.uk/research/security/ctsrd/cheri/, pgrx https://github.com/pgcentralfoundation/pgrx.
