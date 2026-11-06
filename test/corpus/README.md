# Corpus gate

A standing differential and plan regression for the aggregate and column paths: `cases.sql` holds 323 statements (the shapes of the pushdown families, one `-- @case slug | family | note` marker each) that run on the lab of `lab.sql` (20000 events and the tables around them, deterministic: `setseed(0.42)`).

Every statement runs twice, each time in a **new backend** (`\connect`, so no cache, relcache entry or memory context of an earlier statement is warm):

1. sift off (`sift.enable_*=off`): the native planner;
2. sift on.

It fails when

- the sorted rows differ (`extra_float_digits=-5`), or a statement errors;
- the **plan slug** is not the pinned one in `plans.tsv`: the Custom Scan (Sift...) nodes of `explain (costs off)` and the verdict of `sift.explain_why` per arm (`t` used, `f:SLUG` refused). A statement that stops engaging, starts engaging, or is refused for another reason is a failure. Changing the set is allowed with `scripts/corpus.sh --update` **in the same commit**: read the diff of `plans.tsv`, a statement that left the engaged set is a regression unless the change says so.

A node `~` in `plans.tsv` leaves a statement whose engagement is a cost call that differs across majors unpinned (rows are still compared); `--update` keeps it.

## Run

    scripts/corpus-local.sh                # one command: installs the library, starts a throwaway cluster, runs the corpus, removes the cluster
    scripts/corpus.sh                      # the same on the cluster of scripts/pgtest.sh (SIFT_TMP, SIFT_PORT; installed and started), as the CI job does
    scripts/corpus.sh --only 'agg_*'       # a focused run (the statements whose slug matches; the lab is loaded as ever)
    scripts/corpus.sh --update             # re-record plans.tsv
    CORPUS_ON_SET='set sift.agg_dense=off;' scripts/corpus.sh   # try a setting: what it does to rows and plans

On an installed cluster it takes **about 2 s** (lab load 1 s, statements 1 s, PostgreSQL 18); `corpus-local.sh` adds the build and install (about 40 s the first time). The CI job is `corpus` (every major; the pinned plans are the same on 17, 18 and 19 apart from the `~` rows).

## Add a statement

Append a `-- @case` block to `cases.sql` (the header of the file lists the markers: `@pre` setup of the session, `@txn` a transaction rolled back after, `@K@`/`@K2@` the entity keys), run `scripts/corpus.sh --update`, and commit `cases.sql` with the new line of `plans.tsv`.
