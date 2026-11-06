# Tuning: what to touch, and what to leave alone

pg_sift has about 150 options, and you should not need most of them. The build and the planner decide: `CREATE INDEX` picks the structures a table pays for and prints one NOTICE that says what it chose and how to turn each choice off; the planner chooses between the aggregate scan and PostgreSQL's own plan by cost. This page is the short list of what is worth knowing, in the order you meet it. Every option has its full case (what it does, measured gain, cost, how to turn it off) in [OPTIONS.md](OPTIONS.md); the generated tables are in [USAGE.md](USAGE.md#maintenance-and-operations); the SQL functions are in [REFERENCE.md](REFERENCE.md).

## The ten things a user touches

| # | What | Kind | Use it when |
|---|---|---|---|
| 1 | `sift.enable_agg = off` | setting | you want the plan PostgreSQL has without pg_sift, for one statement, a role or a database: to compare answers and times, or to find out which side gave a wrong result |
| 2 | `sift.explain_why(query)` | function | a query kept the normal plan and you want to know why and what to change |
| 3 | `sift.index_choices(index)` | function | you want to read what `CREATE INDEX` chose for a `sift_col` index and the command that turns each choice off |
| 4 | `track_changes` | index option | the index is read without the table (on by default for the primary key) |
| 5 | `key_order` | index option | the table is written in the order of a key and you read ranges, `ORDER BY .. LIMIT` or the latest rows of each tenant |
| 6 | `paths`, `sift.suggest_paths()` | index option, function | a `jsonb` column is in a `sift_col` index and you choose which of its keys are columns |
| 7 | `sift.workers` | view | you ask whether maintenance runs, has failed, or an index is due for a rewrite |
| 8 | `sift.upgrade_commands()` | function | after `ALTER EXTENSION pg_sift UPDATE`: what to `REINDEX` or rewrite (it changes nothing) |
| 9 | `stemmer`, `prefix_index`, `analyzer = 'keyword'` | index options of `sift` (BM25) | you want word forms to match, autocomplete, or exact keys and facets |
| 10 | `sift.table_options()`, `sift.table_set_option()` | functions | you tune one `sift_table` (its options are set per table, with functions, because PostgreSQL has no table options for an extension before version 20) |

```sql
-- 1  the plan without pg_sift, for this transaction only
BEGIN; SET LOCAL sift.enable_agg = off; EXPLAIN SELECT kind, count(*) FROM events GROUP BY kind; ROLLBACK;

-- 2  why the aggregate scan was not used
SELECT arm, used, slug, reason FROM sift.explain_why('SELECT kind, count(*) FROM events GROUP BY 1');

-- 3  what the index build chose (and "off": the command that undoes it)
SELECT choice, kind, value, off FROM sift.index_choices('events_col');

-- 4  reading without the table: keep it, or turn it off
ALTER INDEX events_col SET (track_changes = 'off');

-- 5  rows written in the order of ts
CREATE INDEX events_col ON events USING sift_col (id, kind, ts) WITH (key_order = 'ts');
--    the latest rows of each tenant in one big table
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, ts) WITH (key_order = 'tenant_id, ts');

-- 6  the keys of a jsonb column that are worth a column: the function reads a sample and writes the option
SELECT sift.suggest_paths('events', 'payload');

-- 7  maintenance: running, failed, due
SELECT index, kind, segments, sealed, running, failures, due FROM sift.workers;

-- 8  after an update of the extension
SELECT kind, object, reason, command, required FROM sift.upgrade_commands();

-- 9  BM25: English word forms, prefixes of up to 3 letters, exact keys
CREATE INDEX docs_body ON docs USING sift (body) WITH (stemmer = 'en', prefix_index = 3);
CREATE INDEX docs_kind ON docs USING sift (kind) WITH (analyzer = 'keyword');

-- 10 one sift_table: the value in force, where it comes from, and a change
SELECT * FROM sift.table_options('events');
SELECT sift.table_set_option('events', 'write_buffer_limit', '8MB');
```

Two more habits are worth having. After a load or a heavy delete, `VACUUM` is what returns space and merges segments; the background workers (`sift.col_auto_merge`, `sift.table_auto_defrag`, `sift.async_maintenance`) do the same in the background and are on. And `sift.stats()` / `sift.explain_refusals = on` are for support, when `explain_why` is not enough.

## You should not need these

Every option has a class in [OPTIONS.md](OPTIONS.md). **default** is on and right: leave it. **opt-in** is off because it fails a term of the rule for a default (its `why` says which) and pays only for the workload it names. **safety** is a limit on memory, locks, I/O or replicas: set one when a limit hurts, not before. **diagnostic** is for measuring, comparing or explaining a plan, not for a deployed configuration: switching one of these off gives the same rows by another path, which is the point when you must find out whether an optimization gave a wrong result. **deprecated** is an older spelling that is still accepted.

The diagnostic options (generated from [options.tsv](options.tsv)):

<!-- sift-diagnostics:start -->
| Option | Applies to | What it does |
|---|---|---|
| `build_segments` | BM25 | forces N segments at build and turns the parallel build off |
| `cluster_advice` | sift_col | sift.cluster_advice() may recommend a physical order or a key_cut for the table, and CREATE INDEX may suggest key_cut |
| `sift.enable_scan` | BM25 | switches of the ranked scan, the count(*) pushdown, the bitmap of the other quals and the read-ahead |
| `sift.enable_count` | BM25 | switches of the ranked scan, the count(*) pushdown, the bitmap of the other quals and the read-ahead |
| `sift.log_min_duration` | all | logs promotions, merges and worker runs of at least that long |
| `sift.simd` | all | caps the SIMD level of every kernel; every level returns the same results bit for bit |
| `sift.enable_col` | sift_col | the column scans over sift_col indexes |
| `sift.cluster_advice` | sift_col | sift.cluster_advice() may recommend a physical order or a key_cut for the tables, and CREATE INDEX may suggest key_cut |
| `sift.agg_verdict_cache` | sift_col, sift_table | a plan that does not depend on the statements planned before it: the choice between a shared CTE and its inline plan, and between the flattened form of a statement and the form as written, is made in full for every statement (support, benchmarks, comparing two plans) |
| `sift.explain_refusals` | sift_col, sift_table | a notice says why a query kept the normal plan instead of SiftAgg |
| `sift.count_refusals` | sift_col, sift_table | a notice says why a query kept the normal plan instead of SiftAgg |
| `sift.plan_scale` | sift_col, sift_table | plan as if the tables with a sift_col index had this many times the pages and rows, to see the planner's choice at a larger size (on every supported PostgreSQL) |
| `sift.agg_force` | sift_col, sift_table | takes the aggregate scan wherever one is possible, whatever the plans cost, to time the plan the cost model refused |
| `sift.eval_methods` | sift_col | selects the developer evaluation method named: penalty_maintenance prices the VACUUM that reads ask for |
<!-- sift-diagnostics:end -->

## Same word, three places

An option that means the same thing in the three kinds has the same name, but not the same default, because the work it controls costs different amounts:

| Name | `sift` (BM25 index) | `sift_col` index | `sift_table` |
|---|---|---|---|
| `write_buffer_limit` | index option, kB, 4096 | index option, kB, 4096 | `sift.table_write_buffer_limit` or table option, kB 2048 to 8192, 4096 |
| `dead_rewrite_pct` | index option, 50 (100 never rewrites a lone segment) | index option, 0 = by cost (100 never) | `sift.table_dead_rewrite_pct` or table option, 30 |
| `vacuum_truncate` | index option, true | index option, true | none |
| `dict_lookup` | none | index option, `auto`, `on` or `off` | `sift.table_dict_lookup` or table option, `on` or `off` |
| `key_table`, `compression` | none | index options | `sift.table_key_table`, `sift.table_compression`, or table options |
| background work | `sift.async_maintenance` | `sift.col_auto_merge` | `sift.table_auto_defrag` |

## The limits of the background work

The same four questions have three sets of settings. They are different mechanisms with different units and defaults, which is why they were not merged:

| Question | `sift_col` (rewrite after churn) | `sift_table` (background pass) | Shared |
|---|---|---|---|
| Is it on? | `sift.col_auto_merge` (on) | `sift.table_auto_defrag` (on) | `sift.async_maintenance` (on) |
| Limit the WAL | `sift.col_auto_merge_wal_pct`: percent of the server's WAL (5; 0 does not wait) | `sift.table_auto_defrag_wal_rate`: kB/s (0: no limit) | `sift.maintenance_wal_rate`: kB/s of the merges of the worker (0: no limit) |
| Work in one pass | `sift.col_auto_merge_pass_mb` (1024 MB) | `sift.table_auto_defrag_budget` (128 MB) | `sift.vacuum_rewrite_limit` (128 MB a `VACUUM` of a column index or table) |
| Wait while clients are busy | `sift.col_auto_merge_max_active` (0: never) | `sift.table_auto_defrag_max_active` (0: never) | none |
| Slow down | none | `sift.table_auto_defrag_delay` (10 ms) | `sift.maintenance_cost_delay` (0 ms), `sift.maintenance_naptime` (2000 ms) |

## The words that mean several things

- **key.** `key_columns` (and `sift.table_key_table`, `key_table`) are *lookup keys*: the columns whose distinct values every unit lists, for equality, `IN` and `count(DISTINCT)`. `key_cut` and `key_cut_rows` are a *sort key*: the column whose range cuts the units. `key_order` is the sort key of the table as a whole and stands for `key_cut`, `order_columns`, `order_array_columns` and `point_tid_columns` on it. For ordered reads set `key_order` and nothing else.
- **rollup.** `rollup` is the *array elements and map values* a unit groups by, `rollup_dims` the *scalar columns* a unit groups by (the rollup of the rows), `rollup_on_demand` and `sift.col_rollup_on_demand` / `sift.col_rollup_worker` the *refresh* of rollups in the background (`sift.col_rollup_on_demand` asks, `sift.col_rollup_worker` serves; with the worker off the requests wait for `sift.col_rollup_refresh(index)`), `sift.agg_rollup_cost` the price of reading one.
- **field and section.** The text of a `fields` index is made of fields; `sift.sections()` is the function that joins them.
- **`cluster_advice`.** A setting, an index option and a function share the name: the function `sift.cluster_advice()` recommends a physical order; the setting and the option only switch the recommendation off.

## The values of the list options

(`track_changes` defaults to `auto`.) Most options of a `sift_col` index that take columns accept `auto` (the build decides), `off` and a list. What `''` (not set) means depends on the option, and the word `auto` is not accepted by all of them:

| Option | `''` means | `auto` | `off` | Other words |
|---|---|---|---|---|
| `key_columns`, `key_cut`, `order_array_columns`, `order_array_group`, `rollup_dims`, `count_columns` | automatic | yes | yes | |
| `calendar_columns` | every date and timestamp column | no | yes | `all` |
| `rollup` | the byte-budget rule (2% of the unit) | no | yes | |
| `preagg` | the default | yes | yes | `default`, `cube(..)` |
| `order_layout` | `auto` | yes | no | `whole`, `compact` |
| `compression` | `auto` | yes | no | `none`, `lz4`, `zstd` |
| `point_columns`, `point_tid_columns` | none | yes | yes | |
| `order_columns`, `hll_columns`, `return_columns`, `key_order` | none | no | yes | |
| `array_lengths`, `dict_lookup` (enums) | not allowed | yes | yes | `on` |
| `document_present` (new in 4.2) | not allowed | yes | yes | `on` |

## Which function to call

| You want | `sift` (BM25) | `sift_col` | `sift_table` |
|---|---|---|---|
| size and free space of the file | `sift.index_info` | `sift.col_space` (free space in detail), `sift.col_index_info` (rows, bytes, units), `sift.col_extents` (every run of blocks) | `sift.table_space` |
| the segments | `sift.segments` | `sift.col_segments` | `sift.table_segments` |
| what it holds | `sift.segment_postings` | `sift.col_leaves`, `sift.col_sections` | |
| merge, promote, check | `sift.merge`, `sift.promote`, `sift.verify` | `sift.col_merge`, `sift.col_promote`, `sift.col_verify` | `sift.table_maintain` |
| maintenance state | `sift.workers`, `sift.worker_state` | `sift.workers`, `sift.col_maintenance_status`, `sift.col_maintenance_due` | |
| what the build chose or advises | | `sift.index_choices`, `sift.index_advice`, `sift.key_cut_decision`, `sift.cluster_advice` | |

A function with the prefix of the other kind answers `"x" is not a sift index` (or `sift_col`) and changes nothing.
