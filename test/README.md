# Tests

Layout (run with `scripts/pgtest.sh regress [test...]`, which starts a throwaway cluster; `make installcheck` against a running server):

| path | what |
|---|---|
| `sql/`, `expected/` | pg_regress tests (one `.sql` per test, its output in `expected/`, a second `_1.out` for a version-dependent output) |
| `isolation/` | concurrent-session tests (specs, expected output) |
| `t/` | TAP tests with clusters of their own (crash, standby, WAL) |
| `col/` | the column index differential harness: the same statements with the index on and off, rows compared |
| `fuzz/` | the differential fuzzer |
| `plan/` | planning and execution budgets per statement family |
| `matrix/` | data-type matrix |
| `fixtures/` | binary fixtures of old on-disk formats |

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
