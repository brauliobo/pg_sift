# Focused performance gates

`python3 bench/steps/perf_gate.py --help` describes the standalone gate. It requires
PostgreSQL 18 development tools, a C compiler, make, Python 3, and libpq.

```sh
python3 bench/steps/perf_gate.py \
  --baseline-ref FULL_MERGE_BASE_SHA --candidate-ref FULL_PR_HEAD_SHA \
  --groups smoke,bm25,agg,storage --tier pr \
  --scratch-root OWNED_SCRATCH --output ARTIFACT_DIRECTORY
```

The workflow runs nightly and on dispatch, not on pull requests: of 439 hosted measurements (2026-10-20..22) 12 % were inconclusive and two of the six
regressions it reported cleared on a rerun of the same commit.

Both refs must be exact 40-character commit SHAs available locally. The workflow
resolves the merge-base and PR head; the driver records and validates their
identities. Nightly CI chooses the newest first-parent candidate ancestor at least
24 hours old as its baseline; it records that exact SHA. Manual and release-wide
benchmarking remain separate from the PR subset. Optional `--baseline-tree` and
`--candidate-tree` must have the specified HEAD and no tracked modifications. The driver archives each revision into its own
fresh source directory even when a tree is supplied, avoiding inherited objects.

Builds run sequentially with `nice -n19 make -j2`, identical production flags, and
the same PostgreSQL installation. Each revision has its own staged extension,
`extension_control_path`, `dynamic_library_path`, cluster, socket, data, and WAL.
There is no system-wide extension install and no physical index shared across
revisions. SQL fixtures are independently loaded in deterministic order, checked
by logical digest, and span at least three actual heap superblocks. Actual
`sift_table` column units, segment count, relation pages, and reusable pages are also recorded and checked;
the payload contains deterministic noncompressible per-row bytes. Serial SQL,
64 MiB shared buffers, disabled autovacuum/JIT, and asynchronous maintenance off
are identical on both sides. Fsync/full-page writes are disabled: these tests do
not measure durable commit latency. The caller must reserve its scratch storage
and two ports (`--port-base`, default 56240); use dedicated test partitions on a
shared local development host.

Socket endpoints alone use unique private0700 directories under `/tmp`, independent
of build/data/artifact roots and `TMPDIR`; all database/build/benchmark temporary
data stays on the caller's scratch backing. Both actual socket names are checked
in filesystem-encoded bytes before either build: Linux permits107 pathname bytes
plus NUL; macOS and supported BSD platforms permit103 plus NUL. No TCP/abstract
namespace or alternate-path fallback exists. Reports retain actual paths/limits.
Stopped clusters release their owned socket directories; the cost-probe restart
recreates its directory exclusively. A live or unsuccessfully stopped cluster
retains its directory. Startup server logs are copied before scratch removal even
without a PID; absent logs are explicitly reported, and copy failure retains
original scratch and primary error evidence.

After both fixture loads finish, an untimed synchronous `CHECKPOINT` runs on each
cluster before settled size probes or read validation/calibration/timing. Both
barriers must complete within the global setup budget. Reports retain each
barrier’s UTC start/end, duration and completion state, plus read/block start
timestamps. This drains pending setup checkpoints; it does not guarantee a quiet
host or lower variance. INSERT has the paired final-restoration barrier described below; UPDATE/VACUUM
receive no extra checkpoints.

The 16 PR cases in `bench/perf/catalog.py` cover BM25 top ten and counts for common,
rare, and filtered terms; aggregate scan, group, distinct, filter, and table
paths; and table point lookup, insert, update, and VACUUM space behavior.
`smoke` selects three fixed cases: common BM25 top ten, aggregate scan, and table
point lookup. These run on every PR that can change the built extension regardless of
selected groups; the gate, its selector, the CI machinery, tests and documentation select nothing,
because both revisions would build the same extension. Aggregate sources select `agg` only: the
`storage` cases run no grouped query and no table source scan (`scripts/areas.tsv`, `{ci}`). `bm25`,
`agg`, and `storage` add their cases; `--tier nightly` uses 32,768 rows
(PR: 8,192) and adds phrase-count and high-cardinality group workloads, for 18 cases.
This is a representative warm workload, not exhaustive feature or size coverage.

Before measuring, the driver compares baseline/head results and checks required
structured plan paths independently on both revisions. It records topology
changes as diagnostics without rejecting valid planner changes.
`EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING OFF)` runs outside measurement.
Aggregate/count/storage results are checked against ordinary heap SQL; plans and
relation sizes are retained. Ranked results are ordered by score then ID and
compared exactly between revisions; they do not have an independent ranking
oracle. UPDATE and VACUUM truncate and logically reload the same state before **every
operation**, outside timing. VACUUM starts with a fixed contiguous deleted range.
Its validation advances the xid horizon, runs a second untimed VACUUM, and requires
additional reusable pages or file shrinkage. Post-maintenance mutable table and
its B-tree index are each gated through the same relative-plus-page-tolerance size rule below, separately from time.

Index and table sizes are independent gate decisions. The settled-build and
settled-churn/VACUUM states compare BM25 `docs_bm25`, column `docs_col`, and
`sift_table` `store` with identical logical data/options in both revisions.
Churn updates indexed text and amounts, deletes fixed rows, and runs two VACUUMs
with an intervening xid advance. Raw main/FSM/VM/init bytes are retained separately
and summed per relation.
Each relation passes when candidate bytes are at most the larger of baseline
bytes times 1.10 (rounded up) and baseline plus two 8 KiB pages. The two-page floor
avoids rejecting harmless page rounding in tiny indexes. A larger result is a
regression, independently of elapsed-time results; missing/invalid SQL measurements
are errors. Completed valid size regressions remain decisive if later work is interrupted. Reports
separate extension indexes, auxiliary B-tree indexes, ordinary heap bytes, table
storage, and total footprint, with bytes/pages/ratio/threshold/state/verdict.
Heap and immutable auxiliary B-tree sizes are reported, not extension-size
gates; mutable table and B-tree are gated separately at full INSERT-trajectory
and VACUUM lifecycle states. The normal
size gate has its own pass/regression/inconclusive status, so a budget interruption
before all planned size states complete cannot claim a complete size pass.

The primary metric is monotonic elapsed time from sending simple-protocol SQL on
a persistent libpq connection through receipt of its complete result, including
planning. Connection startup, fixture/reset, correctness checks, EXPLAIN, and
size probes are outside measured windows. There is no empty-operation subtraction,
CPU-counter interpretation, fallback metric, or discarded outlier. SQL failures,
wrong answers/plans, and invalid timings are errors (exit 3).

Both sides warm independently. Calibration chooses one fixed common operation
count targeting 150 ms SQL for the faster side. Calibration also measures reset
plus SQL wall cost; a one-second batch wall cap limits the operation count when
reloading mutable state would dominate. The calibration records that limitation
and the resulting operation count rather than hiding reset work or pretending
that every mutable timing window achieved 150 ms. Six independent paired
batches alternate baseline/head then head/baseline. The per-pair candidate/base
elapsed ratios are recorded and transformed with the natural logarithm; their
mean and sample standard deviation in log space give the Student-t interval, and
the ratio and its two-sided 95% bounds are the exponentials of the mean and of
the interval ends (a geometric ratio). Supported complete blocks
are six pairs (df 5, critical value 2.570581835636305) and sixteen pairs
(df 15, critical value 2.131449545559323); partial blocks have no interval.
Repeated operations within a batch are not treated as independent samples.

INSERT uses lifecycle `insert_trajectory`, workload
`committed_append_settled_composite_v4`. Historical v2/v3/one-row timings are not
interchangeable with this metric. One observation consists of K restored history
pairs. Within an observation, history orders alternate AB/BA; the next observation
complements the whole schedule. Each fixed six-observation block starts AB. An
odd K therefore has complementary position counts across each two observations.
The excluded pilot is one AB history; full-N/K validation starts AB. Every history
uses the same owner, with its selected order O:

1. Warm both sides in O: configure, restore seed, N separate autocommitted points.
2. Final restore and all pre-stream counter/geometry/raw-fork probes on both in O.
3. Synchronous CHECKPOINT on each in O; BOTH must finish before either stream.
4. N individually completed point statements on each in O, with no SQL/probes/
   configure between streams. SQL timer sums remain separate from loop wall.
5. Deferred counters, seed/full-row multiset/count/digest and geometry/fork checks
   on both in O. Publish the history only after BOTH checks pass.

There are exactly two restores per side, no inner reset or extra checkpoint.
Before-forks are explicitly pre-finalization observations; after-forks are
sequential pair-end observations. Full-N validation gates mutable and mutable_pk
separately for EVERY validation history. History zero is the declared representative;
any other history size regression still fails the size gate. All sample history
footprints are retained, with no pooling of relation sizes or favorable state selection.
Partial phases/raw timings survive failure, without a valid pair or partial CI.
Warm/reset/probe/checkpoint/stream/check UTC and common process-origin monotonic
offsets expose unequal ages. Pair wall includes both sequential checkpoints,
all warm/reset/probe/loop/check/wait and residual bookkeeping exactly once;
phase costs explain wall, and overlapping ages are not additional costs.

IDs are seed maximum+1 through+N, checked against both seeds and the full integer
ID domain before any insertion. Payload is unchanged, including the compressible
1,500-byte pad. One history is N autocommitted point requests; no BEGIN wrapper,
bulk/server loop, pipeline, rollback restoration or physical fixture copy exists.
Full checks use explicit rows including NULLs/pad against ordinary heap seed plus
generated expected inserts, bidirectional EXCEPT ALL and count/digest. Actual
units/segments/pages/free/reusable blocks and raw main/FSM/VM/init forks remain.

One fixed clean AB pilot pair REPLACES the old prefix pilot: 2,048 warm AND
2,048 measured points per side, under this SAME owner and full checks. Retain
all raw operations. For each side, c=SQL sum/2048, w=loop wall/2048 and R=FINAL
reset wall. Freeze N once as min(ceil(.150/min(c)), floor(min((1-R)/w)),2048).
Nonpositive admissible wall cap is inconclusive, never coerced to1. The predicted
1s cap covers FINAL reset plus measured loop only; warm/probes/BOTH checkpoints/
waits/checks remain in the global budget. Freeze K once as min(ceil(.150/min(N*c)),5), retaining desired K, cap and
predicted shortfall. This bounded composition preserves the per-history growth
cap instead of appending K*N rows continuously. All histories, including recurring
operation-boundary spikes, contribute to each side SQL SUM; exactly six such sums
produce the unchanged df5 CI. Components are not independent statistical samples
and their ratios are not averaged. Any failed history invalidates the observation
and prevents a partial CI. A separate full-N/K validation follows;
there is no new pilot or recalibration under a fault/after a verdict. If N<2048,
the longer pilot's growth/warm history need not model N linearly.

This measures non-durable append after paired settled restoration, not durable
commit latency. It changes dirty/writeback/redo/recycling and cache age; fsync and
full_page_writes remain off. A waits through B's checkpoint, B through A's append;
slow candidate warm work can age the opposite side. Alternation does not equalize
ages or prove independence. Natural actual-append/time/other-cluster activity and
checkpoint overlaps remain in the samples, with no wait-until-quiet, exclusion or
retry. Restoration/checkpoint cost increases can be absent from the point ratio;
full wall costs remain visible, without a separate 10% restoration gate. Short windows do not prove adequate precision. Composition retains per-row
Python/encoding/deadline/libpq/autocommit overhead, multiplicative side bias and
correlated host noise; no sqrt(K) improvement is assumed. Reports give predicted
and actual useful-window fractions and all history walls without subtracting
client overhead or measured equal-source offsets.

The material threshold is 1.10. An initial upper bound at most 1.10 passes.
Every other case receives exactly one fresh confirmation block: **sixteen pairs
for reads without resets, six pairs for mutations**. This policy is fixed before
sampling, with the original 150 ms target and the same calibrated operation count
for both blocks. No post-verdict calibration or sample reuse occurs. Both blocks
and every raw sample are retained separately, including interrupted blocks. Only the confirmation decides: lower bound above 1.10 is
a regression (exit 1), upper bound at most 1.10 passes, and overlap is inconclusive
(exit 2). An inconclusive confirmation takes one more fresh **sixteen-pair extension** that alone
decides the case, only when the remaining budget covers it (predicted from the confirmation's pair
time) plus 30 seconds for each later mutation case; otherwise the case stays inconclusive. Controlled
INSERT and INSERT feasibility never extend. Exhausted measurement budget is inconclusive unless completed normal
timing/size evidence already establishes a regression (exit1). No noisy cell becomes
a silent pass. Intervals are per case, without a multiple-comparison guarantee;
shared-host load and temporal drift remain limitations of paired elapsed timing.
The prospective sixteen-pair read confirmation adds ten pairs versus the original
six: about 3 seconds of target SQL per suspect read, 9 seconds for three suspects
or 39 seconds for all thirteen PR reads. These estimates exclude checkpoint,
warmup and other overhead and are not runtime or precision guarantees. No prior
inconclusive run is reinterpreted or pooled under this new policy.

`--budget-seconds` defaults to a **global** 300 seconds including source/build,
cluster setup, fixtures, validation, reset, measurement, and size lifecycle checks.
Subprocesses use the remaining deadline and an expired build process group is
terminated. SQL checks the deadline before and after reset, operations, and
validation; an already running statement can overrun it by at most the 30-second statement timeout. Cleanup has
a separate bounded stop allowance and always contributes to reported total time.
CI passes the timestamp recorded before checkout. The driver deducts all prior
workflow elapsed time from 300 seconds and reserves 30 seconds for cleanup,
reporting and upload; it never starts with a fresh 300 seconds after dependencies.
An exhausted remaining allowance reports inconclusive (exit 2) before building.
Active cases are registered before fallible work; interruption preserves completed
timing verdicts and partial raw arrays. Normal verdict precedence is error3,
known confirmed timing or valid size regression1, incomplete2, complete all-pass0.
Completion, size coverage and budget exhaustion are separate artifact fields;
a regression can coexist with incomplete coverage. Size PASS requires every
planned lifecycle state. An initial suspect alone is never a known regression.
The report records nominal budget, prior elapsed, reserve and actual remaining
allowance. This is a soft end-to-end budget: dependency steps are not individually
bounded to it, in-flight SQL and cleanup can overrun, and upload follows the
reported workflow time. The separate 20-minute CI job timeout is a safety ceiling,
not proof of meeting the three to five minute target. Final-head hosted duration
and checkpoint cost still require measurement on the actual runner.

`results.json` and `summary.md` include exact revisions, UTC date, PostgreSQL and
compiler versions, CPU/runner/load, metric/configuration, fixture digest, raw
initial/confirmation pairs, operation count, spread, ratios/intervals/status,
plans, size probes, and setup/measurement/cleanup runtimes. The schema4
`terminal-accounting.json` records actual driver/workflow elapsed through the
closed final reducer/writer, the complete phase ledger and its exclusive
top-level sum plus nonnegative residual. Child phases are explanatory, never
additional demand. `results.json` explicitly labels its pre-final-writer runtime.
The sidecar write, device cleanup and artifact upload are outside that boundary.
A final-writer envelope overrun cannot return PASS. Build and server
logs are retained. Errors produce these reports too. Scratch clusters stop and
owned temporary files are removed; failures to stop are errors and preserve
scratch for inspection.

For focused methodology tests:

```sh
python3 -m unittest discover -s bench/perf -v
```

For explicit end-to-end acceptance, use equal SHAs and `--verify-control`.
Normal comparisons have no injection flags. Acceptance retains unchanged point
lookup, candidate SQL `pg_sleep(0.075)` once per read batch, and actual BM25
`build_segments=64`/REINDEX growth with unchanged logical rows. All catalog/read
checks precede normal churn/size capture, then BM25 growth/logical/fork proof.
These already-required phases execute once, before clean INSERT. The versioned
acceptance schedule deliberately changes physical/cache/WAL history; seed and
mutable fixtures remain independently checked. Captured normal sizes remain
separate from intentional growth.

Before clean INSERT, acceptance persists an incomplete, not-admitted setup intent,
then measures ONE inactive infrastructure install/drop and ONE stop/restart of
the existing clusters. Creation/disable/counter initialization is a DDL-only
transaction; no mutable INSERT occurs during this probe. The disabled trigger
cannot fault seed or new rows. Absence is checked after removal; restart reuses
the same data/binary/log, reconnects through the same SQL owner, loads extension
GUC definitions, reapplies settings and verifies cluster/seed/version/settings
identity. There is no retry, rebuild, initdb or fixture reload on restart.

The same inactive helper supplies one declared CPU calibration: fixed32 disposable
non-workload inputs, warm then measured, at zero and16 md5 iterations, with raw
client timings and deterministic checksums. No trigger fires or mutable row changes.
A nonpositive/nonfinite incremental slope is incomplete. After clean calibration,
freeze L= min(ceil(slower clean SQL seconds per row / measured seconds per
iteration),1024). This targets a large functional effect, not an11% sensitivity
claim or a selected passing control. The active BEFORE INSERT trigger performs
exactly L deterministic md5 iterations and increments a session counter only for
IDs above the seed maximum, retaining payload. Last-row checksums and counts
around every reset/warm/measured stream prove the path and exactly26*K*N candidate
new rows across controlled validation+initial6+mandatory fresh6. Seed restoration
performs predicate overhead but no counted CPU work. There is no per-batch delay,
sleep, under-fault calibration, retry or verdict-driven control selection.

Only complete clean full-N/K validation/verdict observations fund the observed
maximum M; the2048 pilot is excluded even when N=2048. Each observation wall
already includes all K histories, both sides, checkpoints, waits and residuals.
The same-run ledger supplies future setup/removal estimates from the inactive
cycle, teardown from stop/log-copy, and reporting from closed real writes. A closed
report containing clean INSERT evidence refreshes the report estimate before
admission. Every estimate carries exact sources/tier/settings and phase provenance.
Completed read/catalog/churn/BM25 work is already spent, never charged again.

The existing writer persists immutable `mutation_control_preflight` before any
ACTIVE fault installation/enable: source/version/seed/N/K/orders, full raw pilot,
clean validation/blocks, CPU calibration/frozen work, completed prerequisites and
cost evidence. After the writer closes, re-read the FRESH remainder. With reserve
R, measurement horizon Dm=workflow deadline-R and current time t, admit only if:

```text
Q = 13*M + 26*K*N*predicted_added_CPU_seconds_per_row + measured_setup_estimate
F = measured_removal_estimate + measured_report_estimate + measured_stop_estimate
Q <= Dm-t                 and                 Q+F <= Dm+R-t
```

Missing/incomplete/nonfinite/wrong-source evidence is incomplete before activation,
never zero or historical substitution. The configured30s allowance appears once.
Observed maxima and measured estimates are NOT bounds. Helper calibration excludes
trigger predicate/counter/enable overhead; seed work, future dirty-stop differences,
report growth, scratch removal and upload variability remain explicit unknowns.
Actual removal/final teardown/writer durations remain in the ledger. Cleanup
failures cannot demote a primary SQL/correctness error. An expected injected
regression is acceptance evidence, not a normal gate failure; acceptance0 requires
unchanged read+INSERT, real per-row fresh6 lowerCI>1.10, all normal sizes and actual
BM25 growth detection, complete evidence and successful cleanup. No source-only
implementation or cheap test pass establishes real precision, duration or readiness.

`--insert-feasibility` remains a separate no-fault diagnostic requiring equal exact
commits, PR tier, committed harness identity, no tree overrides/groups/controls.
It runs one2048 history pilot, full-N/K validation, initial6 and mandatory fresh6:
1+13*K history pairs, 2*(1+13*K) restoration checkpoints and4*(1+13*K) restores,
plus two setup checkpoints. It omits normal gate/acceptance and their coverage.
Exit0 requires BOTH independent upper CIs<=1.10 and all INSERT evidence/cleanup;
exit1 is fresh lowerCI>1.10 or valid size discrepancy; exit2 is imprecision,
incomplete evidence or budget; exit3 is SQL/correctness/source/cleanup error.
No pooling/retry/early pass occurs. A diagnostic0 neither resolves unknown
production-tail estimates nor clears the normal/acceptance/hosted gate.

Temporary INSERT SQL restoration/removal preserves any existing SQL/correctness
error if cleanup also expires; both failures remain in case/oracle evidence. A
standalone cleanup SQL error remains exit3, and ordinary deadline-only interruption
remains incomplete/nonzero. Cluster teardown remains the final cleanup owner.
