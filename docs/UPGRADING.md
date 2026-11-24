# Upgrading to `pg_sift` 6.0

`pg_sift` 6.0 is a major release introducing **Format Major 2** for new `sift_col` indexes and `sift_table` tables, a smaller configuration surface (27 release settings; the options of every access method are in [OPTIONS.md](OPTIONS.md)), and a unified `sift.*` SQL API in which every 5.1 function keeps working. **Every existing 5.x index and table keeps working with full read and write support: nothing has to be rebuilt or migrated to upgrade.** `sift_col` and `sift_table` relations created by 5.x stay on Format Major 1, and `sift` (BM25) indexes stay on Format `1.7`; new relations and every `REINDEX` use Format `2.0`, and you can move a 5.x relation to Format `2.0` online whenever you want its smaller size and faster scans. The update path is one statement per database, `ALTER EXTENSION pg_sift UPDATE TO '6.0';`, after the 6.0 library is installed.

For pre-6.0 release history, see [CHANGELOG.md](../CHANGELOG.md) and [docs/releases/](releases/).

## Zero-Downtime Summary

| Access method | 5.1 format | 6.0 format | What happens at the binary swap | Required action | Downtime |
|---|---|---|---|---|---|
| `sift` (BM25) | `1.7` | `1.7` | Reads, writes, `VACUUM`, and merges keep working | None | 0 |
| `sift_col` | `1.x` | `1.x` stays; `2.0` for new indexes and `REINDEX` | Queries, inserts, updates, deletes, and `VACUUM` keep using the index as before | Optional: `REINDEX INDEX CONCURRENTLY <idx>;` to move it to Format 2.0 | 0 |
| `sift_table` | `1.x` | `1.x` stays; `2.0` for new tables | The table is read, written, and vacuumed as before | Optional: `VACUUM FULL <t>;` or `SELECT sift.upgrade_table('<t>');` to rewrite it into Format 2.0 | 0 |

## 1. Preflight Check (Run on 5.1 Before Upgrading)

Run the standalone preflight script from the 6.0 release against your live 5.1 database **before** installing the 6.0 binary:

```sh
psql -d mydb -f sql/pg_sift_upgrade_check.sql
# Or after installing 6.0 SQL:
psql -d mydb -c "SELECT * FROM sift.upgrade_check();"
```

The same script runs before and after the binary swap, reads only the catalogs and the metapages, and changes nothing. It checks seven things, in this order:

1. `version`: the extension and the library; `warn` when the extension is older than the target and `ALTER EXTENSION` is still to run, `blocker` when the library is older than the extension scripts.
2. `setting`: every removed `sift.*` setting (90 of 5.1, plus 3 that 4.2 and 5.0 removed, each with its replacement), every `sift.test_*` setting, and `sift.backend_cache_kb = -1`, wherever it is set (configuration files, `ALTER SYSTEM`, `ALTER DATABASE`, `ALTER ROLE`). Each is a `blocker` with the `RESET` statement or the file to edit.
3. `option`: the reloptions of 5.1 that 6.0 no longer documents (25 `sift_col` options, `mutable_limit` and `build_segments` of `sift`, the `cols(...)` and `max_multiplicity(...)` clauses of a `scope`, and 13 retired `sift_table` options). 6.0 still reads them, so each is `info` with an optional cleanup statement.
4. `index` and `table`: the on-disk format of every `sift`, `sift_col`, and `sift_table` relation. A relation of Format Major 1 is `info` with its optional upgrade statement; an invalid index is a `warn`; one whose metapage cannot be read is a `blocker`.
5. `dependency`: a user view that depends on `sift.explain_why()` or `sift.refusal_slugs()` (6.0 gives both an `action` column, so the update fails on the dependent view) is a `blocker`; a function whose body names them is a `warn`.
6. `replication`: a published `sift_table` on a server where `pg_sift` is not in `shared_preload_libraries` is a `blocker`.
7. `test`: an installed `pg_sift_test` extension is a `warn`; it needs the test build.

Every row reports `status`:
- **`blocker`**: Must be resolved before starting the 6.0 binary or running `ALTER EXTENSION pg_sift UPDATE TO '6.0'`.
- **`warn`**: Look at it; for example the extension still has to be updated in this database.
- **`info`**: Optional. Relations on Format Major 1 (`sift_col`, `sift_table`) and the options 5.1 stored are fully read and written by 6.0; the statement to adopt Format 2.0 or to clean an option is in `advice`.
- **`ok`**: Nothing to do, including a `sift` (BM25) index on Format 1.7 and a relation with nothing written yet.

`sift.upgrade_check('5.1')` asks the other way round, what a 5.1 binary could not read: every relation in Format 2.0 is a `blocker` there (see section 5).

## 2. Breaking Changes Checklist (5.1 → 6.0)

| Area | Change in 6.0 | Required Action |
|---|---|---|
| `sift_col` on-disk format | New indexes and `REINDEX` write Format Major 2 (`2.0`); v5 Format 1 indexes keep full read and write support | **None.** Optionally run `REINDEX INDEX CONCURRENTLY <idx>;` to adopt Format 2.0 |
| `sift_table` on-disk format | New tables write Format Major 2 (`2.0`); v5 Format 1 tables keep full read and write support | **None.** Optionally run `VACUUM FULL <t>;` or `SELECT sift.upgrade_table('<t>');` to adopt Format 2.0 |
| `sift` (BM25) on-disk format | Unchanged (`1.7`) | **None**: readable and writable across 5.1 → 6.0 without `REINDEX` |
| Removed GUCs | 90 settings of 5.1 are removed or folded and 27 release settings remain (the 4 `sift.gpu*` Vulkan settings stay; see [GPU.md](GPU.md)); for example `sift.table_dead_rewrite_pct` becomes `sift.dead_rewrite_pct`, `sift.table_compression` the `compression` option of the table, `sift.table_backpressure` has no replacement, and most others are sized automatically or from `work_mem`; `sift.upgrade_check()` names the replacement of each | Remove flagged GUCs from `postgresql.conf`, `ALTER SYSTEM`, roles, and databases before restart |
| `sift.backend_cache_kb = -1` | `-1` (unlimited) removed; default `16384` (16 MB), min `0` | Reset or set an explicit kB cap (`>= 0`) before restart |
| Retired `sift_col` reloptions (25) | `key_cut`, `order_*`, `rollup_*`, `calendar_columns`, FM1 tuning flags, `write_buffer_limit` are no longer documented; `hll_columns` is now `sketches = 'hll(...)'` and `skip_index` is new. A Format 1 index keeps reading them; Format 2.0 takes its meaning from `key_order`, `preagg`, and `sketches` | **None.** Optional: `ALTER INDEX <idx> RESET (<opt>);` (`info` rows of the preflight) |
| `scope` clause cleanup | `cols(...)` and `max_multiplicity(...)` are deprecated: they still parse, Format Major 1 reads them as before, and Format Major 2 ignores both because Entity-Once serves all columns with no fan-out | **None.** Optionally remove them from the `scope` option ([SCOPES.md](SCOPES.md)) |
| Retired `sift` reloptions (2) | `mutable_limit` is the deprecated name of `write_buffer_limit`; `build_segments` is a test-only option and is ignored | **None.** Optional: `ALTER INDEX <idx> RESET (mutable_limit), SET (write_buffer_limit = N);` |
| Retired `sift_table` options (13) | `wide_rows`, `inline_promote`, `async_promote`, `hot_update`, `bulk_groups`, `dict_lookup`, `key_table`, `write_buffer_limit`, `backpressure`, and the four `auto_defrag_*` tuning options are tuned by Format 2.0 itself; a Format 1 table keeps applying its stored values. `paths`, `scope`, `connect`, `preagg`, `sketches`, and `skip_index` are the options a table shares with `sift_col` ([TABLES.md](TABLES.md#3-per-table-options-10-options)) | **None.** Optional: `SELECT sift.table_reset_option('<t>', '<opt>');` |
| SQL API consolidation | Every 5.1 function that 6.0 replaces with a unified one (`sift.verify`, `sift.col_verify`, `sift.table_verify` → `sift.check`; `sift.col_promote` → `sift.promote`; `sift.col_merge` → `sift.merge`; `sift.col_maintain`, `sift.table_maintain` → `sift.maintain`; `sift.index_info`, `sift.col_space`, `sift.table_space` → `sift.space`; `sift.col_segments`, `sift.table_segments` → `sift.parts`; and the rest of the `@deprecated` set in [REFERENCE.md](REFERENCE.md)) keeps its name, arguments, and result columns and calls its successor; a column that the successor cannot give is `NULL`. `sift.explain_why()` and `sift.refusal_slugs()` gain an `action` column. New helpers live in `sift_internal` | Move scripts to the unified functions at your pace. A view that selects from `sift.explain_why()` or `sift.refusal_slugs()` must be dropped before `ALTER EXTENSION` and created again after (preflight `dependency` row) |
| Default `sift.parallel_scan` | Default is now `on` (parallel guided-morsel scan, aggregation, build, and compaction) | Set `sift.parallel_scan = off` to restore single-process plans |
| Default `sift.agg_exact_float` | New GUC, default `off` (parallel `float8` sums add in the order of the vector kernels) | Set `sift.agg_exact_float = on` if a `float8` sum must repeat to the last bit: it adds in row order in one process |

### Full v5 Compatibility and the Optional Upgrade to Format 2.0

A 6.0 binary reads the 16-byte header of block 0 to tell the two formats apart and serves both with the same behavior:

- **Format Major 1 relations (`sift_col`, `sift_table`) and Format 1.7 `sift` indexes** are read, written, vacuumed, merged, and used by `SiftAgg` and the planner exactly as on 5.1. They are never treated as corrupt and never placed in `sift.quarantine`, and no statement is refused because of their format.
- **New relations and every `REINDEX`** are written in Format Major 2 (`2.0`).
- **Format 2.0 is an opt-in gain, not a requirement.** It adds the smaller footprint and the faster scans, key-order pruning, pre-aggregates, and maintenance of Format 2.0 (see [ARCHITECTURE.md](ARCHITECTURE.md)). Upgrade a relation when convenient, online and one at a time:

| Relation | Upgrade to Format 2.0 | Effect |
|---|---|---|
| `sift_col` index | `REINDEX INDEX CONCURRENTLY <idx>;` | Rebuilds the index into Format 2.0 while the old one keeps serving queries and writes, then swaps it in atomically |
| `sift_table` table | `VACUUM FULL <t>;` or `SELECT sift.upgrade_table('<t>');` | Rewrites the table into Format 2.0 |
| `sift` (BM25) index | None | Stays on Format 1.7; it is the current BM25 format |

`sift.indexes` shows the on-disk `format` of every relation (`1.x` for a 5.x columnar relation), and `sift.health()` lists each one as an `info` row (`format_upgrade_available`) with the statement above. The new 6.0 options (`key_order = 'auto'`, `paths = 'auto'`, `sketches`, `skip_index`, the 5 `sift_table` options) apply to Format 2.0 relations; a Format 1 relation keeps the behavior of its 5.x options until it is upgraded.

**Functions between the library swap and `ALTER EXTENSION`:** if the 6.0 library is loaded while a database still has the 5.1 SQL objects, calling one of the 30 functions that 6.0 replaces raises `55000` (`object_not_in_prerequisite_state`) with the hint to run `ALTER EXTENSION pg_sift UPDATE TO '6.0';` and the name of the successor, never a missing-symbol error; after the update the function is a thin alias of that successor. Three functions are gone (`sift.col_track_old_rows`, `sift._col_value_store_begin`, `sift._col_value_store_end`) and raise `0A000` (`feature_not_supported`). Run the update in every database soon after the restart.

### Defaults of 6.0

There is no setting that chooses the format: every new relation is written in Format Major 2, with `key_order = 'auto'`, `paths = 'auto'`, `preagg = 'auto'`, and `track_changes = 'auto'` chosen once at build time and reported in a `NOTICE`. Two cases are built in Format Major 1 on purpose because Format Major 2 does not hold them yet: a `sift_col` index that declares or is the target of a `connect`, and one whose `scope` has a `text`, `varchar`, or `uuid` member or key ([SCOPES.md](SCOPES.md)). `sift.index_format(rel)` and `sift.indexes.format` report the format of any relation. Other defaults that change what a query does: `sift.parallel_scan = on`, `sift.agg_exact_float = off`, and `sift.backend_cache_kb = 16384` with `0` as the least.

## 2a. The Migration Plan (`sift.migrate_plan()`)

For a cluster with many indexes, `sift.migrate_plan(target text DEFAULT '6.0')` turns the preflight findings into one ordered runbook of copy-pasteable SQL. It is a plain SQL function over the same findings as `sift.upgrade_check()`, reads the catalogs, changes nothing, and has no settings. A `target` other than `6.x` is refused (use `sift.upgrade_check('5.1')` for a downgrade).

```sql
SELECT step, phase, object, severity, reason, action_sql FROM sift.migrate_plan() ORDER BY step;
```

| Phase | What it holds |
|---|---|
| `pre_guc` | `ALTER SYSTEM ... RESET`, `ALTER DATABASE ... RESET`, and `ALTER ROLE ... RESET` for every removed setting, then `SELECT pg_reload_conf();`; also a published `sift_table` that needs `pg_sift` in `shared_preload_libraries` |
| `pre_dep` | Views that depend on `sift.explain_why()` or `sift.refusal_slugs()`, a failed `CREATE INDEX CONCURRENTLY` to drop, and an installed `pg_sift_test` |
| `upgrade_ext` | `ALTER EXTENSION pg_sift UPDATE TO '6.0';` (only when the extension is not at 6.0) |
| `post_optional_upgrade` | *Optional.* `REINDEX INDEX CONCURRENTLY <idx>;` for every Format 1 `sift_col` index and `SELECT * FROM sift.upgrade_table('<rel>');` for every Format 1 `sift_table`, smallest first so progress shows early; BM25 indexes are left out |
| `post_cleanup` | *Optional.* `ALTER INDEX ... RESET (<option>)` and `SELECT sift.table_reset_option(...)` for the options 5.1 stored that 6.0 no longer documents |
| `post_verify` | `SELECT * FROM sift.check();` and `SELECT * FROM sift.health();` |

`severity` is `blocker`, `warn`, `action` (do it), `info` (optional), or `ok`, and `action_sql` is `NULL` where the fix is a manual edit (a line of `postgresql.conf`, a `scope` option). The upgrade itself needs only the `pre_*` and `upgrade_ext` steps.

```sh
# 1. Before the binary swap, on 5.1: export the pre-upgrade statements
psql -d mydb -At -c "SELECT action_sql FROM sift.migrate_plan() WHERE phase LIKE 'pre\_%' AND action_sql IS NOT NULL ORDER BY step;"

# 2. After the swap and ALTER EXTENSION UPDATE, on 6.0: optionally run the online rebuilds
psql -d mydb -At -c "SELECT action_sql FROM sift.migrate_plan() WHERE phase = 'post_optional_upgrade' ORDER BY step;" | psql -d mydb -v ON_ERROR_STOP=1
```

## 3. Step-by-Step Upgrade (Standbys First)

Install the 6.0 binary on **all physical standbys first**, then the primary, so a standby never receives Format Major 2 WAL pages from a newer primary.

```sh
# 1. On the 5.1 primary: run preflight and resolve all 'blocker' rows
#    (sift.migrate_plan() lists the statements in order, see section 2a)
psql -d mydb -f sql/pg_sift_upgrade_check.sql

# 2. Reset the flagged sift.* settings (removed names, sift.test_*, sift.backend_cache_kb = -1)
psql -d mydb -At -c "SELECT action_sql FROM sift.migrate_plan() WHERE phase = 'pre_guc' AND action_sql IS NOT NULL ORDER BY step;" | psql -d mydb
```

```sh
# 1. Install the pg_sift 6.0 package/binary on every standby and restart standbys
sudo apt install ./postgresql-18-pg-sift_6.0.0*.deb
sudo systemctl restart postgresql

# 2. Install the pg_sift 6.0 package/binary on the primary and restart primary
sudo apt install ./postgresql-18-pg-sift_6.0.0*.deb
sudo systemctl restart postgresql

# 3. Update the extension in every database
psql -d mydb -c "ALTER EXTENSION pg_sift UPDATE TO '6.0';"

# 4. Verify soundness and health across all relations
psql -d mydb -c "SELECT * FROM sift.check();"
psql -d mydb -c "SELECT * FROM sift.health();"
```

Queries and writes continue on every existing index and table throughout. Adopt Format 2.0 later, one relation at a time, when it suits you:

```sh
# 1. Rebuild a Format 1 sift_col index online into Format 2.0 (the old index serves until the swap)
psql -d mydb -c "REINDEX INDEX CONCURRENTLY orders_col;"

# 2. Rewrite a Format 1 sift_table into Format 2.0
psql -d mydb -c "SELECT sift.upgrade_table('events_col');"
```

### Dump and Restore Instead of an In-Place Upgrade

A logical dump of 5.x restored on 6.0 creates every `sift_table` and every `sift_col` index in Format Major 2 (except the ones that stay in Format Major 1, see Defaults of 6.0), so no `sift.upgrade_table()` or `REINDEX` is needed afterwards. The per-table options are dumped as data and may load after the rows; see [OPERATIONS.md](OPERATIONS.md#10-backup-pg_dump-and-pg_restore) for the restore order that applies them at once and for the `table_options_unapplied` check.

## 4. Upgrading PostgreSQL Majors (`pg_upgrade`)

`pg_upgrade` preserves `sift`, `sift_col`, and `sift_table` files as-is when **the exact same `pg_sift` major release** is installed in both the old and new PostgreSQL binaries:

1. If moving from `pg_sift` 5.1 on PG 17 to `pg_sift` 6.0 on PG 18/19, perform the `pg_sift` 5.1 → 6.0 upgrade **either before or after** `pg_upgrade` — never combine an extension major jump and a PostgreSQL major jump in one restart.
2. Install the matching `pg_sift` package for the target PostgreSQL major (`postgresql-18-pg-sift` or `postgresql-19-pg-sift`) before running `pg_upgrade --check`.
3. After `pg_upgrade` completes, run `VACUUM ANALYZE` and `SELECT * FROM sift.check();`.

## 5. Downgrade Policy

- **Format Major 2 (`sift_col`, `sift_table`):** A `pg_sift` 5.x binary cannot read Format Major 2 files. Only relations you upgraded to Format 2.0 (or created on 6.0) are affected: roll those back by dropping the `sift_col` indexes and converting the `sift_table` relations to `heap`, then downgrade the binary and extension and rebuild them under 5.1, or restore a pre-upgrade backup. `SELECT * FROM sift.upgrade_check('5.1');` lists every such relation as a `blocker` with the statement that makes it readable by 5.1 (`ALTER TABLE <t> SET ACCESS METHOD heap;` for a `sift_table`, drop and build again with that release for a `sift_col` index). Keep the upgrade to Format 2.0 for after you no longer need the 5.1 rollback.
- **Format 1.7 (`sift` BM25):** BM25 indexes written by 6.0 remain on Format 1.7 and are readable by 5.1 unless a newer minor feature is introduced later.
- **Patch releases (`6.0.x`):** Patch releases never change on-disk formats or SQL schemas and can be rolled back in place by swapping the `.so` binary.
