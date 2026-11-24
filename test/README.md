# Tests

Layout (run with `scripts/pgtest.sh regress [test...]`, which starts a throwaway cluster; `make installcheck` against a running server):

With test names, one pg_regress run covers all of them. A crashed backend stops the run and names the test; a run that only differs runs the tests that differ alone; only a leak or a leftover cuts the batch in halves. `SIFT_REGRESS_NO_BISECT=1` never runs a test again (a fast local loop or an agent script): the findings of the batch and exit 1.

| path | what |
|---|---|
| `sql/`, `expected/` | pg_regress tests (one `.sql` per test, its output in `expected/`, a second `_1.out` for a version-dependent output) |
| `isolation/` | concurrent-session tests (specs, expected output) |
| `t/` | TAP tests with clusters of their own (crash, standby, WAL) |
| `col/` | the column index differential harness: the same statements with the index on and off, rows compared |
| `fuzz/` | the differential fuzzer |
| `plan/` | planning and execution budgets per statement family |
| `corpus/` | the corpus gate: 174 statements in a new backend each, sift off against on, the plan slug of each pinned (`scripts/corpus.sh`) |
| `matrix/` | data-type matrix |
| `fixtures/` | binary fixtures of old on-disk formats |

## Test budget

A test proves its invariant at the smallest deterministic scale that crosses every boundary it names (a page, a segment, a granule, a batch, an overflow state). Brute-force loops, millions of rows, a reference recomputed per case and fixed multi-second sleeps do not belong in a PR run; the deep versions stay behind the knobs of the full tier (`SIFT_CI_TIER=full`, `SIFT_FUZZ_ROUNDS`, `SIFT_ORACLE_SEEDS`).

| suite | budget | enforced by `scripts/lint.sh` |
|---|---|---|
| `unit/` | each binary at most 1 s plain and 3 s under ASan or TSan; `ROWS` at most 4000 and fuzz rounds at most 500, unless a `__SANITIZE_THREAD__` / `__SANITIZE_ADDRESS__` branch scales the constant down | `test-budget` |
| `sql/` | a test runs in about 2 s and never past 5 s; a `generate_series` above 50000 rows carries a comment that says what the size proves; a table that only needs to cross a segment uses the `sift.test_*` size knobs instead of rows | `test-budget` |
| `SLOW_REGRESS` (`scripts/ci/weights.sh`) | the list shrinks and never grows: a slow test is made small, not listed | `test-budget` |
| `t/` | a PR run of a script takes at most 15 s through `knob` of `lib.bash`; multi-seed and soak loops run in the full tier only; a state is awaited with `wait_for` (shell) or `poll_query_until` (perl), never with a `sleep` of half a second or more | `tap-sleeps` |

`scripts/test-budget.allow` and `scripts/tap-sleeps.allow` list the sites that exist today with their count. A count only goes down: a new site, a count that grew and a row whose site was fixed (delete it, or lower the count) fail the check. Fix a site by scaling it down, not by listing it.

Verification scripts for the sanitizer flavours follow the same budget:

- run `make MODE=tsan test` once and do not run the same binaries a second time by hand;
- never wrap an ASan or TSan run in `ulimit -v`: the sanitizers reserve a huge address space and fail at startup under a virtual memory limit (bound the run with a timeout instead).

## Scope markers of the testing flavour

The test flavour (`SIFT_TESTING=1`, the extension `pg_sift_test`) has two markers that name a scope conjunct without the join an application writes: `sift.in_scope(index, key_name, fk, key or keys)` is the scope read of the keys, and `sift.in_scope_pairs(index, key_name, fk, key or keys)` has the meaning of the join (a fact once for each pair), so that a test can say `EXPLAIN` of a scope plan or compare one with the other. They exist only in `sql/pg_sift_test--1.0.sql`; the release library and the user pages ([docs/COL-SCOPE.md](../docs/COL-SCOPE.md)) know only the join, `IN`, `EXISTS` and the other forms the planner recognises.

## Synthetic data

Every test builds its data with `generate_series` and a seeded generator. The domain is declared synthetic and stays small: **orders**, **events**, **docs** and
**facts**, with **customers** and `country_code`. No test holds real-looking data, and none names an application.

## Naming vocabulary for new tests

A name says what a column or key *is* (its type and role), never where it came from. Use these in new tests, benchmarks and fixtures:

| role | name |
|---|---|
| integer references | `foreign_id`, `foreign_id_b`, `parent_id`, `ref_id`, `dim_a_id`, `dim_b_id`, `link_id` |
| partition and group keys | `tenant_id`, `set_id`, `category_id` |
| enums | `status` (smallint), `kind` (text), `lvl3` (three ordered labels `lo`, `ok`, `hi`) |
| timestamps and dates | `created_at`, `updated_at`, `deleted_at`, `event_date` |
| jsonb | `metrics` / `metrics_a` (object of numbers), `items` / `tags_a` (array of objects with a `type` key), `doc` / `doc_b` |
| scalars | `counter_a`, `counter_b`, `a_count` (a counter the index treats as often rewritten), `label`, `label_a`, `body`, `title`, `flag_a`, `value_num`, `value_txt` |
| tables | `dim_a`, `dim_b`, `dim_c` (dimensions), `tenants`, `key_sets`, `orders`, `events`, `docs`, `facts` |

The column index decides on some names (`updated_at`, a `_count` suffix, an `_id` suffix: the index cuts by key columns picked by it); keep those suffixes where a test depends on them.

`scripts/check-neutral.sh` (the `neutral` step of `scripts/lint.sh`) fails a change that brings back a name this vocabulary replaced. The list of replaced names is not in
the repository: the check reads it from `NEUTRAL_VOCAB_FILE` (a file) or `NEUTRAL_VOCAB` (the text of a CI secret); without it only the structural rules run and the check
passes. `scripts/apply-rename.sh DIR` rewrites a tree to the vocabulary with the same private list (`--check` only lists the files that still need it).

## Private overlay

Tests and data that are not part of this repository run from a directory of the maintainer's, laid over the work tree:

```
SIFT_PRIVATE_TESTS=~/private-tests scripts/private-overlay.sh sync    # copies its files to the same paths and keeps them out of git
scripts/pgtest.sh regress ...                                         # the overlaid tests are part of the run (the Makefile takes every test/sql/*.sql)
scripts/private-overlay.sh clean
```

Without `SIFT_PRIVATE_TESTS` the sync does nothing and succeeds; CI has no overlay and never depends on one.
