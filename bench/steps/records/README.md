# Measurement records

One schema for every number the benchmark harnesses report. A harness writes records (`schema.py`), `check.py` refuses a set that cannot be compared, `report.py` generates the tables
from the records that pass. No figure of a document, a CHANGELOG line or a hand-off is typed: it comes from a record set and cites its `run_id`.

    python3 -I bench/steps/records/check.py FILE...        one line for each problem, exit 1
    python3 -I bench/steps/records/report.py [--tsv] FILE...   the tables of a set that passes

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
| `structures` | text | yes | the structures on the table, comma separated and sorted (btree,heap,sift): what was built, not what the arm used |
| `statement` | text | yes | the name of the statement |
| `sql_hash` | text | yes | hash of the statement text without its white space: one name is one text |
| `kind` | text | yes | read, maintenance (VACUUM, CREATE INDEX, merge) or load |
| `protocol` | text | yes | what one repetition is: single, or the rounds it averages (rounds=12): a figure of one round is not the average of twelve |
| `mode` | text | yes | forced (the arm was selected with settings) or default (the planner chose) |
| `arm` | text | yes | heap, btree, gin or sift; `default` rows carry the structure the plan named |
| `arm_option` | text | yes | the option of the structure: default, key_order=ts, key_cut, ... |
| `picked` | 0 or 1 | yes | 1 when this structure and option are the plan the planner chooses by default |
| `structure` | text | yes | the structure the EXPLAIN of this run names: heap, btree, gin, sift or mixed |
| `temp` | text | yes | warm or cold |
| `rep` | integer | yes | the repetition, from 1 |
| `est_cost` | number |  | the planner total cost of the plan that ran |
| `plan_ms` | number |  | planning time (EXPLAIN SUMMARY), apart from the execution |
| `exec_ms` | number |  | execution time (EXPLAIN ANALYZE), planning not included |
| `wall_ms` | number | yes | the statement from the client: planning, execution and the transfer of the rows |
| `instr_m` | number |  | millions of user instructions of the statement |
| `cycles_m` | number |  | millions of user cycles |
| `instr_scope` | text |  | cluster (the postmaster tree: backend and parallel workers) or backend (the backend alone: only valid with no worker launched) |
| `workers_requested` | integer | yes | parallel workers the plan asked for |
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
| `wal_bytes` | integer |  | WAL written by the run |
| `fpw` | 0 or 1 | yes | full_page_writes of the server |
| `cpus` | number | yes | the CPU quota the server ran with (cpus) |
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

## What the checker refuses

A comparison is the rows of one statement on one dataset and size. Within it:

| refusal | rule |
|---|---|
| MIXED RUNS | one `run_id`: the arms were measured together, on one build, under one load |
| MIXED STATE | one table state, protocol, heap order, key distribution and visibility |
| MIXED SETTINGS | one `settings_hash` (every setting that is not the default, the cluster and the session before an arm forces anything), `shared_buffers`, `work_mem`, version, cpu quota, `full_page_writes` and harness; the report header shows the hash and the list |
| TEST SETTINGS NOT LABELLED / LABELLED DISABLED WITHOUT A SWITCH | a setting that turns the maintenance off (`autovacuum`, `sift.col_auto_merge`, `sift.async_maintenance`, any `sift.test_*`, as a test cluster's configuration sets them) is refused unless the set says `maintenance` = `disabled`, which the report prints in capitals; the label without such a setting is refused too |
| TWO STATEMENTS | one `sql_hash` for a statement name |
| HEAP ORDER NOT AS DECLARED | `sorted` needs a measured correlation above 0.9, `hash` under 0.1 |
| WRONG ARM | a forced arm is the structure its EXPLAIN names (a mixed plan is no arm) |
| NO PICKED / PICKED FLAG | the default plan is in the set and each arm says whether it is that plan |
| INSTRUCTIONS WITHOUT SCOPE / WITHOUT WORKERS | a count of the backend alone is refused when workers ran |
| WORKERS SHORT / QUOTA UNDER THE PROCESSES | the workers the plan asked for ran, and the cpu quota holds them and the backend |
| PLANNING NOT SEPARATE | a read has planning and execution apart |
| COLD WITHOUT os_cache_dropped | a cold row says what was emptied |
| TOO FEW RUNS / DUPLICATE REPETITIONS | at least 3 repetitions of each arm and temperature |
| DIFFERENT LOAD | the host pressure of the arms within 10 points of each other; a run above 10 points of pressure or 10% CV is marked noisy in the report |
| OTHER STRUCTURES | a maintenance or load figure is of a table that holds the arm's structure alone |
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
| a baseline file of an older harness compared with a new run | `harness` with the library version and one `run_id` |
| a number that held for one setting of a structure stated for all | `arm_option` carries the options that changed the structure |
| one round quoted as the average of twelve | `protocol` |
| a state measured on a cluster whose configuration turned the maintenance off (dead rows no VACUUM took, segments no merge joined), reported as the default's | `settings`, `settings_hash`, `maintenance`: TEST SETTINGS NOT LABELLED, MIXED SETTINGS |
| a time taken while the host was busy beside one taken while it was not | `psi_*`: DIFFERENT LOAD, the noisy mark |
| a pass read from an empty output | NO RECORDS, and the count of rows the tools print |
| a CHANGELOG figure that no longer matches the measurement | the claims check against the records |
