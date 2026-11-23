# Corpus gate

A standing differential and plan regression for the aggregate and column paths: `cases.sql` holds the statements (the shapes of the pushdown families, one `-- @case slug | family | note` marker each) that run on the lab of `lab.sql` (20000 events and the tables around them, deterministic: `setseed(0.42)`).

Every statement runs twice, each time in a **new backend** (`\connect`, so no cache, relcache entry or memory context of an earlier statement is warm):

1. sift off (`sift.enable_*=off`): the native planner;
2. sift on.

It fails when

- the sorted rows differ (`extra_float_digits=-5`), or a statement errors;
- the **plan slug** is not the pinned one in `plans.tsv` (`plans-f1.tsv` for `--format f1`): the Custom Scan (Sift...) nodes of `explain (costs off)` and the verdict of `sift.explain_why` per arm (`t` used, `f:SLUG` refused). A statement that stops engaging, starts engaging, or is refused for another reason is a failure. Changing the set is allowed with `scripts/corpus.sh --update` **in the same commit**: read the diff of `plans.tsv`, a statement that left the engaged set is a regression unless the change says so.

A node `~` in `plans.tsv` leaves a statement whose engagement is a cost call that differs across majors unpinned (rows are still compared); `--update` keeps it.
A row `slug@19` pins the plan of that statement on PostgreSQL 19 where the major's planner gives a label the others do not (the row without `@` is the pin of the other majors); `--update` keeps those rows as they are, and is run on PostgreSQL 18. The two rows there: PostgreSQL 19 turns the semi join of an `IN` over a UNION into an inner join, which the two level analysis then reads and refuses (`two_level:f:two_arms`), while the scope read of the statement is the same.

## Formats and tables

The corpus runs on the format of the library and on both storages, with one set of statements:

| command | library | indexes of the lab | plans pinned in |
|---|---|---|---|
| `scripts/corpus.sh` (`--format f2`) | testing | Format 2.0 | `plans.tsv` |
| `scripts/corpus.sh --format release` | release (no test setting; refused on a testing library) | Format 2.0 | `plans.tsv` |
| `scripts/corpus.sh --format f1` | testing | Format 1 | `plans-f1.tsv` |
| `scripts/corpus.sh --table` (with any of the formats) | as above | the base tables are `sift_table` (`table.sql`), no `sift_col` index | rows only |

The plan slug names the `Custom Scan (Sift...)` nodes whole (`SiftAgg` of Format 1, `SiftAggV6`, `SiftAggV6Sip`, ... of Format 2.0): a statement that moves from one node to another of the family is a change of the plan and fails until the pins say so.

## Run

    scripts/corpus-local.sh                # one command: installs the library, starts a throwaway cluster, runs the corpus, removes the cluster
    scripts/corpus.sh                      # the same on the cluster of scripts/pgtest.sh (SIFT_TMP, SIFT_PORT; installed and started), as the CI job does
    scripts/corpus.sh --only 'agg_*'       # a focused run (the statements whose slug matches; the lab is loaded as ever)
    scripts/corpus.sh --update             # re-record plans.tsv
    CORPUS_ON_SET='set sift.agg_dense=off;' scripts/corpus.sh   # try a setting: what it does to rows and plans

On an installed cluster it takes **about 2 s** (lab load 1 s, statements 1 s, PostgreSQL 18); `corpus-local.sh` adds the build and install (about 40 s the first time). The CI job is `corpus` (every major; the pinned plans are the same on 17, 18 and 19 apart from the `~` rows and the `@19` rows).

## Add a statement

Append a `-- @case` block to `cases.sql` (the header of the file lists the markers: `@pre` setup of the session, `@txn` a transaction rolled back after, `@K@`/`@K2@` the entity keys), run `scripts/corpus.sh --update`, and commit `cases.sql` with the new line of `plans.tsv`.
