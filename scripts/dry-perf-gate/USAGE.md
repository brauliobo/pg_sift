# Performance gate for refactor commits (scripts/dry-perf-gate.sh)

1. Commit your refactor on your branch, then run (nice 19; with GATE_HEAVY=CMD behind that job-queue wrapper; scratch is tmpfs, GATE_ROOT):
   `scripts/dry-perf-gate.sh --src <your repo> BASE CAND`  e.g. `scripts/dry-perf-gate.sh --src . HEAD~1 HEAD`  (BASE = the parent, or the tip you branched from).
   Not committed yet: `scripts/dry-perf-gate.sh --src . HEAD --worktree .` (candidate = HEAD of `.` plus the uncommitted diff) or `--patch FILE`.
2. Both sides are built with identical flags (release library, `-D__LINE__=0`, fixed build id). Stripped object files all identical => it prints `PASS-IDENTICAL`
   in about a minute and stops (include trims, comment and whitespace edits, line moves): attach that line.
3. Otherwise the changed paths select case groups (`scripts/dry-perf-gate/area-map.tsv`; `scripts/dry-perf-gate.sh select BASE CAND` shows them, `--groups g1,g2` or `--cases id,..` override,
   `list` prints all cases). Each case runs in a fresh backend, base and candidate interleaved, 1 discarded + 9 measured runs, on two clusters copied from one
   data template (the same bytes), max_parallel_workers_per_gather = 0 on both sides; instructions and cycles are those of the whole process tree.
4. Reading the table: `instr base -> cand` and `d%` are instructions:u (minimum of the runs; deterministic, the primary signal), `cycles d%` the same for cycles:u
   with `noise%` = the spread of the runs (information only; the cycles verdict needs > max(2%, 1.5 x the stored min-vs-min noise of the case, 8% machine floor in baselines/noise.json); statements under 5 M cycles are judged on instructions only),
   `hit+read` shared-buffer hits + reads of the backend, `wal` WAL bytes of the statement, `ms b/c` elapsed. Verdict per case PASS / FAIL with the reasons.
5. FAIL when: instructions > +0.5% (and > 20,000 more), cycles > that threshold after up to 2 automatic re-measurements, hits / reads differ at all, WAL bytes differ by more than twice the stored base-vs-base noise (0 for almost every case),
   result rows (md5 of the sorted rows) or the EXPLAIN (COSTS OFF) plan differ, binary text grows by more than 0.1% in a commit that adds more src lines than it
   removes. All limits are options (`--max-instr-pct`, `--max-cycles-pct`, `--instr-abs`, `--blocks-tol`, `--wal-tol`, `--max-size-pct`, `--min-cycles`, `--runs`).
   Exit status: 0 PASS / PASS-IDENTICAL, 1 FAIL. `--json FILE` keeps every run.
6. A FAIL on instructions is a real change of the generated work: look at the case's groups, keep the helper `static inline`/macro in per-row loops, or split
   the commit; a FAIL on cycles alone with equal instructions: rerun once (`--cases id --runs 9`); if it reproduces, treat it as real (lost inlining, layout).
7. Put the last line of the output (the evidence line) in the commit message body, e.g.:
   `perf-gate 295f60d3e879 -> 7c1d... (14 cases, groups agg-read,agg-kernel): instructions:u 1,204.31M -> 1,204.87M (+0.047%, worst agg_mode +0.11%), cycles:u 612.0M -> 611.4M (-0.10%, noise 1.2%), blocks 41,880 -> 41,880, WAL 0 -> 0 B, text -64 B, rows identical: PASS`
   or `perf-gate A -> B: PASS-IDENTICAL (all 533 stripped object files byte-identical, text +0 B)`.
8. The base is rebuilt/re-measured when it moves (builds, objects and the data template are cached per base sha under GATE_ROOT (default /dev/shm/sift-perf-gate); 4 builds and 3
   templates are kept). `scripts/dry-perf-gate.sh baseline BASE` (about 12 minutes, full registry, base against base) stores $GATE_STATE/baselines/<sha>.{json,tsv} and noise.json (default $XDG_STATE_HOME/sift-perf-gate, never in git)
   (the per-case cycles noise floor the later runs use); run it once per integration tip, not per commit.
9. Cross-checks the gate cannot make: parallel-worker plans (workers are 0 by default; use `--cases` with your own settings for those), cold device IO (tmpfs), and
   sizes beyond 100k rows (`--rows N` rebuilds the template; slopes belong to scripts/scaling-gate.sh). It does not replace the covering regress tests.
10. Run it after the covering tests pass and before landing; a lane that cannot meet the budget (about 10 minutes) uses `--runs 3` for the first look and the default for the final.
