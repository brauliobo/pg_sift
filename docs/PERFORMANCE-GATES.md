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
host or lower variance. Mutable resets are unchanged and receive no added checkpoint.

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
oracle. Mutable cases truncate and logically reload the same state before **every
operation**, outside timing. VACUUM starts with a fixed contiguous deleted range.
Its validation advances the xid horizon, runs a second untimed VACUUM, and requires
additional reusable pages or file shrinkage. Post-maintenance mutable table and
its B-tree index are each gated through the same relative-plus-page-tolerance size rule below, separately from time.

Index and table sizes are independent gate decisions. The settled-build and
settled-churn/VACUUM states compare BM25 `docs_bm25`, column `docs_col`, and
`sift_table` `store` with identical logical data/options in both revisions.
Churn updates indexed text and amounts, deletes fixed rows, and runs two VACUUMs
with an intervening xid advance. All main/FSM/VM/init fork bytes are included.
Each relation passes when candidate bytes are at most the larger of baseline
bytes times 1.10 (rounded up) and baseline plus two 8 KiB pages. The two-page floor
avoids rejecting harmless page rounding in tiny indexes. A larger result is a
regression, independently of elapsed-time results; missing/invalid SQL measurements
are errors, while interrupted measurements leave an inconclusive run. Reports
separate extension indexes, auxiliary B-tree indexes, ordinary heap bytes, table
storage, and total footprint, with bytes/pages/ratio/threshold/state/verdict.
Heap and immutable auxiliary B-tree sizes are reported, not extension-size
gates; the mutable B-tree is also gated at its VACUUM lifecycle state. The normal
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

The material threshold is 1.10. An initial upper bound at most 1.10 passes.
Every other case receives exactly one fresh confirmation block: **sixteen pairs
for reads without resets, six pairs for mutations**. This policy is fixed before
sampling, with the original 150 ms target and the same calibrated operation count
for both blocks. No post-verdict calibration or sample reuse occurs. Both blocks
and every raw sample are retained separately, including interrupted blocks. Only the confirmation decides: lower bound above 1.10 is
a regression (exit 1), upper bound at most 1.10 passes, and overlap is inconclusive
(exit 2). Exhausted measurement budget is also inconclusive. No noisy cell becomes
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

For explicit end-to-end acceptance, use equal SHAs and `--verify-control`. It
runs unchanged point lookup, then the same query with a candidate-side SQL
`pg_sleep(0.075)` once inside each candidate timed batch. Acceptance exits zero
only when
unchanged passes, the real SQL slowdown produces a confirmed regression, normal
settled/churn size checks pass, and a real SQL index-size fault is rejected.
The size fault explicitly changes candidate `docs_bm25` to `build_segments=64`
and REINDEXes it, leaving logical table rows unchanged. This deliberate option
difference exists only in acceptance and is labeled `control_build_segments_64`.
Normal PR comparisons never alter one side's options. The report retains the
injected timing and size regressions as such; acceptance success does not
turn it into a normal gate pass. The sleep is available only in this explicit
mode; there are no normal PR injection flags or environment controls. Do not use
acceptance mode for a PR comparison.
