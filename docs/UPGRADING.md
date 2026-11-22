# Upgrading to `pg_sift` 6.0

`pg_sift` 6.0 is a major release introducing **Format Major 2** for new `sift_col` indexes and `sift_table` tables, a streamlined 63-option configuration surface (27 GUCs, 12 `sift_col`, 14 `sift`, and 10 `sift_table` options), and a cleaned-up `sift.*` SQL API. **Every existing 5.x index and table keeps working with full read and write support: nothing has to be rebuilt or migrated to upgrade.** `sift_col` and `sift_table` relations created by 5.x stay on Format Major 1, and `sift` (BM25) indexes stay on Format `1.7`; new relations and every `REINDEX` use Format `2.0`, and you can move a 5.x relation to Format `2.0` online whenever you want its smaller size and faster scans.

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

The same script runs before and after the binary swap. It checks seven things: the extension and library `version`, removed `setting`s (all 90 removed GUCs, `sift.backend_cache_kb = -1`, and `sift.test_*`), removed `option`s (25 `sift_col` reloptions, 2 `sift` reloptions, 13 retired `sift_table` options that 6.0 still accepts with a deprecation `NOTICE`, and the `scope` clauses `cols(...)` and `max_multiplicity(...)`), the on-disk format of every `index` and `table`, user objects that `depend`s on dropped `sift` objects or call dropped functions, `replication` publications of a `sift_table` without the library preloaded, and an installed `pg_sift_test`.

Every row reports `status`:
- **`blocker`**: Must be resolved before starting the 6.0 binary or running `ALTER EXTENSION pg_sift UPDATE TO '6.0'`.
- **`warn`**: Safe to upgrade; requires a post-upgrade action (such as removing a flagged setting from a role).
- **`ok`**: No action required. Relations on Format Major 1 (`sift_col`, `sift_table`) and Format 1.7 (`sift`) report `ok` with the advice that an online upgrade to Format 2.0 is optional.

## 2. Breaking Changes Checklist (5.1 → 6.0)

| Area | Change in 6.0 | Required Action |
|---|---|---|
| `sift_col` on-disk format | New indexes and `REINDEX` write Format Major 2 (`2.0`); v5 Format 1 indexes keep full read and write support | **None.** Optionally run `REINDEX INDEX CONCURRENTLY <idx>;` to adopt Format 2.0 |
| `sift_table` on-disk format | New tables write Format Major 2 (`2.0`); v5 Format 1 tables keep full read and write support | **None.** Optionally run `VACUUM FULL <t>;` or `SELECT sift.upgrade_table('<t>');` to adopt Format 2.0 |
| `sift` (BM25) on-disk format | Unchanged (`1.7`) | **None**: readable and writable across 5.1 → 6.0 without `REINDEX` |
| Removed GUCs | Reduced from 113 → 27 release GUCs (the 4 `sift.gpu*` Vulkan settings stay; see [GPU.md](GPU.md)); `sift.col_auto_merge`, `sift.table_backpressure`, `sift.table_compression`, `sift.table_dead_rewrite_pct`, and `sift.table_write_buffer_limit` are folded into `sift.async_maintenance`, `sift.max_maint_workers`, `sift.dead_rewrite_pct`, and automatic sizing | Remove flagged GUCs from `postgresql.conf`, `ALTER SYSTEM`, roles, and databases before restart |
| `sift.backend_cache_kb = -1` | `-1` (unlimited) removed; default `16384` (16 MB), min `0` | Reset or set an explicit kB cap (`>= 0`) before restart |
| Removed `sift_col` reloptions (25) | `key_cut`, `order_*`, `rollup_*`, `calendar_columns`, FM1 flags, `write_buffer_limit` removed; `hll_columns` is now `sketches = 'hll(...)'` and `skip_index` is new | `ALTER INDEX <idx> RESET (<opt>);` (migrate to `key_order` or `preagg`) |
| `scope` clause cleanup | `cols(...)` and `max_multiplicity(...)` removed | Update `scope` definition before upgrade; Entity-Once serves all columns automatically |
| Removed `sift` reloptions (2) | `mutable_limit` and `build_segments` removed from release | `ALTER INDEX <idx> RESET (mutable_limit), SET (write_buffer_limit = N);` |
| Removed `sift_table` options (13) | Legacy write/defrag tuning options, `write_buffer_limit`, and `backpressure` auto-tuned and removed; `paths`, `scope`, `connect`, `preagg`, `sketches`, and `skip_index` are new for tables (10 documented) | `pg_sift--5.1--6.0.sql` prunes stale rows automatically |
| SQL API consolidation | 56 internal functions moved to `sift_internal`; 14 FM1 + 17 dupes + 5 views dropped | Update user views/functions referencing dropped objects (flagged by preflight `pg_depend` check) |
| Default `sift.parallel_scan` | Default is now `on` (parallel guided-morsel scan, aggregation, build, and compaction) | Set `sift.parallel_scan = off` to restore single-process plans |
| Default `sift.agg_exact_float` | New GUC, default `off` (IEEE-754 parallel sum) | Set `sift.agg_exact_float = on` if you require bit-identical row-order `float8` sums |

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

**Retired functions before `ALTER EXTENSION`:** if the 6.0 library is loaded while a database still has the 5.1 SQL objects, calling any of the 31 removed C-backed functions raises `0A000` (`feature_not_supported`) naming the 6.0 replacement, never a missing-symbol error. Run `ALTER EXTENSION pg_sift UPDATE TO '6.0';` in every database soon after the restart.

## 2a. The Migration Plan (`sift.migrate_plan()`)

For a cluster with many indexes, `sift.migrate_plan(target text DEFAULT '6.0')` turns the preflight findings into one ordered runbook of copy-pasteable SQL. It is a plain SQL function over `sift.upgrade_check()` and the catalogs, with no new settings:

```sql
SELECT step, phase, object, severity, reason, action_sql FROM sift.migrate_plan() ORDER BY step;
```

| Phase | What it holds |
|---|---|
| `pre_guc` | `ALTER SYSTEM`, `ALTER DATABASE`, and `ALTER ROLE ... RESET` for every removed setting, then `SELECT pg_reload_conf();` |
| `pre_option` | `ALTER INDEX <rel> RESET (<option>);` for every removed reloption |
| `pre_dep` | User views and functions that block `ALTER EXTENSION` |
| `upgrade_ext` | `ALTER EXTENSION pg_sift UPDATE TO '6.0';` (only when the extension is not at 6.0) |
| `post_reindex` | *Optional.* `REINDEX INDEX CONCURRENTLY <idx>;` for every Format 1 `sift_col` index, smallest first so progress shows early; BM25 indexes are left out |
| `post_table` | *Optional.* `SELECT sift.upgrade_table('<rel>');` for every Format 1 `sift_table`, smallest first |
| `post_verify` | `SELECT * FROM sift.check();` and `SELECT * FROM sift.health();` |

The `severity` of the optional phases is `action`; the upgrade itself needs only the `pre_*` and `upgrade_ext` steps.

```sh
# 1. Before the binary swap, on 5.1: export the pre-upgrade statements
psql -d mydb -At -c "SELECT action_sql FROM sift.migrate_plan() WHERE phase LIKE 'pre_%' AND action_sql IS NOT NULL ORDER BY step;"

# 2. After the swap and ALTER EXTENSION UPDATE, on 6.0: optionally run the online rebuilds
psql -d mydb -At -c "SELECT action_sql FROM sift.migrate_plan() WHERE phase = 'post_reindex' ORDER BY step;" | psql -d mydb -v ON_ERROR_STOP=1
```

## 3. Step-by-Step Upgrade (Standbys First)

Install the 6.0 binary on **all physical standbys first**, then the primary, so a standby never receives Format Major 2 WAL pages from a newer primary.

```sh
# 1. On the 5.1 primary: run preflight and resolve all 'blocker' rows
#    (sift.migrate_plan() lists the statements in order, see section 2a)
psql -d mydb -f sql/pg_sift_upgrade_check.sql

# 2. Remove obsolete GUCs and reset sift.backend_cache_kb if set to -1
psql -d mydb -c "ALTER SYSTEM RESET ALL;"   # or reset only the flagged sift.* settings
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

A logical dump of 5.x restored on 6.0 creates every `sift_table` and `sift_col` index in Format Major 2, so no `sift.upgrade_table()` or `REINDEX` is needed afterwards. The per-table options are dumped as data and may load after the rows; see [OPERATIONS.md](OPERATIONS.md#10-backup-pg_dump-and-pg_restore) for the restore order that applies them at once and for the `table_options_unapplied` check.

## 4. Upgrading PostgreSQL Majors (`pg_upgrade`)

`pg_upgrade` preserves `sift`, `sift_col`, and `sift_table` files as-is when **the exact same `pg_sift` major release** is installed in both the old and new PostgreSQL binaries:

1. If moving from `pg_sift` 5.1 on PG 17 to `pg_sift` 6.0 on PG 18/19, perform the `pg_sift` 5.1 → 6.0 upgrade **either before or after** `pg_upgrade` — never combine an extension major jump and a PostgreSQL major jump in one restart.
2. Install the matching `pg_sift` package for the target PostgreSQL major (`postgresql-18-pg-sift` or `postgresql-19-pg-sift`) before running `pg_upgrade --check`.
3. After `pg_upgrade` completes, run `VACUUM ANALYZE` and `SELECT * FROM sift.check();`.

## 5. Downgrade Policy

- **Format Major 2 (`sift_col`, `sift_table`):** A `pg_sift` 5.x binary cannot read Format Major 2 files. Only relations you upgraded to Format 2.0 (or created on 6.0) are affected: roll those back by dropping the `sift_col` indexes and converting the `sift_table` relations to `heap`, then downgrade the binary and extension and rebuild them under 5.1, or restore a pre-upgrade backup. Keep the upgrade to Format 2.0 for after you no longer need the 5.1 rollback.
- **Format 1.7 (`sift` BM25):** BM25 indexes written by 6.0 remain on Format 1.7 and are readable by 5.1 unless a newer minor feature is introduced later.
- **Patch releases (`6.0.x`):** Patch releases never change on-disk formats or SQL schemas and can be rolled back in place by swapping the `.so` binary.
