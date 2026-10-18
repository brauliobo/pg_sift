# Fuzzing and sanitizers

`scripts/fuzz.sh [--scale N] [--seed N] [TARGET...]` builds every fuzzer and
randomized harness with AddressSanitizer and UndefinedBehaviorSanitizer and runs
them. It prints `PASS`/`FAIL` per target and exits 1 on any finding.

## Server under ASan

    SANITIZE=1 scripts/pgtest.sh install    # make clean + build with -fsanitize=address,undefined
    scripts/pgtest.sh stop; scripts/pgtest.sh start
    scripts/fuzz.sh                         # all targets
    scripts/pgtest.sh sanitizer-report      # exit 1 if the server logged a finding

The PostgreSQL server binary is not instrumented, so `pgtest.sh start`
preloads `libasan`/`libubsan` (`LD_PRELOAD`). It sets
`ASAN_OPTIONS=detect_leaks=1:exitcode=0:abort_on_error=1`: LeakSanitizer
reports the malloc leaks of every server process as it exits, minus PostgreSQL's
own startup allocations (`scripts/lsan.supp`); palloc memory belongs to memory
contexts and is not a malloc leak. The standalone harnesses (`tools/analyze`,
`tools/query`, `tools/codec`) run with leak detection too; their palloc stubs
free the arena at exit (`tools/pgshim.c`). Reports are written to
`$SIFT_TMP/sanitizer/`. The `canary` target loads a deliberately overflowing
module (`asan_canary.c`) and checks that a report appears, which shows the
pipeline works. Plain `scripts/pgtest.sh install` switches back to the normal
build.

ASan only sees `malloc`, stack and global memory. Overruns inside a `palloc`
chunk stay invisible unless they reach the chunk header, which memory-context
checks then catch in assert-enabled builds. The standalone harnesses below use
`malloc` and get full coverage, so prefer them for pointer-heavy code.

## Targets

| Target | Owner | What runs |
|---|---|---|
| `canary` | tests | ASan pipeline self-test (server must be the ASan flavor) |
| `codec` | codec | `tools/codec` randomized property tests against sorted-array references, at every ISA level |
| `blob` | hardening | `tools/codec/fuzz`: bit-flipped, truncated and spliced term blobs; `sift_blob_check` must never read outside the blob, and every accepted blob must decode to the same strictly increasing tids on all ISAs, through the AND/OR/ANDNOT cursors, seeks and counts. `fuzz -s SEED -i ITER` replays one iteration |
| `kernels` | kernels | `tools/kernels` randomized and exhaustive equivalence of SIMD and scalar kernels (`make SANITIZE=1`) |
| `analyze` | analyze | `tools/analyze/harness fuzz N SEED`: random text through the tokenizer, checking its invariants |
| `query` | query | `tools/query/gen.py --mode fuzz` corpus through the standalone parser harness, then through the server (`tools/query/fuzz.sql`: fixpoint deparse, same matches) |
| `colchunk` | safety | `test/fuzz/fz_colchunk.c`: the column codec blobs, string and bit lists, unit chunks, unit records, segment dictionaries and schemas (7 modes in one input), at every SIMD level, levels must agree. Coverage guided, 6 s per `--scale` step |
| `bm25` | safety | `test/fuzz/fz_bm25.c`, mode 1: `sift_blob_check` then every cursor on every ISA; an accepted blob must stay inside itself |
| `bm25-raw` | safety | the same target on the query path: bytes nobody verified (`sift_cur_blob`, `sift_post_open`, tf, positions). Fails today; opt-in |
| `shred` | safety | `test/fuzz/fz_shred.c`: a path spec and a JSON text through the shredder's event interface, the produced document checked (ranges, states, parents at every level), spec deparse a fixed point |
| `sql` | tests | `test/fuzz/fuzz.sql` in the server: hostile SiftQL to `sift.parse_query` (failures other than 42601/54000/2201B; disagreements with the SQL reference parser are reported, not fatal) and hostile text to `sift.tokenize` (invariants) |

`--scale` multiplies the iteration counts of every target (default 10, about
3.5 minutes; `codec` runs 300 iterations and `kernels` 0.2 million per unit).
`--budget SEC` runs scale-1 passes (about 25 s each) until SEC seconds have
passed, the seeded targets with a new seed each pass; `scripts/ci.sh quick`
uses a 20 s budget. Run `scripts/fuzz.sh` with no arguments, at nice 19, for
long sessions. Every input of the `sql` target is logged as `sift_fuzz input`
before use, so after a backend crash the script prints the input that killed it.

## Coverage-guided targets (test/fuzz)

`colchunk`, `bm25` and `shred` link the decoders without a backend: `tools/pgshim.c` and `fz_shim.c` stand in for the allocator (every `palloc` is its own
malloc, so ASan sees an overrun of a palloc'd object; `MaxAllocSize` is enforced), for `ereport(ERROR)` (a `longjmp` to `fz_try`; the
answer a decoder may give to corrupt bytes is DATA_CORRUPTED, INDEX_CORRUPTED, FEATURE_NOT_SUPPORTED or a limit, any other SQLSTATE is
reported once as `FINDING-LOW`) and for the stream reader. They build with clang as libFuzzer binaries, with gcc (or `FZ_DRIVER=1`)
against `fz_driver.c`, a coverage-guided mutation driver with the same command line. Always ASan and UBSan with `-fno-sanitize-recover=all`
and without `-fwrapv`: signed overflow is a finding here.

    nice make -j4 -C test/fuzz OUT=/tmp/x all                   # colchunk, bm25, shred
    FZ_GEN=/tmp/seeds /tmp/x/colchunk                           # the seed corpus from the writers, then exit
    /tmp/x/colchunk -max_total_time=60 -timeout=10 -rss_limit_mb=4096 -artifact_prefix=/tmp/art- /tmp/corpus
    /tmp/x/colchunk /tmp/art-crash-...                          # replay
    nice make -j4 -C test/fuzz OUT=/tmp/x regress               # replay regress/: the crashes of the fixed findings
    nice /tmp/x/bm25 -fork=3 -ignore_crashes=1 -max_total_time=60 /tmp/corpus   # many findings in one run (libFuzzer)

Environment: `FZ_MODE=n` forces the mode of an input (the first byte otherwise), `FZ_SKIP=a,b` skips checks whose finding is recorded
(`lut` in colchunk), `FZ_NOERR=1` makes any raised error fatal (a seed that does not parse is a bug of the seed), `FZ_STRICT=1` makes a
wrong SQLSTATE fatal, `FZ_TRACE=1` prints the digest after each step (to see which operation two SIMD levels disagree on),
`UBSAN_OPTIONS=suppressions=test/fuzz/ubsan.supp` skips the UB findings already recorded. `scripts/fuzz.sh` sets the skips and
suppressions of the findings that are not fixed yet; delete an entry when its fix lands.

Adding a fuzzer: give it a standalone harness under `tools/<module>/` whose
Makefile accepts `SANITIZE=1` (or always sanitizes), add a function named after
the module to `scripts/fuzz.sh`, and add a row here. libFuzzer entry points
(`LLVMFuzzerTestOneInput`) need clang. gcc is the only compiler on this host, so
the harnesses of tools/ drive their own generators; the targets of test/fuzz use libFuzzer where clang has it and their own driver else.
