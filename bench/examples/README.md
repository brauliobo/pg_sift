# Typified examples

A library of named scenario classes to test and measure sift indexes that know about joined tables, and to serve as the baseline against native
PostgreSQL and the existing sift join machinery. Every example is a class with explicit type tags, so what the library covers can be shown (`run-example
--coverage`) and what it misses is a list of gaps, not a feeling.

An example is a directory under `examples/<id>/`:

| file | content |
|---|---|
| `schema.sql` | generic tables and primary keys (no foreign keys, as in an analytics schema) |
| `gen.sql` | the generator: SQL functions `gen_<table>(lo, hi)` and the inserts; plain SQL, `generate_series` and hashing through `gx.*` (`lib/gx.sql`), no files |
| `btree.sql` | the b-trees a user would normally have (join keys, filter columns) |
| `sift.sql` | the sift_col indexes as built today ("unconnected") |
| `connected.sql` | the slot for the connected variant; notes only until a design fills it (`designs/README.md`) |
| `links.tsv` | the joins of the example (child, parent, relation, the parent attributes the queries read): what a design builds its DDL from |
| `queries.sql` | the typed queries, each with a header `-- query:` `-- kind:` `-- join:` [`-- ordered: yes`] [`-- empty: ok`] [`-- sift: <one-line SQL>`] [`-- oracle: <one-line SQL>`] [`-- plan: SiftAgg`] |
| `writes.sql` | the write workload, batches with a header `-- batch:` `-- pattern:` `-- rows:` [`-- concurrent: no`] [`-- maintenance: yes`] [`-- sift_only: yes`, skipped on the native engine] [`-- rounds: N`] |

`registry.tsv` has one row per example (class, scale ratio, knob defaults, tags); `vocab.tsv` is the tag vocabulary; `ready.tsv` lists the examples that
passed `--validate` (an example edited afterwards is draft again: the digest of its files is in the row).

## Run

```sh
eval "$(bench/examples/cluster.sh start)"          # a scratch cluster on /dev/shm (EX_ROOT = a staged sift install, see cluster.sh)
bench/examples/run-example star-orders S native
bench/examples/run-example star-orders S native,sift-unconnected,sift-off --phases load,queries,writes,concurrency
bench/examples/run-example star-orders S connected:<design>
bench/examples/run-example star-orders S x --validate    # native + sift-unconnected, queries + writes, oracle must hold: marks the example ready
bench/examples/run-example --list | --check | --coverage [--ready]
# --exact-oracle: the oracle runs the main text, not the `-- oracle:` rewrites (small scales; proves the rewrite changed nothing)
# narrowing a mismatch: --only QUERY_IDS (comma list), --skip BATCH_IDS (concurrency), --set "SET guc = v" (measured sessions, repeatable), --knob k=v
bench/examples/cluster.sh stop
```

Scales: `S` = 100,000, `M` = 1,000,000, `L` = 10,000,000 fact rows; the dimensions scale by the example's ratio (`nd_div`: fact rows per dimension row,
or a fixed `nd_min`). A number is a row count for development. M and L run through the heavy-job queue of the host and, for L, on a large-memory host with the cluster on tmpfs.
Knobs (`--knob k=v`, defaults in the registry row, all deterministic through the seed): `skew` (1 uniform, > 1 power law toward the first keys),
`hot` (percent of the facts on dimension row 1), `null_pct`, `card` (cardinality of the dimension attributes), `corr` (`random`, `clustered`, `anti`: how
the join key follows the heap order), `seed`.

Engines: `native` (b-trees, no extension, parallel plans on), `sift-unconnected` (sift_col as `sift.sql`, `sift.enable_agg` on), `sift-off` (the same
structures with the sift planner steps off: what the write side pays without a read benefit), `connected:<design>`.
The cluster has autovacuum and the sift background worker off (`EX_MAINTENANCE=on` leaves them on); the runs say so in `run.json`. Every load ends with
`VACUUM (ANALYZE)`; a table whose pages are not all visible is reported.

## Output

`<run_id>/` under `bench/out/examples/` (untracked; `--out` or `EX_OUT` name another directory):

| file | rows |
|---|---|
| `queries.tsv` | example, scale, engine, query, kind, join, `ms` (median of 3 warm, EXPLAIN ANALYZE execution), `instr_m` (millions, whole process tree, perf stat), `cold_hit` / `cold_read` / `cold_ms` (relations evicted first), `warm_hit` / `warm_read`, `plan` (distinct node types, `Custom Scan:SiftAgg` for the pushdown), `sift_used`, `rows_equal`, `nrows`, `trivial` |
| `writes.tsv` | one row per batch: rows, ms, `wal_bytes`, `wal_per_row`, growth of heap, b-tree indexes and other indexes (sift_col, sift, gin) in bytes, `index_bytes_per_row`, `oracle_ok` (every query compared with the oracle right after the batch), `mismatches` |
| `load.tsv` | generator and index build times, the rows of each table, the size of each index |
| `concurrency.tsv` | readers, writers, reads, snapshot reads, mismatches, torn snapshots, deadlock retries, errors, `ok` |
| `run.json` | run id (git tip, UTC time), example digest, knobs, engines, failures |

## Connections

`run-example <id> S connected:connected-b` connects each n:1 link of the example (`links.tsv`) the way a user would by default: the child's `sift_col` index
gets `connect = 'fk -> parent(key)'`, and the join reads the parent's columns from the leaves of the parent's own index. `links.tsv` names the parent
attributes the queries read (`reads`). The `parent-read-*` examples read one parent column of each kind (int, date, text, all three) through the connection
while the parent changes. A link with a composite or expression key, a key that is not an integer primary key, the table itself as parent, a partitioned
child or a sift_table fact stays unconnected. The parent-side shapes (EXISTS, NOT EXISTS, IN with a child filter, LEFT JOIN counts, latest child) are the
`parent-child-*` examples. Both are written with the parent updates that matter to a connection: an update that keeps the key, a key change, a delete and a
reinsert.

## When to connect

`connected:connected-b` against `sift-unconnected`, connection only (each n:1 link as `fk -> parent(key)`, no carried value), on the 4.3 line: every example at
`S` (100,000 fact rows), 15 examples at 1,000,000 rows and 2 at 5,000,000; warm medians of 3 and instructions of the whole process tree, one cold read per
query, same oracle. Sweep: `run-example <id> <rows> sift-unconnected,connected:connected-b --phases load,queries,writes --patterns append_only,dim_attr_churn,key_churn,delete_heavy`.
The numbers are lab numbers of one run, not promises.
The lab cluster runs `maintenance_work_mem = 256MB` (the default is 64MB). After the load, `run-example` runs `sift.col_maintain` on each connected child
and records for every connection the parent key count, the `sift.col_connections` `fill` before and after, and the effective `maintenance_work_mem` in `load.tsv`
(step `connection`); it fails the run unless `fill` is `filled` (`stamp` is only the state of the parent's journal), so a connected run never measures an
unfilled or refused connection. Every connection above was `filled` before and after, checked on 4.3 `d92130075`; the parent key counts:
`orphan-sessions-not-exists` 100,000 and `orphan-facts` 42,567 (1M rows), `parent-read-*` 20,000, `tags-many-to-many` 100,000 (1M) and 500,000 (5M),
`parent-child-exists-shapes` 50,000 (1M) and 250,000 (5M), and `extreme-dim-eq-fact` 1,000,000 (1M).

| what | measured | reading |
|---|---|---|
| a child tested for its parent, no parent column read (`NOT EXISTS`, `IN`, a join that only counts) | 1M: `orphan-sessions-not-exists` 63.1 -> 33.5 M instructions, 924 -> 441 buffer hits, 33 -> 7.6 ms; `orphan-facts` 76.9 -> 49.4 M, 683 -> 456 hits | pays: the found flag answers the join, the parent is not read (-36 to -47% instructions, -33 to -52% buffer hits) |
| a parent as large as its child, parent columns read by the join | 1M `extreme-dim-eq-fact`, 4 grouped queries: -21 M of 144-438 M instructions (-5 to -15%), warm buffer hits about 1,800 -> 740 | pays a little |
| parent columns read by the join, parent much smaller than the child | S: `parent-read-*` 67 -> 59 M (-12%), `tags-many-to-many` 407 -> 387 M; 1M: 805 -> 790 M (-2%) and 3,704 -> 3,704 M; 5M: 70,665 -> 70,647 M | neutral: the gain at 100k rows is 3-11 M instructions on queries of 0.1-2 ms and disappears at 1M and 5M |
| the other examples at 1M (`parent-child-*`, tiny parent, orphans, versioned parent, composite key) | the sum of instructions within 1.5% of the unconnected one for each example | neutral |
| child appends | WAL per row 2,235 -> 2,236 (1M), 1,935 -> 1,937 (5M) | free |
| parent attribute update (key kept) | 1,340 -> 1,367 bytes per row (1M), 643 -> 671 (5M); `sift_col` +25 bytes per row | +2 to +4%: one 32-byte journal record for each entry the update ends |
| parent key change, parent delete | key change 3,185 -> 3,206 (1M), 3,495 -> 3,518 (5M); delete 310 -> 318 | +0.7% and +2.6%; in batches of 20 rows +36 bytes per row |
| index size | S: 0 to +0.25 MB; 5M: `tags-many-to-many` 127.9 -> 131.1 MB (+2.5%), `parent-child-exists-shapes` 102.1 -> 109.1 MB (+7%) | a found flag and a stamp per child row |

Connect when child rows are tested against their parent without reading a parent column (anti, semi and counting joins: the saving is a third to a half of the
instructions and buffers at 1M), or when the parent is about as large as the child and its columns are read through the join. For a small parent whose columns
are read through the join the connection is neutral at 1M and above, and it is not worth its journal: a parent that is updated or deleted a few rows at a time
pays one journal record for each of those rows and gets no read back. Hash and text keys, versioned parents and composite keys are not connected at all; a
partitioned child connects and is neutral.

Parent-driven queries (EXISTS, NOT EXISTS, LEFT, RIGHT and FULL joins from the parent, top-N children per parent) are read from the child's `sift_col` leaves with
or without a connection (at 1M: `latest-child-by-group` and `parents-in-recent-a-children` are pushed down by default; `parents-with-big-child-by-group`, the
LEFT JOIN .. IS NULL anti, and the LEFT, RIGHT and FULL join counts are plain joins). With `sift.agg_force = on` the last group has 1.2 to 5.3 times fewer instructions
(`right-join-children-per-group` 1,902 -> 359 M, `left-join-count-by-group-and-kind` 1,569 -> 532 M) but not always fewer milliseconds (`full-join-orphans-and-childless`
214 -> 578 ms: the plain join runs in parallel), and two shapes get worse (`parents-with-big-child-by-group` 864 -> 2,102 M, 61 -> 920 ms).

## Regression shapes for the found flag

The queries that a connection answers from the found flag of the child's `sift_col` leaves (the parent is not read) are named here and asserted in their
examples: `-- plan: SiftAgg` (the sift engines push the query down) and `-- found: yes` (on a `connected:` engine EXPLAIN ANALYZE must report `found proofs`
above zero for it; `SET sift.agg_connect_found = off` fails the run). A planner or connection change that loses the flag fails the example before it shows up as a number.

| name | example / query | shape | 1M, unconnected -> connected |
|---|---|---|---|
| orphan-sessions-not-exists | `anti-left-sessions-users` / `orphan-sessions-not-exists` | `count(*)` of the children with no parent (`NOT EXISTS`, NULL keys excluded) | 63.1 -> 33.5 M instructions, 924 -> 441 buffer hits |
| orphan-facts | `late-orphan-keys` / `orphan-facts` | `count(*)`, `sum(child column)` of the children with no parent (`NOT EXISTS`) | 76.9 -> 49.4 M instructions, 683 -> 456 buffer hits |
| children-with-parent | `anti-left-sessions-users` / `sessions-with-user-semi`, `sessions-with-user-join-count`, `sessions-in-users-duration`; `late-orphan-keys` / `matched-facts-semi`, `matched-facts-join-count` | `EXISTS`, `IN` and an inner join that only counts, optionally with a sum of a child column | asserted, not measured at 1M |

The same anti question written as `LEFT JOIN .. WHERE parent.id IS NULL` (`sessions-without-user-left-null`) is not answered by the flag
today: the join reads the parent (10,000 rows loaded at `S`).

## The oracle

The expected result of a query is computed by the same SQL on the heap with every index and every sift step off (`enable_indexscan`, `enable_indexonlyscan`,
`enable_bitmapscan` off, no parallel workers, `sift.enable_agg` and `sift.enable_col_scan` off): sequential scans, hash and merge joins. It runs in its own
session on the same database, so it is always the truth of the current data. Rows are compared as text and exactly; as a set unless the header says
`ordered: yes`, which needs a total order (a tie-breaker). Rules for queries:

- aggregates are exact: counts, `numeric` sums, `avg` of numerics rounded in the query; no float sums (their order of addition depends on the plan);
- a query whose oracle is empty or all zeros proves nothing and fails the validation (`trivial`); a filter must hit rows at every scale (`-- empty: ok` for a
  query that is meant to be empty);
- `-- sift:` is the same question in sift's own predicate syntax (a BM25 filter), run by the sift engines instead of the main text; `-- oracle:` is an equal
  formulation the oracle runs when the main text has no practical plan without indexes (a per-row LATERAL with LIMIT 1 scans the table once per outer row);
- `-- plan: SiftAgg` asserts that the sift engines (not native) plan the query with that node; it records what is pushed down today, so a planner
  change that loses the node fails the run (`plan_ok` in `queries.tsv`). Queries without it are not constrained;
- one statement per query, no `;` inside, tables in the default schema, psql variables (`:nf` `:nd` `:nb` `:round` `:s` `:card` ...) allowed.

After every write batch all queries are checked again; `--validate` stops the example from being listed ready unless every query and every batch holds on
native and on sift-unconnected at the scale given. Concurrency mode (`--phases concurrency --seconds N`): readers run each query twice inside one
REPEATABLE READ snapshot (engine plan, then oracle plan) and must agree; one session keeps a snapshot for rounds of three re-reads and takes a new one for the next round (a snapshot kept for the whole run would hold
`DROP INDEX CONCURRENTLY` until the end), its answers must not change inside a round; writers run the batches marked concurrent (two of them, with deadlock retries counted); a maintenance session runs the `maintenance` batches (a 60 s run of a batch with `-- rounds: N` fails unless it ran N times);
the final state is compared with the oracle.

## Add an example

1. `registry.tsv`: a new row (id = the directory name, class, `nd_div`/`nd_min`, knob defaults, `cardinality`, `size_relation`, `key_type`, `clustering`).
2. The files above. Salts in the generator are `:s + N` (N unique per column); a row is a pure function of its number, so batches generate rows with
   `gen_<table>(:nf + :round * :nb + 1, ...)`: `:round` is unique per executed batch (ids never collide, also across sessions).
3. 3 or more queries with different kinds and 3 or more batches with different patterns; the batches that depend on each other (delete then restore) are
   `concurrent: no`.
4. `run-example --check` (vocabulary, files), then `run-example <id> S x --validate`.

Names are generic (fact, dim, parent, child, tenant, source, item, tag): no product, customer or host names, and nothing private in files or results that can
become repository content.

## Type tags

The values are in `vocab.tsv`; the join types and the query kinds are read from the query headers, the write patterns from the batch headers, the other
dimensions from the registry row. `run-example --coverage` prints, for each value, the examples that carry it, and the gaps.
