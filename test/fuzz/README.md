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
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` because backend memory lives in
memory contexts and is never freed. Reports are written to
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
| `kernels` | kernels | `tools/kernels` randomized and exhaustive equivalence of SIMD and scalar kernels (`make SANITIZE=1`) |
| `analyze` | analyze | `tools/analyze/harness fuzz N SEED`: random text through the tokenizer, checking its invariants |
| `query` | query | `tools/query/gen.py --mode fuzz` corpus through the standalone parser harness, then through the server (`tools/query/fuzz.sql`: fixpoint deparse, same matches) |
| `sql` | tests | `test/fuzz/fuzz.sql` in the server: hostile SiftQL to `sift.parse_query` (failures other than 42601/54000/2201B; disagreements with the SQL reference parser are reported, not fatal) and hostile text to `sift.tokenize` (invariants) |

`--scale` multiplies the smoke-sized counts of `analyze`, `query` and `sql`.
The default is 10; `scripts/ci.sh smoke` uses 1. `codec` and `kernels` have
fixed sizes. Run `scripts/fuzz.sh` with no arguments, at nice 19, for long
sessions. Every input of the `sql` target is logged as `sift_fuzz input` before
use, so after a backend crash the script prints the input that killed it.

Adding a fuzzer: give it a standalone harness under `tools/<module>/` whose
Makefile accepts `SANITIZE=1` (or always sanitizes), add a function named after
the module to `scripts/fuzz.sh`, and add a row here. libFuzzer entry points
(`LLVMFuzzerTestOneInput`) need clang. gcc is the only compiler on this host, so
the harnesses drive their own generators.
