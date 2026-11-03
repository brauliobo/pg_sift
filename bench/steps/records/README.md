# Measurement records

One schema for every number the benchmark harnesses report. A harness writes records (`schema.py`), `check.py` refuses a set that cannot be compared, `report.py` generates the tables
from the records that pass. No figure of a document, a CHANGELOG line or a hand-off is typed: it comes from a record set and cites its `run_id`.

    python3 -I bench/steps/records/check.py FILE...        one line for each problem, exit 1
    python3 -I bench/steps/records/report.py [--tsv] FILE...   the tables of a set that passes

## Who writes records

    bench/bench cases --records FILE     every query case on the arms it can answer with: the default plan, a forced heap scan, the sift access method alone, the b-tree twin of a
                                         `btree` variant and each other variant as a sift arm named by it (an arm that no setting makes the plan of its name is listed as not forceable and
                                         left out); a `-- fresh: yes` case runs every repetition in a new backend. A load case has no plan to pick: its lanes (the sift table with its index,
                                         the heap with the b-tree or GIN twin, each variant) are forced arms, each repetition prepares its table again, the statements are timed and added, a
                                         second session counts the parallel workers. The file is checked on the way out and a case without records fails the run. The cluster of the cases is
                                         a test cluster (no autovacuum, no automatic merge), so its records say `maintenance` = `disabled`.
    steps/records/collect.py             the library any other harness calls (`measure_statement`) with its subject, arms and environment.

## The figures of the CHANGELOG

`bench/claims.tsv` lists the figures a CHANGELOG line states (a phrase of the line, the statement, size, arm, metric and value, a tolerance in percent). `bench records claims RECORDS...`
(and `bench cases --check --records FILE`) fails a figure that the median of the records misses by more than its tolerance and prints both: the line and the run. A phrase that matches no
line is stale and fails as well. The value is the one the line states, typed once there; the records are never typed. The phrase `-` is a figure that no CHANGELOG line states (a design note, a
page; its `note` column says where): it is checked against the records alone, and its metric may be a build's `instr_leader_m`, `instr_workers_m`, `index_bytes`, `wal_bytes`, `segments`,
`immutable_segments` or `buffer_segments`.

## What one row is

One statement, on one arm, warm or cold, one repetition. The **arm** is the structure that answered: `heap` (a sequential scan), `btree`, `gin` or `sift`, with its option (`default`,
`key_order=ts`, ...). Arms are chosen by forcing (`enable_*`, the index in the search path or the setting of the structure) on the same data, so they are comparable; the
plan the planner chooses with nothing forced is measured as well (`mode` = `default`) and each forced arm says whether it is that plan (`picked`). The structure is read from the
EXPLAIN of the run and the arm must be it. Time is three columns: planning, execution (EXPLAIN ANALYZE) and the whole statement from the client. Instructions are those of the
postmaster tree, workers included, and say their scope. Blocks are split by the kind of relation: heap, b-tree, gin, sift. Cold means the shared buffers of the relations were evicted;
`os_cache_dropped` says whether the OS cache was emptied as well.

<!-- columns -->
| column | type | required | meaning |
|---|---|---|---|
| `run_id` | text | yes | the run that wrote the row: tip, UTC time and four hex digits; every number in a document cites one |
| `tip` | text | yes | the commit of the build measured (12 hex digits) |
| `harness` | text | yes | the program that measured, with the version of its records library: rows of two harnesses are not compared |
| `dataset` | text | yes | the name of the data (generator and parameters) |
| `rows` | integer | yes | rows of the table |
| `keys` | integer |  | distinct values of the key column |
| `rows_per_key` | number |  | rows / keys |
| `heap_order` | text | yes | how the heap holds the key: arrival, sorted, hash or other |
| `heap_corr` | number |  | the correlation of the key with the heap, measured (pg_stats), not the order the generator meant |
| `table_state` | text | yes | fresh_vacuumed, fresh_unvacuumed, churned_vacuumed or churned_unvacuumed |
| `allvisible_pct` | number |  | pages marked all-visible, in percent |
| `churn_history` | text |  | what was done to the table before the arms: upd=, ins=, del= (percent of the rows a round) and rounds= when the harness churned it with apply_churn, else the tuples inserted, updated and deleted since its statistics began (ins=, upd=, del=); empty for a fresh table |
| `vacuums` | integer |  | VACUUMs run on the table (pg_stat_user_tables vacuum_count) when the arms started |
| `autovacuums` | integer |  | autovacuums run on the table (autovacuum_count) when the arms started |
| `settled` | 0 or 1 |  | 1 when the harness waited for the maintenance (VACUUM, the worker pass) to settle before the arms; needs a VACUUM or a pass on record |
| `settled_s` | number |  | seconds from the last VACUUM, autovacuum or sift maintenance pass of the table to the start of the arms |
| `dead_pct` | number |  | dead tuples in percent of live and dead (pg_stat_user_tables) at the start |
| `tombstone_pct` | number |  | dead rows in percent of the rows of the segments of the sift index (sift.col_segments) at the start, when the table has one |
| `structures` | text | yes | the structures on the table, comma separated and sorted (btree,heap,sift): what was built, not what the arm used; `constraint` stands for the indexes of a primary key or unique constraint, the table's definition, which a write pays for in every arm |
| `statement` | text | yes | the name of the statement |
| `sql_hash` | text | yes | hash of the statement text without its white space: one name is one text |
| `variant` | text | yes | the table the arm ran on when a comparison has several (a heap in arrival order and one sorted by the key, an index built with other options): `main` when there is one; the planner picks within a variant |
| `kind` | text | yes | read, maintenance (VACUUM, merge), load or build (CREATE INDEX of a sift index: the instructions of the leader and of the workers, the segments it wrote, the options it was built with) |
| `protocol` | text | yes | what one repetition is: single, or the rounds it averages (rounds=12): a figure of one round is not the average of twelve |
| `mode` | text | yes | forced (the arm was selected with settings), default (the planner chose) or unavailable (no setting makes the plan of this arm: one row without times, `structure` names what the plans were) |
| `arm` | text | yes | heap, btree, gin or sift; `default` rows carry the structure the plan named |
| `arm_option` | text | yes | the option of the structure: default, key_order=ts, key_cut, ... |
| `picked` | 0 or 1 | yes | 1 when this structure and option are the plan the planner chooses by default |
| `structure` | text | yes | the structure the EXPLAIN of this run names: heap, btree, gin, sift or mixed |
| `temp` | text | yes | warm or cold |
| `rep` | integer | yes | the repetition, from 1 |
| `est_cost` | number |  | the planner total cost of the plan that ran |
| `plan_ms` | number |  | planning time (EXPLAIN SUMMARY), apart from the execution |
| `exec_ms` | number |  | execution time (EXPLAIN ANALYZE, per-node timing on), planning not included; compares only plans of similar row flow (the clock reads of a node that moves many rows inflate it), `wall_ms` is the figure across join methods |
| `wall_ms` | number | yes | the statement from the client: planning, execution and the transfer of the rows |
| `instr_m` | number |  | millions of user instructions of the statement |
| `cycles_m` | number |  | millions of user cycles |
| `instr_scope` | text |  | cluster (the postmaster tree: backend and parallel workers) or backend (the backend alone: only valid with no worker launched) |
| `instr_n` | integer |  | statements run in the window the instructions were counted over (a statement under 5 ms is run again and again in the one backend between two readings and the count divided: the number is of one run); 1 for a long one |
| `instr_leader_m` | number |  | build: millions of user instructions of the leader backend alone, counted in a run of its own after the cluster count (the same statement on the same prepared table) |
| `instr_workers_m` | number |  | build: instr_m - instr_leader_m, the instructions of the parallel workers (0 when none launched) |
| `workers_requested` | integer | yes | parallel workers the plan asked for (a maintenance statement or a load has no plan: the workers it started) |
| `workers_launched` | integer | yes | parallel workers that ran |
| `heap_hit` | integer |  | blocks of heap relations found in shared buffers |
| `heap_read` | integer |  | blocks of heap relations read |
| `btree_hit` | integer |  | b-tree index blocks found in shared buffers |
| `btree_read` | integer |  | b-tree index blocks read |
| `gin_hit` | integer |  | gin index blocks found in shared buffers |
| `gin_read` | integer |  | gin index blocks read |
| `sift_hit` | integer |  | sift index blocks found in shared buffers |
| `sift_read` | integer |  | sift index blocks read |
| `other_hit` | integer |  | blocks of other relations (toast, other index kinds) found in shared buffers |
| `other_read` | integer |  | blocks of other relations read |
| `os_cache_dropped` | 0 or 1 |  | cold rows: 1 when the OS page cache was emptied too, 0 when only the shared buffers were |
| `table_bytes` | integer |  | pg_table_size of the table after the run (heap, TOAST and free space map; a load names the schema, the sum of its tables) |
| `index_bytes` | integer |  | pg_relation_size of the indexes of the arm's structure after the run (sift, b-tree or GIN; none for the heap arm): what a CREATE INDEX or a rewrite built |
| `segments` | integer |  | segments in the directory of the sift index of the arm after the run (all kinds: sift.col_segments, sift.segments); none for an arm without a sift index |
| `immutable_segments` | integer |  | of them the immutable ones (a build or merge wrote them) |
| `buffer_segments` | integer |  | of them the write buffers, open or sealed (rows the maintenance has not yet written as a segment) |
| `segments_declared` | integer |  | the segment count the harness says the index has (a case that names it, `-- segments: N`): a row whose count is another is refused |
| `index_options` | text |  | the options the sift index of the arm was built with (its reloptions, name=value, sorted, `;` between), `default` when it has none |
| `wal_bytes` | integer |  | WAL written by the run |
| `fpw` | 0 or 1 | yes | full_page_writes of the server |
| `cpus` | number | yes | the CPU quota the server ran with (cpus) |
| `noise` | 0 or 1 |  | 1 when the host CPU or IO pressure was above 10 points during the run: a flag, not a refusal (the load of the host is a fact about the run) |
| `psi_cpu_start` | number |  | host CPU pressure (some, avg10) at the start of the run |
| `psi_cpu_end` | number |  | host CPU pressure at its end |
| `psi_io_start` | number |  | host IO pressure at the start |
| `psi_io_end` | number |  | host IO pressure at its end |
| `shared_buffers_mb` | number | yes | shared_buffers |
| `work_mem_mb` | number | yes | work_mem |
| `pg_version` | text | yes | the server version |
| `settings` | text |  | every setting that is not the default (name=value, sorted, `;` between), the cluster and the session before the arm forced anything |
| `settings_hash` | text | yes | ten hex digits of `settings`: two rows compared have the same one |
| `maintenance` | text | yes | default, or disabled when a switch that turns the maintenance off is set (autovacuum, sift.col_auto_merge, sift.async_maintenance, a sift.test_* setting) |
<!-- /columns -->

A harness that declares what the table of a write or a maintenance holds (`Subject.structures`, `Arm.structures`) is held to it: after the statement the library reads the table's indexes, and a run whose table holds something else (a CREATE INDEX that failed, an index nobody listed) raises. An index that backs a primary key or a unique constraint is listed as `constraint`, whatever its access method: it is the table's definition, every write pays for it, and the checker accepts it beside the arm's structure when every write figure of the comparison has it (COMPANION DIFFERS). Any other index, a b-tree too, is a structure of the figure and refuses it (OTHER STRUCTURES). The `tip` of a record is a commit: a tree that is no work tree of its revision names it by hand.

The subject table of a partitioned table has no statistics, storage or access method of its own, so its facts are those of its leaf partitions together: tuples and dead tuples summed, `vacuums` and `autovacuums` the fewest of any leaf, `settled_s` the time since the latest pass of any leaf (empty while a leaf has had none), `tombstone_pct` over the leaves with a sift structure, `table_bytes` and `index_bytes` summed over the leaves. A view, or a partitioned table with no leaf, has no facts.

A build (kind `build`: a case with `-- kind: build`, `col_build_default` and `col_build_date_key`) is the `CREATE INDEX` of a sift index alone, the arms its options and worker settings. Each warm
repetition counts the cluster (leader and workers, `instr_m`), then the leader alone in a run of its own on a table prepared again (`instr_leader_m`); `instr_workers_m` is the difference. Every
record of a sift arm that has `index_bytes` carries the directory of its index after the run (`segments`, `immutable_segments`, `buffer_segments`, from `sift.col_segments` or `sift.segments`)
and the options it was built with (`index_options`); a case that says `-- segments: N` is recorded with `segments_declared` and refused when the directory holds another count.

## Versions of the schema

A file starts with `#records-schema N` (files before the mark are known by their header). Every version's columns are frozen in `history.py`; columns are only added. A reader upgrades an older file
in memory (`schema.MIGRATIONS`: a step from each version to the next, a new column gets its value from what the row says or "not recorded") and the tools say so: `note: FILE is schema 3, read
as schema 5`; `bench records migrate FILE...` writes the file again in the current schema and keeps the old one as `FILE.schema3`. A change of the columns is a new `VERSION`, its columns in
`history.py`, a step in `MIGRATIONS` and a frozen sample in `bench/test/records_samples`; `test_records.py` (part of lint) fails until the three exist and when a frozen sample is edited.

## What the checker refuses

A comparison is the rows of one statement on one dataset and size. Within it:

| refusal | rule |
|---|---|
| MIXED RUNS | one `run_id`: the arms were measured together, on one build, under one load |
| MIXED STATE | one table state, churn history (updates, inserts, deletes, rounds, VACUUMs and autovacuums on record, settled), protocol and key distribution within a comparison, dead tuples and tombstones within 5 points; one heap order within a `variant` (a variant is one table, so a heap in arrival order and one sorted by the key are two variants, each with its own default plan and picked flag) |
| MIXED SETTINGS | one `settings_hash` (every setting that is not the default, the cluster and the session before an arm forces anything), `shared_buffers`, `work_mem`, version, cpu quota, `full_page_writes` and harness; the report header shows the hash and the list |
| TEST SETTINGS NOT LABELLED / LABELLED DISABLED WITHOUT A SWITCH | a setting that turns the maintenance off (`autovacuum`, `sift.col_auto_merge`, `sift.async_maintenance`, any `sift.test_*`, as a test cluster's configuration sets them) is refused unless the set says `maintenance` = `disabled`, which the report prints in capitals; the label without such a setting is refused too |
| TWO STATEMENTS | one `sql_hash` for a statement name |
| HEAP ORDER NOT AS DECLARED | `sorted` needs a measured correlation above 0.9, `hash` under 0.1 |
| WRONG ARM | a forced arm is the structure its EXPLAIN names (a mixed plan is no arm) |
| NO PICKED / PICKED FLAG | the default plan is in the set and each arm says whether it is that plan; a default plan that mixes structures is a row of its own (arm `mixed`) that no forced arm equals; a maintenance or a load has no plan to pick: forced, picked 0 |
| CHURN HISTORY MISSING / SETTLED WITHOUT A VACUUM | a churned read says what was applied (`collect.apply_churn` returns it: percent of rows updated, inserted and deleted a round, rounds; else the tuples the statistics counted); `settled` needs a VACUUM, an autovacuum or a sift pass on record |
| INSTRUCTIONS MISSING | a warm row has a count (perf failed or was not run: refused, not left empty) |
| INSTRUCTIONS WITHOUT SCOPE / WITHOUT WORKERS | a count of the backend alone is refused when workers ran |
| WORKERS SHORT / QUOTA UNDER THE PROCESSES | the workers the plan asked for ran, and the cpu quota holds them and the backend |
| PLANNING NOT SEPARATE | a read has planning and execution apart |
| COLD WITHOUT os_cache_dropped | a cold row says what was emptied |
| TOO FEW RUNS / DUPLICATE REPETITIONS | at least 3 repetitions of each arm and temperature |
| (not a refusal) noisy host | a run above 10 points of host pressure carries `noise` = 1 and a cell above it or with a CV over 10% is marked noisy; the report warns when the arms of a comparison were measured under pressure more than 10 points apart. The load of the host is a fact about the run, so it never refuses a file; different data, table state or settings do |
| OTHER STRUCTURES | a maintenance or load figure is of a table that holds the arm's structure alone (and `constraint`: the indexes a primary key or unique constraint needs, part of the table's definition) |
| COMPANION DIFFERS | the write figures of a comparison are not all of a table with a constraint index, or all of one without: the arms would differ in more than their structure |
| SEGMENTS MISSING | a sift arm with `index_bytes` has no `segments`, `immutable_segments`, `buffer_segments` or `index_options` (rows of schema 6 and later; an older row is not refused) |
| SEGMENT KINDS | immutable and write buffer segments are more than the segments of the directory |
| SEGMENTS CONTRADICT | the count a case declares (`segments_declared`) is not the count of the directory |
| BUILD WITHOUT SPLIT / BUILD SPLIT / BUILD WORKERS | a warm build row lacks `instr_leader_m` or `instr_workers_m`, their sum is not the cluster count (1%), or the cluster ran beyond the leader with no worker launched |
| BUILD WITHOUT index_bytes / wal_bytes | a build row lacks the bytes of its index or its WAL |
| BUILD ON TEST SETTINGS | a build measured on a cluster with a `sift.test_*` setting: the build is not the product's |
| TIP UNKNOWN | the `tip` of a record is not a commit (12 hexadecimal digits) |
| NO RECORDS | an empty set proves nothing |

## The imprecisions this prevents

Each row is a way a reported number was wrong or had to be revised, and the column or rule that now makes it impossible to state.

| what went wrong | prevented by |
|---|---|
| a figure taken on sequential strings quoted beside a case that used random strings | the dataset columns (`keys`, `rows_per_key`, `heap_order`, `heap_corr`) and one dataset in a comparison |
| the time of a CREATE INDEX quoted as the time of the merge statement | the statement name and `sql_hash`, `kind`; a claim names its statement |
| gains of parallel workers hidden by a one-cpu quota | `cpus`, `workers_requested`, `workers_launched`: QUOTA UNDER THE PROCESSES, WORKERS SHORT |
| instructions of the backend beside a time with workers | `instr_scope`: INSTRUCTIONS WITHOUT WORKERS |
| execution time quoted as the whole statement, or the reverse | `plan_ms`, `exec_ms`, `wall_ms` apart |
| a cold read that was cold in the shared buffers only | `temp`, `os_cache_dropped` |
| WAL of a run with full page writes beside one without | `fpw` in the settings that must agree |
| a fresh table beside a churned one that was not vacuumed | `table_state`: MIXED STATE |
| the plan the planner picks quoted for an arm it did not run, or two arms that were the same plan | `mode`, `picked`, `structure`: WRONG ARM, PICKED FLAG |
| a heap meant to be in no order that was clustered by the key | `heap_corr` measured: HEAP ORDER NOT AS DECLARED |
| a maintenance figure of an arm whose table also kept another index | `structures`: OTHER STRUCTURES |
| a write cost with the primary key of the table in it, beside one without | `structures`: `constraint`, COMPANION DIFFERS |
| a record of code nobody can find again | `tip`: TIP UNKNOWN |
| a "7 segments" index whose record holds no count | `segments`, `segments_declared`: SEGMENTS MISSING, SEGMENTS CONTRADICT |
| a build cost that counts the leader only, or the workers by guess | `instr_leader_m`, `instr_workers_m`: BUILD SPLIT |
| a baseline file of an older harness compared with a new run | `harness` with the library version and one `run_id` |
| a number that held for one setting of a structure stated for all | `arm_option` carries the options that changed the structure |
| one round quoted as the average of twelve | `protocol` |
| a state measured on a cluster whose configuration turned the maintenance off (dead rows no VACUUM took, segments no merge joined), reported as the default's | `settings`, `settings_hash`, `maintenance`: TEST SETTINGS NOT LABELLED, MIXED SETTINGS |
| a time taken while the host was busy beside one taken while it was not | `psi_*`, `noise`: the noisy mark and the warning of the report |
| a pass read from an empty output | NO RECORDS, and the count of rows the tools print |
| a CHANGELOG figure that no longer matches the measurement | the claims check against the records |
