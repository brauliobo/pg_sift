# Multi-Tenant and Hierarchy Scopes (`scope`)

Multi-tenant analytics typically join a large fact table (`events`) through a many-to-many membership or hierarchy table (`tenant_members`) to filter by tenant, organization, or group. Standard PostgreSQL plans either scatter random heap lookups or hash-join millions of rows; denormalizing copies the fact table on every write.

`pg_sift` 6.0 solves this with an **Entity-Once Scope Engine**: fact rows are stored **once** in entity parts, while a compact, versioned summary tree and Top-K rings answer scoped rollups and latest-N queries from the summary, without reading the fact rows.

## 1. Declaring a Scope

Add a `scope` clause to a `sift_col` index, or set the `scope` option of a `sift_table`. An index or table takes up to **4** scopes (separated by `;`), each with its own name.

```sql
CREATE INDEX events_col ON events
  USING sift_col (member_id, event_date, kind, score)
  WITH (
    key_order = 'member_id, event_date',
    scope     = 'tenant: member_id -> tenant_members(member_id, tenant_id)'
  );

-- the same declaration on a native columnar table
SELECT sift.table_set_option('events_tbl', 'scope', 'tenant: member_id -> tenant_members(member_id, tenant_id)');
```

The grammar, the summary tree, and the Top-K rings are the same on both access methods; a `sift_table` is not a second implementation. A scope declared on a populated `sift_table` is built in the background by the maintenance workers. The declaration is stored as written (in `pg_class.reloptions` for an index, in `sift.table_option_values` for a table), so `pg_dump`, `REINDEX`, and a restore resolve it again.

### Syntax

```text
scope = 'name: fk -> [schema.]pairs(member, scope_key) [using btree]
              [| [schema.]joined(via, scope_key) via [schema.]members(via, member)]; ...'
```

| Clause | Meaning |
|---|---|
| `name` | Identifier for the scope (used in `EXPLAIN` and `sift.scope_stats()`); unique within the index |
| `fk` | Member-key column of the indexed fact table: `int2`, `int4`, `int8`, `text`, `varchar`, or `uuid` (deterministic collation, not a virtual generated column) |
| `[schema.]pairs(member, scope_key)` | Relation that maps members to scope keys. Both columns are of the types above, stored (not virtual) and `NOT NULL`. The relation needs a btree on exactly `(member, scope_key)` |
| `using btree` *(optional)* | Names the btree that defines the rows of that relation, when the default choice is not the one you want |
| `\| joined(via, scope_key) via members(via, member)` *(optional)* | A second path: a key also covers the members of every group (`via`) that `joined` assigns to it. The `via` column is `int2`, `int4`, or `int8`; on two paths a `NULL` in a column of a path means that row is not a pair of that path |

> **6.0 Breaking Change:** The v5.x `cols(...)` and `max_multiplicity(...)` sub-clauses are deprecated. They still parse so existing declarations load, and Format Major 1 indexes read them as before; Format Major 2 ignores both because every column of the index is served from the entity parts with no row duplication.

### Which Format a Scope Gets

| Scope | `sift_col` index |
|---|---|
| One path or two paths, `int2` / `int4` / `int8` member and scope key | Format Major 2: summarized by the engine below |
| `text`, `varchar`, or `uuid` member or scope key | Stays Format Major 1: same grammar and answers, served by the Format Major 1 scope engine |
| An index that also declares or is the target of a `connect` | Stays Format Major 1 |

A Format Major 1 index never errors because of its format and upgrades only when its scope and `connect` fit Format Major 2; `sift.indexes.format` shows which one an index got. A Format Major 1 index refuses `INCLUDE` columns and `UNIQUE`; leave out the scope or `connect` that holds it back to get Format Major 2 and both.

### 1-Hop and 2-Hop Hierarchies

Both direct membership joins (1-hop) and indirect group/folder hierarchies (2-hop) are recognized automatically when you write standard SQL joins:

```sql
-- 1-hop scoped rollup (answered from the per-key summary)
SELECT date_trunc('month', e.event_date), count(*), avg(e.score)
FROM events e
JOIN tenant_members tm ON tm.member_id = e.member_id
WHERE tm.tenant_id = 42
GROUP BY 1;

-- Multi-tenant rollup across all keys
SELECT tm.tenant_id, count(*), sum(e.score)
FROM events e
JOIN tenant_members tm ON tm.member_id = e.member_id
WHERE e.event_date >= '2026-01-01'
GROUP BY tm.tenant_id;

-- Scoped latest-20 (answered from the Top-K ring)
SELECT e.member_id, e.event_date, e.kind, e.score
FROM events e
JOIN tenant_members tm ON tm.member_id = e.member_id
WHERE tm.tenant_id = 42
ORDER BY e.event_date DESC
LIMIT 20;
```

`IN (K1, K2, ...)` and `EXISTS (...)` queries enforce exact once-per-fact-row deduplication (`Scope Semantics: once`), while explicit joins with `GROUP BY scope_key` preserve per-pair multiplicity (`Scope Semantics: per pair`).

## 2. How the Entity-Once Engine Works

Instead of duplicating fact rows `G` times for `G`-way membership, `pg_sift` 6.0 stores every fact row once and maintains an immutable, Copy-on-Write scope summary switched atomically in the manifest (`SEC_SCOPE_*` sections, directory pages of type `SIFT_PT_SCOPEDIR`):

- **Level 0 (Pure vs. Impure Granule Refs):** Granules containing a single `member_id` are *pure* and reference the entity granule's own pre-aggregate cube directly (no duplicate cube bytes). Multi-member *impure* granules store thin LZ4-compressed slice cubes in `SEC_SCOPE_L0_SLICE`.
- **Level 1 (Scope-Key Rollup Nodes):** Groups 16 to 64 member granules per scope key across all entity parts, with all-time and per-month merged cubes plus member min/max bounds (or split-block Bloom filters for the member set).
- **Level 2 (Per-Key Root Summary):** One fixed-size record per scope key with pre-merged all-time and per-month cubes. Whole-key, single-month, and `GROUP BY scope_key` queries read one record per key regardless of the number of parts.
- **Primary-Parent Clustering (automatic):** When at least 75% of the members have a single parent and no pair repeats, the build samples the entity granules and, if they hold few distinct primary parents, clusters the entity parts by each member's primary parent and keeps an entity-to-parent route (`SEC_SCOPE_ENT_ROUTE`), so nearly every granule holds one scope key and answers from its own pre-aggregate. A member with several parents keeps its primary parent, so adding another parent never moves rows. There is no clause to force it; `key_order = 'auto'` sorts by the primary parent first when the route exists ([ANALYTICS.md](ANALYTICS.md#2-physical-sort-and-partitioning-key_order)).
- **Top-K Rings:** Each scope key keeps its newest 64 rows by the time leaf, delta-coded and LZ4-compressed when that saves at least 10%, so unfiltered `ORDER BY time DESC LIMIT N` reads the ring. Up to three more order columns get a ring of their own when most of their rows are not `NULL` and their granule zones mostly rise; the choice is automatic and there is nothing to declare.
- **MonthMap + 256-Row Time-Stride Index:** Descending month bitmaps and per-granule time strides of 256 rows accelerate filtered `ORDER BY time DESC LIMIT N` and narrow date-range queries.
- **Granule Router:** For a scan that needs some keys of the summary, a router builds the mask of granules that hold them; it is used when the keys take at most 60% of the granules, above which the scan reads nearly all of them anyway (`EXPLAIN` shows `Scope Granules Kept` and `Scope Granules Total`).

## 3. Live Mutations, Pair Edits, and MVCC

Queries stay consistent with PostgreSQL MVCC snapshots while facts and pairs change:

- **Fact-table deletes & updates:** Queries subtract unvacuumed deletes (from Copy-on-Write deletion vectors) directly from the L2/L1 cubes on the fly, which avoids full-index invalidation cliffs.
- **Pair-table grants & revokes:** Each pair relation has a *journal*: a ready Format Major 1 `sift_col` index on the relation (member first, holding the scope key, `track_changes = 'ctid'`) that hears every pair edit. A scope of two paths depends on three journals, one for each of its relations, each with its own stamp. When members are added to or removed from a scope key, queries patch only the changed members (`O(|changed|)`) against the active snapshot, and fall back to the join (`Scope Fallback`) when the journal cannot vouch for the pairs.
- **The journal index is made for you.** `CREATE INDEX` with a `scope` builds a journal on a pair relation that has none and says so in a `NOTICE`, which prints the `CREATE INDEX` statement to run first when you want to name it, place it, or build it without blocking writers. The journal goes with the last scope index that uses it. When the pair btree is partial, a journal you make yourself must hold every row that btree holds.
- **Pair-map cache across statements:** The map from members to their keys is read from the pair relation once and kept by the backend for the statements that follow, until the scope summary is replaced or the backend cache budget (`sift.backend_cache_kb`) asks for the memory.
- **Incremental background refresh:** When `sift.async_maintenance = on`, background workers refresh only dirty L1/L2 nodes rather than rebuilding the full scope metadata.
- **Logical replication:** A scope is index state (or, for a `sift_table`, an option of the table), and PostgreSQL's logical replication carries rows, not indexes. Declare the scope on the subscriber as well; it builds its own summary from the replicated facts and pairs. Publishing a `sift_table` itself needs `shared_preload_libraries = 'pg_sift'` on the publisher ([TABLES.md](TABLES.md#7-replication)).

## 4. Inspecting and Troubleshooting Scopes

Use `sift.scope_stats()` to inspect cube levels, Top-K rings, dirty keys, and byte footprint per scope:

```sql
SELECT scope_name, nkeys, l0_slices, l1_nodes, l2_roots, topk_keys, dirty_keys,
       pg_size_pretty(bytes) AS size, clustered
FROM sift.scope_stats('events_col');
```

Check `EXPLAIN (ANALYZE, SIFT)` (PG 18+) or `sift.explain_why(query)` to verify scope usage:
- `Scope Index: events_col (tenant)` confirms the query used the scope summary tree.
- `Scope Read` says whether the node read the `summary`, a `ring`, the `join`, or a scan of the facts, and `Scope Cube` whether the cubes were `clean` or corrected by a `delta`.
- `Scope Fallback` explains if a query fell back to scanning entity parts (for example, when reading columns outside the index or when an unsupported predicate prevented scope pruning).

## 5. Comparing Multi-Tenant Approaches

| Approach | Fact Storage | Pair Grant / Revoke | Arbitrary Filters & Latest-N | Setup |
|---|---:|---|---|---|
| **`pg_sift` 6.0 `scope`** | **1×** (plus the summary) | Incremental (O(changed)) | Yes (Top-K rings + MonthMap) | One `CREATE INDEX` |
| Trigger-denormalized copy | G× (2–5×) | Heavy multi-row write | Yes (with extra B-trees) | Custom triggers + table |
| `pg_ivm` matview | Per-view | Synchronous on commit | Fixed query shapes only | One matview per query |
| Periodic `REFRESH MATERIALIZED VIEW` | Per-view | Stale until refresh | Fixed query shapes only | Cron + full refresh |
