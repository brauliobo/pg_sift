# Memory safety in C for pg_sift

The column engine and the BM25 readers decode bytes from disk. Every byte read goes through a length-carrying type (`SiftSpan`, src/core/span.h), arithmetic on stored
sizes is checked, the compiler inserts what it can, and tools find the rest.

## Threat model

| Input | Example | Layers that catch it |
|---|---|---|
| Corrupt or malicious segment bytes (disk fault, hostile restore) | a length, count, offset or code that points outside its buffer | Span (at the read), `sift_blob_check` / sift.verify (structure, offline), fuzzers (before release), trap UBSan and FORTIFY (at run time, abort instead of exploit) |
| Hostile sizes (OOM) | `n` = 16M rows in a 40-byte header, allocation before validation | Span-first order (validate, then allocate), `MaxAllocSize`, per-open decompression budget |
| Concurrent modification | a page changes between two reads of one header | copy the header once into a local, then parse the copy; buffer locks (not a C-language issue) |
| Own bugs | off-by-one in a SIMD tail, use after free in a memory context | ASan/UBSan CI, fuzzers, Valgrind, PG debug build, static analysis |

An attacker who can write a data file already owns the server (PostgreSQL trusts its data directory); the goal is that corruption becomes an `ERROR`
(ERRCODE_DATA_CORRUPTED), never a wild read or write. The layers complement each other: Span checks what the code asks, ASan sees heap and
stack but not an overrun inside one palloc chunk, trap UBSan sees array bounds of known size and pointer wrap, fuzzers find the unexpected.

## Ranking (safety gain / cost; cost is instructions by cpu_gate, below; "pkg" is shipped in packages)

| # | Measure | Gain | Cost | Where |
|---|---|---|---|---|
| 1 | `SiftSpan` for every stored offset/length; validate before allocate | closes the P1/P2 class of the audits | ~0 on parse paths, to be measured per converted decoder | src/core/span.h, phase 2 |
| 2 | Fuzz every decoder, corpus from tests, differential vs scalar | 8 findings, 3 high | CI minutes | test/fuzz, scripts/fuzz.sh |
| 3 | `SIFT_HARDEN=pkg`: stack protector strong, stack clash, CET/branch protection, strict-flex-arrays=3, FORTIFY_SOURCE=3, relro/now | stack smash, libc misuse, ROP | 0.0 to +0.3% | pkg |
| 4 | `-ftrivial-auto-var-init=zero` (`SIFT_HARDEN=init`) | uninitialized stack and padding leaks | +0.1 to +1.9% | CI hardened; pkg optional |
| 5 | Trap UBSan `bounds,shift,nonnull,returns-nonnull,vla-bound` | flexible/array overruns, bad shifts | +1 to +5% (bounds), +2 to +4% (shift set) | CI hardened |
| 6 | `object-size` trap | pointer-to-object overruns | +3 to +8% | CI hardened |
| 7 | `pointer-overflow` trap (`SIFT_POINTER_OVERFLOW=1`) | pointer wrap | +9 to +36% | separate opt-in, off in `1` |
| 8 | Static analysis (scripts/static.sh) | shallow bugs, widening multiplies | none at run time | CI `static`, non-blocking |
| 9 | Structural verifier (sift.verify) sharing the Span code | offline detection | none at run time | phase 2 |
| 10 | CBMC on Span and the chunk header parser; clang `-fbounds-safety` | proof for small code; compiler-enforced bounds | CBMC not used; bounds-safety blocked | later |
| 11 | `debug_discard_caches = 1` on a server with assertions (tests `cache_discard`, `verify_relcache`) | a pointer into the relcache's cached state (`rd_amcache`) kept across a lock or a catalog read: the entry is reloaded at every such point and the freed state is overwritten. Found the crash of , of `sift.col_verify` and of the cost estimate of a scan; each dies in seconds on a few rows | `cache_discard` 48 s (0.5 s without the setting, about 30 ms per row), `verify_relcache` 2 s; 0 at run time | the PostgreSQL 19 job of CI (a build with assertions) and the `assert` lane |

## Measured overhead

`bench/steps/cpu_gate.sh`: user-mode instructions per operation (8 rounds, 20k rows, 95% intervals under 1%), build with and without; wall time was not used (loaded host).
BM25 cells: bulk, merge and the query classes count, rank, and, or, phrase. Column-scan cells: 5 pushdown queries (bench/cpu-query.sh, 100 queries, 5 rounds). Single checks ran on top of `init`.

| Flag set | BM25 cells | Column scan | Verdict |
|---|---|---|---|
| `pkg` | 0.0 to +0.3% (bulk +0.3) | 1.000 to 1.002 | pkg: on |
| `init` | +0.1 to +1.9% (merge +1.9, bulk +1.1) | +0.1 to +0.4% | CI; packages optional |
| `1` as measured, with `pointer-overflow` | +13 to +45% (bulk +45, merge +38) | +14.5% to +62% (ts_group +62, filter_count +40) | `1` now omits it: lower, not re-measured |
| `bounds` alone | +1.0 to +4.8% | not run | off by default |
| `shift,nonnull,returns-nonnull,vla-bound` | +0 to +3.7% | not run | off by default |
| `object-size` | +2.7 to +8.1% | not run | off by default |
| `pointer-overflow` | +9 to +36% (bulk +36, merge +27) | not run | opt-in only |
| ASan+UBSan (SANITIZE=1) | not gated | not gated | CI sanitize flavor |

Flags above 1% on hot loops are off by default and on in the `hardened` flavor (`pointer-overflow` only on request). `signed-integer-overflow` is not trapped (`-fwrapv` defines it).
The Span is not measured yet (no decoder uses it); phase 2 measures each converted decoder with the same cells. Reproduce: `bench/steps/cpu_gate.sh --env LABEL=SIFT_HARDEN=1`.

## What each knob is

`SIFT_HARDEN=pkg|init|1` (scripts/harden.mk, included by the Makefile) probes every flag with the host compiler and drops unknown ones. `SIFT_UBSAN_SET="bounds shift"` (or comma list)
picks the trap checks and `SIFT_POINTER_OVERFLOW=1` adds the pointer-wrap trap; scripts/pgtest.sh passes them. `pkg` also adds `-Wvla -Walloca -Warray-bounds=2 -Wstringop-overflow=4
-Wformat=2 -Wundef`. `-Wconversion` (474 hits) and `-Wsign-conversion` (1306) are too noisy to enable; `-Wnull-dereference` (36) and `-Wclobbered` (2, see the findings) merit a one-time read.

`scripts/check-memory.sh` (lint, with a selftest; `check-memory: allow <why>` on a site) flags what a grep can decide: `rd_amcache` outside the files that own an accessor of it, and an
inclusive `for` loop over a 64-bit counter (it never ends when the bound is the largest value, the key loop of the point directory).

CI (scripts/ci.sh): `hardened` (scripts/hardened.sh: private copy built with `SIFT_HARDEN=1`, 16 decoder regress tests), `static`, `span-test` (sqlite/tests/span_test.c, ASan/UBSan,
SIMD=none), `fuzz` (scripts/fuzz.sh). Opt-in lanes on a PostgreSQL built from source (`make pg-assert`, `make pg-valgrind`, `PG_ASSERT_MAJOR=17` for the 17 branch; scripts/pg-assert.sh
pins 18.6 and 17.11 with cassert, debug, `-DRANDOMIZE_ALLOCATED_MEMORY`, `-DCATCACHE_FORCE_RELEASE` (every syscache lookup builds a fresh entry and frees it at release, so a `HeapTuple`
kept past its `ReleaseSysCache` is garbage; found F-MCX-1), plus `-DUSE_VALGRIND` for the valgrind prefix; cached in `$SIFT_PG_CACHE`, default `~/.cache/sift/pg`):
- `SIFT_CI_JOBS=assert scripts/ci.sh quick` (scripts/assert.sh): 13 regression tests and 2 quick-tier TAP tests; fails on a TRAP or a memory-context warning in any server log; passes on 18.6 and 17.11.
- `SIFT_CI_JOBS=valgrind` (scripts/valgrind.sh): 4 tests under memcheck with the server's valgrind.supp and `--error-exitcode`, bounded by `SIFT_VALGRIND_TIMEOUT`; a host whose glibc has
  no debug symbols runs it in a pinned Debian bookworm container (see the script header); skips when neither works.
- Weekly and non-blocking on GitHub: .github/workflows/assert.yml. The quick tier of `ci.yml` has one assertions entry ("PostgreSQL 18 (assertions)", the 30 `installcheck-agg_*` regress
  tests of the planner paths: cte, cost, scope, lateral, subplan, placeholder; the whole `installcheck-agg*` set takes 61 minutes there): planner asserts (a `foreach_node` cast of the wrong
  node type, found on PostgreSQL 19 first) exist only in a `--enable-cassert` build, which the distribution packages of 17 and 18 lack.

## Tools (gcc 16.2, clang 23.1)

Used: clang-tidy, scan-build, gcc -fanalyzer, libFuzzer, MSan and UBSan-minimal runtimes, valgrind 3.25, perf. Not used: cppcheck (scripts/static.sh skips it when absent), CBMC, ESBMC, Frama-C, CodeQL, AFL++.
- clang `-fbounds-safety` (`__counted_by`, `__sized_by`, `__indexable`): clang 23 only has `-Xclang -fexperimental-bounds-safety`, no
  `<ptrcheck.h>`, and a one-function probe did not build; Apple's clang ships it. Not usable on the Debian/RPM/AUR toolchains.
- `counted_by` on flexible array members: gcc 15+ and clang 18+; with `-fsanitize=bounds` the access traps (verified with gcc 16). Applies to 5 sites only (src/core/page.h x3,
  src/bm25/parallel.h, src/col/batch.c); the pgsql-hackers thread "Add counted_by attribute" is the PostgreSQL-side attempt (it found write-order problems under bounds checking). Low value: decoders mostly walk byte buffers.
- PostgreSQL aids: `--enable-cassert` implies MEMORY_CONTEXT_CHECKING and CLOBBER_FREED_MEMORY (they catch palloc overruns ASan cannot); the
  `assert` and `valgrind` lanes above use them. amcheck is the model for sift.verify.
- MSan: only for the standalone fuzz targets, with every dependency instrumented (zstd/lz4/libpgcommon are not: `DEFS= COLZ=`, false positives in libc wrappers).
- MTE (arm64) and CHERI: hardware tag/capability checks; no toolchain in the distro targets.
- Rust: not recommended now. pgrx would replace the C ABI of the access methods and the portable core shared with SQLite and MariaDB;
  possible later for a standalone decoder crate behind a C ABI and `cbindgen`, with the Span semantics as its contract.

Packages (Debian/RPM/AUR) use `SIFT_HARDEN=pkg` (FORTIFY needs -O2; aarch64 gets `-mbranch-protection` instead of CET); `init` and above stay off.

## Coverity Scan

.github/workflows/coverity.yml (weekly and on demand) builds under `cov-build` and uploads to scan.coverity.com; without the secrets it does nothing and
passes. Setup: (1) sign in at https://scan.coverity.com with the GitHub account and add the public repository as a project (C/C++; project name `owner/repo`, or set the repository
variable `COVERITY_SCAN_PROJECT`); (2) copy the project token from Project Settings; (3) add the Actions secrets `COVERITY_SCAN_TOKEN` (that token) and `COVERITY_SCAN_EMAIL` (the email of the
account that owns the project); (4) run the workflow once from the Actions tab (`workflow_dispatch` needs the workflow on the default branch); (5) triage the defects at scan.coverity.com.
Coverity limits builds per day.

## Phase 2: conversion list (31 entries)

P1 out-of-bounds from stored data, P2 unbounded loop or allocation, P3 correct hand-written checks to move onto Span, P4 hygiene. Each item: raw offset arithmetic becomes
`sift_span_sub/array/take/read_*`, validate before allocate, raise DATA_CORRUPTED.

| Id | File:function | Gap and replacement |
|---|---|---|
| P1-1 | bm25/bitmap.c:sift_cur_blob, gr_seek, ls_seek, ef_hdr, ef_decode, ls_at | query path decodes unchecked blobs (F-BM25-2/3/4, fixed by explicit checks in 2.0.2, 463951f; the Span conversion is open): cursor over a Span, per-group sub-span, lazy check_grouped rules |
| P1-2 | bm25/bitmap.c:sift_chunk_pagemask/offsets/count (called by bm25/segment.c:load_sb) | no length argument; pass `sift_span_sub(rec,8,h[1])`, require w in {1,2,4,8..64}, pg<256 |
| P1-3 | bm25/segment.c:load_sb; bm25/walk.c:walk_docs; bm25/liveness.c:live_sb_bits | ndocs from mask popcounts never compared with the header; `keys[]` overflow |
| P1-4 | col/scan_unit.c:unit_group, col/scan_read.c:sift_col_read; col/batch.c:sift_cunit_decode | uint32 group-count sum wraps, `expand_parents` writes far past `parent`: uint64 sum, reject outside [0,n], total equals child n |
| P1-5 | col/ctest.c:chunk_lut, cc_lut | done (F-COL-1): tables of at most 16 bits cover the width, wider codes pass `max32(codes) < sbits` |
| P1-6 | col/scan_read.c:read_positions (STR), sift_col_read_lane (CODE lane) | done: `max32(codes) <= dict->n` once per chunk read, before agg/agg.c indexes `map[code]` or the LUT |
| P1-7 | table/wbuf.c:sift_tbl_delta_page, link_delta_page | disk `npages` copied into a 1024 array |
| P1-8 | table/wbuf.c:sift_tbl_page_rec and callers (scan, seg, vacuum) | slot offsets unchecked, VACUUM writes there: return a record Span |
| P1-9 | table/wbuf.c:sift_tbl_rec_tuple | `rec->len` unchecked vs page, `t_len`; overflow chain without hop limit |
| P2-1 | bm25/posting.c:post_attach, sift_post_group_tf, list_tf_pos | nnib/df/ng/esc length raw (F-BM25-5, fixed by explicit checks in 3.0.0, 7f59d1a; the Span conversion is open): carve sidecar Spans at attach |
| P2-2 | bm25/posting.c:post_open; bm25/dict.c:sift_dict_open; bm25/segment.c:docmap, load_sb; bm25/walk.c:walk_docs | allocation before validation (up to 32 GB) |
| P2-3 | bm25/rank.c:score_block | offsets not ANDed with `present`: `knorm` reads past the row |
| P2-4 | bm25/segment.c:sift_seg_open; bm25/dict.c:sift_dict_open; col/seg.c:sift_cseg_open | u64 `off+len` can wrap; section ends not checked against the stream. The pre-aggregate sums and cubes of col/seg.c are compared without a wrapping sum; every read is bounded by `sift_sr_get` |
| P2-5 | core/live.c:sift_live_clear | page index and write unchecked (Assert only) |
| P2-6 | table/tomb.c:sift_tbl_tomb_sync | `nent` unchecked; `next` chain can cycle |
| P2-7 | col/codec_parse.c:sift_cc_open, parse_node | row count compared after the whole tree is built: pass expected n in. Done: `sift_cc_open_rows` |
| P2-8 | col/codec_parse.c:parse_node WRAP | decompression total not capped: per-open byte budget |
| P2-9 | col/codec_blob.c:sift_cc_decode_strs | `total` allocated before the lengths are summed. Done for the count and the raw form (`sift_cc_decode_strs` takes the expected n); `total` of a compressed list stays bounded by 2^31 and the allocation limit |
| P2-10 | col/seg.c:sift_cseg_unit, sift_cseg_unit_head | unit buffer lacks `SIFT_COL_PAD`; `unpack` reads 31 bytes past |
| P2-11 | col/enc.c:sift_cchunk_zone, col/ctest.c:chunk_scalar | `palloc((n+8)*8)` with header n up to 2^28 |
| P3-1 | col/codec_parse.c:parse_node, parse_child, sift_cc_open, col/codec_blob.c:sift_cc_decode_bits | hand-written `need()` checks: move to take/sub/read_u32le |
| P3-2 | col/chunk.c:sift_cchunk_parse, col/unit.c:unit_parse, col/dict.c:sift_cdict_parse, col/schema.c:sift_cschema_parse | `nulls`/`ovf`/`nbytes` set before the check; plain memcpy |
| P3-3 | col/seg.c:sift_cseg_open, sift_cseg_dict, zone_arrays | manual arithmetic; one 7-byte over-read at 429/441 |
| P3-4 | col/compress.c:sift_colz_decompress | `(int)` truncation of clen/rawlen: reject above INT_MAX |
| P3-5 | core/reader.c:sift_sr_get, sr_bytes, sift_sr_init | correct; return a SiftSpan so callers inherit the bound |
| P3-6 | bm25/dict.c:block_begin, block_next, find_block | correct; `pos_len` points into the shared buffer: take/read_varint |
| P3-7 | bm25/pos.c:pos_next, pos_window, pos_ckpt; bm25/posting.c:BLOB_NG/BLOB_KEYS; bm25/segment.c:docmap; core/store.c:check_dir; bm25/wbuf.c:rec_at | correct; macros become `sift_span_u32le_at`, rest for uniformity |
| P3-8 | real range checks: nd_get DICT, unit_emit, sift_cc_decode_bits, sparse_valid | plain bound or `max32`, not Span |
| P4-1 | core/varint.h:sift_get_varint (bm25/bitmap.c 86,470,504,608,909) | unbounded, safe only by 64 pad bytes (F-BM25-1): no caller of the unbounded `sift_get_varint` is left since 2.0.2 (463951f, 0ddff29); Span `read_varint` for the bounded ones |
| P4-2 | core/varint.h:sift_get_varint64_n, bm25/pos.c:get_v32, sift_post_positions | last-byte overflow bits dropped, `want*=2` wraps above 2 GB: reject |
| P4-3 | col/unit.c:sift_cunit_chunk; col/seg.c:sift_cseg_unit_fill; col/scan.c 584, 621, 871 | XX000 to DATA_CORRUPTED (F-COL-2: done for `sift_cunit_chunk`); `off+len` Assert only; scattered `c.n` checks into sift_cunit_chunk |

Also: extend sift.verify (src/bm25/segment_sql.c) to run the same Span parsers and the P1-3 popcount check, cross-check DICTIDX against the
dictionary blocks. Not audited: bm25/docdir.c, core/lane.c, rest of bm25/wbuf.c, bm25/cscan.c, sift_cscan consumers.

## Decisions

Decided: assert and valgrind lanes, Coverity Scan, `pointer-overflow` out of `1`, and the fix of F-BM25-2/3 and F-COL-1 (reachable from a corrupt page on the query path) ahead of the full conversion (done: F-BM25-2/3 in 2.0.2, F-COL-1 in 3.0.0). Open:
1. Accept up to 1% instructions on column-scan and BM25 cells per converted decoder?

## Sources

clang bounds-safety https://clang.llvm.org/docs/BoundsSafety.html, UBSan https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html, libFuzzer
https://llvm.org/docs/LibFuzzer.html, gcc counted_by https://gcc.gnu.org/onlinedocs/gcc/Common-Attributes.html, FORTIFY_SOURCE=3
https://developers.redhat.com/articles/2022/09/17/gccs-new-fortification-level, OpenSSF flags https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html,
counted_by https://opensource.googleblog.com/2024/07/bounds-checking-flexible-array-members.html, amcheck https://www.postgresql.org/docs/current/amcheck.html,
Valgrind https://wiki.postgresql.org/wiki/Valgrind, CBMC https://www.cprover.org/cbmc/, Frama-C WP https://frama-c.com/fc-plugins/wp.html, Coverity Scan
https://scan.coverity.com/, MTE https://developer.arm.com/documentation/108035/, CHERI https://www.cl.cam.ac.uk/research/security/ctsrd/cheri/, pgrx https://github.com/pgcentralfoundation/pgrx.
