# Focused performance gates

`python3 bench/steps/perf_gate.py --help` describes the standalone gate. It requires
PostgreSQL 18 development tools, a C compiler, make, Python 3, and libpq.

```sh
python3 bench/steps/perf_gate.py \
  --baseline-ref FULL_MERGE_BASE_SHA --candidate-ref FULL_PR_HEAD_SHA \
  --groups smoke,bm25,agg,storage --tier pr \
  --scratch-root OWNED_SCRATCH --output ARTIFACT_DIRECTORY
```

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
point lookup. These run on every code PR regardless of selected groups. `bm25`,
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
elapsed ratios are logged; their mean and sample standard deviation give a
geometric ratio and two-sided 95% Student-t interval. Supported complete blocks
are six pairs (df 5, critical value 2.570581835636305) and sixteen pairs
(df 15, critical value 2.131449545559323); partial blocks have no interval.
Repeated operations within a batch are not treated as independent samples.

INSERT uses lifecycle `insert_trajectory`, experimental workload
`committed_append_settled_pair_v3`. Historical v2/one-row timings are not
interchangeable with this metric. One pair owner uses fixed order O: AB,BA,AB,BA,
AB,BA independently in each block; standalone pilot and validation are AB:

1. Warm both sides in O: configure, restore seed, N separate autocommitted points.
2. Final restore and all pre-stream counter/geometry/raw-fork probes on both in O.
3. Synchronous CHECKPOINT on each in O; BOTH must finish before either stream.
4. N individually completed point statements on each in O, with no SQL/probes/
   configure between streams. SQL timer sums remain separate from loop wall.
5. Deferred counters, seed/full-row multiset/count/digest and geometry/fork checks
   on both in O. Publish the pair only after BOTH checks pass.

There are exactly two restores per side, no inner reset or extra checkpoint.
Before-forks are explicitly pre-finalization observations; after-forks are
sequential pair-end observations. Full-N validation gates mutable and mutable_pk
separately; sample footprints corroborate that state, not select a favorable one.
Partial phases/raw timings survive failure, without a valid pair or partial CI.
Warm/reset/probe/checkpoint/stream/check UTC and common process-origin monotonic
offsets expose unequal ages. Pair wall includes both sequential checkpoints,
all warm/reset/probe/loop/check/wait and residual bookkeeping exactly once;
phase costs explain wall, and overlapping ages are not additional costs.

IDs are seed maximum+1 through+N, checked against both seeds and the full integer
ID domain before any insertion. Payload is unchanged, including the compressible
1,500-byte pad. One sample is N autocommitted point requests; no BEGIN wrapper,
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
waits/checks remain in the global budget. A separate full-N validation follows;
there is no new pilot or recalibration under a fault/after a verdict. If N<2048,
the longer pilot's growth/warm history need not model N linearly.

This measures non-durable append after paired settled restoration, not durable
commit latency. It changes dirty/writeback/redo/recycling and cache age; fsync and
full_page_writes remain off. A waits through B's checkpoint, B through A's append;
slow candidate warm work can age the opposite side. Alternation does not equalize
ages or prove independence. Natural actual-append/time/other-cluster activity and
checkpoint overlaps remain in the samples, with no wait-until-quiet, exclusion or
retry. Restoration/checkpoint cost increases can be absent from the point ratio;
full wall costs remain visible, without a separate 10% restoration gate. Historical
~39ms windows at cap2048 achieved ~26% of150ms; neither the full pilot nor settling
proves target attainment, improved precision, adequate power or budget feasibility.

The material threshold is 1.10. An initial upper bound at most 1.10 passes.
Every other case receives exactly one fresh confirmation block: **sixteen pairs
for reads without resets, six pairs for mutations**. This policy is fixed before
sampling, with the original 150 ms target and the same calibrated operation count
for both blocks. No post-verdict calibration or sample reuse occurs. Both blocks
and every raw sample are retained separately, including interrupted blocks. Only the confirmation decides: lower bound above 1.10 is
a regression (exit 1), upper bound at most 1.10 passes, and overlap is inconclusive
(exit 2). Exhausted measurement budget is inconclusive unless completed normal
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
plans, size probes, and setup/measurement/cleanup/total runtimes. Build and server
logs are retained. Errors produce these reports too. Scratch clusters stop and
owned temporary files are removed; failures to stop are errors and preserve
scratch for inspection.

For focused methodology tests:

```sh
python3 -m unittest discover -s bench/perf -v
```

For explicit end-to-end acceptance, use equal SHAs and `--verify-control`.
It retains unchanged point lookup, candidate SQL `pg_sleep(0.075)` once per read
batch, and the real BM25 `build_segments=64`/REINDEX size fault with unchanged
logical rows. It also requires unchanged full INSERT trajectory PASS, then a
candidate-only BEFORE INSERT row trigger with `pg_sleep(.001)` for IDs above
seed maximum. The clean common N is frozen before installation and reused for
all control validation/warm/measured work and the fresh six-pair confirmation.
The trigger counts new-row delays using a session counter; counts around each
restore/warm/measured phase must prove exactly N per stream and no delayed seed
reload rows. Predicate/function overhead still affects seed restoration; the
counter and scheduler rounding add control overhead and are not subtracted.
The trigger is removed before unrelated lifecycle work. Normal comparisons have
no injection flags or environment controls.

Before installing a trigger, the existing writer persists immutable
`mutation_control_preflight`: refs/version/identity/N, full raw2048 AB pilot,
caps/windows, full-N clean validation/verdict phase evidence and control budget.
The estimate is `13 * max_clean_full_N_pair_wall + 26*N*.001 + required post-control
costs`. The maximum population contains ONLY complete finite CLEAN full-N
validation/verdict pairs, never the pilot (even if N equals2048). Pair wall already
includes both sides, checkpoints, waits and residuals; nested phase costs are not
added again. The13 pairs include controlled validation and fixed6+6; minimum
candidate warm+measured sleep is26*N*.001s (53.248s at2048), once only.

Required disjoint post-control evidence covers fault removal, churn/normal sizes,
BM25 ALTER/REINDEX, logical/size-control probes, final reporting and teardown,
each with measured duration and provenance. The current workflow has NOT_MEASURED
costs for these future phases before control. **Full acceptance is therefore NOT
admissible under this experimental implementation.** Missing/incomplete/nonfinite
cost evidence means inconclusive before fault installation; no zero, historical
setup-checkpoint substitution, invented multiplier or unmeasured savings is used.
The INSERT-only experiment below cannot fill these post-control costs. A later
explicit design must resolve their availability before full acceptance can run.

After preflight writing returns, admission re-reads the FRESH global remainder
before first fault SQL, without overwriting the immutable snapshot. Known demand
exceeding that remainder is nonzero. Startup is already elapsed; extra trigger
seed predicate/counter/DDL/scheduler variability remains unknown. An observed
maximum plus configured minimum sleep is an estimate, not a full runtime bound.
The same global deadline/reserve remains authoritative. All meaningful controls,
exact selected comparison, hosted duration and larger nightly remain unresolved.

`--insert-feasibility` is a separate experimental diagnostic, requiring equal exact
commits, PR tier, committed driver/catalog/fixture identity, no tree overrides,
no groups and no verify-control. It shares the fresh archives/builds/fixture/setup
barrier and v3 pair owner; skips all read/UPDATE/VACUUM/settled/churn/controlled
fault work. It ALWAYS runs one2048 pilot, full-N validation, initial6 and fresh6:
14 pairs/28 restoration checkpoints/56 resets, plus the two setup checkpoints.
No pooling/retry/early pass occurs. Reports persist source/configuration before
measurement, full validation before initial, each block, raw phases and costs.
Normal gate/acceptance and omitted coverage are explicitly NOT evaluated.

Diagnostic exit0 requires BOTH separate upper CIs<=1.10, complete INSERT evidence,
logical/geometry/fork/size checks and cleanup. Exit1 flags fresh lowerCI>1.10 or
valid size discrepancy; exit2 covers either block's imprecision, incomplete
coverage/evidence or budget; exit3 covers SQL/correctness/source/cleanup errors.
An initial imprecise block remains diagnostic2 even if fresh passes. Normal
complete/size_complete stay false; diagnostic completion/status are separate.
Target fractions/attainment are explicit; a diagnostic0 with short SQL windows
still establishes neither adequate window/power nor gate readiness.

Proposed next experiment, ONLY after independent review and separate runtime/
slot/device admission, is ONE180s whole-workflow budget minus actual prior elapsed
and30s reserve, nice19/max2workers, with equal final reviewed SHA. Use explicit
`--budget-seconds 180 --cleanup-report-reserve-seconds 30`; no automatic escalation,
repeat, controls, selected run or nightly follows. SQL/teardown make this a soft
envelope, not a promised180s fit. The review handoff records the complete proposed
command and evidence/cleanup requirements. This documentation authorizes no run.

Temporary INSERT SQL restoration/removal preserves any existing SQL/correctness
error if cleanup also expires; both failures remain in case/oracle evidence. A
standalone cleanup SQL error remains exit3, and ordinary deadline-only interruption
remains incomplete/nonzero. Cluster teardown remains the final cleanup owner.
