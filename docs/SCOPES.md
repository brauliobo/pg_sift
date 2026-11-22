# Multi-Tenant and Hierarchy Scopes (`scope`)

Multi-tenant analytics typically join a large fact table (`events`) through a many-to-many membership or hierarchy table (`tenant_members`) to filter by tenant, organization, or group. Standard PostgreSQL plans either scatter random heap lookups or hash-join millions of rows; denormalizing copies the fact table on every write.

`pg_sift` 6.0 solves this with an **Entity-Once Scope Engine**: fact rows are stored **once** in entity parts, while a compact, versioned 2-level summary tree and Top-K ring answer scoped rollups and latest-N queries in O(1) with **0 bytes of build temp-file I/O**.

## 1. Declaring a Scope

Add a `scope` clause to any `sift_col` index, or set the `scope` option of a `sift_table`. You can declare multiple scopes (separated by `;`) with **no on-disk cap** — Format Major 2 stores scope references in a dynamic `SIFT_PT_SCOPEDIR` chain.

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

The grammar, the summary tree, the Top-K rings, and the build are identical on both access methods; a `sift_table` is not a second implementation. A scope declared on a populated `sift_table` is built in the background by the maintenance workers.

### Syntax

```text
scope = 'name: fk -> [schema.]rel(member_col, key_col) [cluster[(strict)]] [topk(col, ...)] [null_filter(col)]; ...'
```

| Clause | Meaning |
|---|---|
| `name` | Identifier for the scope (used in `EXPLAIN` and `sift.scope_stats()`) |
| `fk` | Member-key column on the indexed fact table (integer, `text`, or `uuid`) |
| `[schema.]rel(member_col, key_col)` | Pair/membership relation mapping fact members to scope keys |
| `cluster` *(optional)* | Forces primary-parent clustering (see section 2); `cluster(strict)` raises `cardinality_violation` for a member with two parents |
| `topk(col, ...)` *(optional)* | Keeps extra O(1) Top-K rings per scope key for up to 3 more ordering columns (multi-column Top-K) |
| `null_filter(col)` *(optional)* | Leaves rows with a non-NULL `col` (for example `deleted_at`) out of the scope cubes, so `WHERE col IS NULL` answers in O(1) at any soft-delete ratio |

> **6.0 Breaking Change:** The v5.x `cols(...)` and `max_multiplicity(...)` sub-clauses are removed. Every column in the `sift_col` index is served automatically from entity parts with zero row duplication.

### 1-Hop and 2-Hop Hierarchies

Both direct membership joins (1-hop) and indirect group/folder hierarchies (2-hop) are recognized automatically when you write standard SQL joins:

```sql
-- 1-hop scoped rollup (answered in O(1) from Level-2 root cubes)
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

-- Scoped latest-20 (answered in O(1) from the 768 B Top-K ring)
SELECT e.member_id, e.event_date, e.kind, e.score
FROM events e
JOIN tenant_members tm ON tm.member_id = e.member_id
WHERE tm.tenant_id = 42
ORDER BY e.event_date DESC
LIMIT 20;
```

`IN (K1, K2, ...)` and `EXISTS (...)` queries enforce exact once-per-fact-row deduplication (`Scope Semantics: once`), while explicit joins with `GROUP BY scope_key` preserve per-pair multiplicity (`Scope Semantics: per pair`).

## 2. How the Entity-Once Engine Works

Instead of duplicating fact rows `G` times for `G`-way membership, `pg_sift` 6.0 stores every fact row once and maintains an immutable, Copy-on-Write `SIFT_SEG_SCOPE_META` object switched atomically in the manifest:

- **Level 0 (Pure vs. Impure Granule Refs):** Granules containing a single `member_id` are *pure* and reference the entity granule's own pre-aggregate cube directly (**0 duplicate cube bytes**). Multi-member *impure* granules store thin LZ4-compressed slice cubes in `SEC_SCOPE_L0_SLICE`.
- **Level 1 (Scope-Key Rollup Nodes):** Groups 16–64 member granules per scope key across all entity parts with all-time and per-month merged cubes plus member min/max bounds (or split-block Bloom filters for `text`/`uuid`).
- **Level 2 (Per-Key Root Summary):** One fixed-size record per scope key with pre-merged all-time and per-month cubes. Whole-key, single-month, and `GROUP BY scope_key` queries run in **O(1) per key** regardless of part count.
- **Primary-Parent Clustering (automatic):** When most members belong to one scope key (at least 75% have a single parent), the entity parts are clustered by each member's primary parent and an entity-to-parent route is built (≤ 1% disk overhead), so nearly every granule holds one scope key and answers from its own pre-aggregate with no duplicated rows. A member with several parents keeps its primary parent, so adding another parent never moves rows. `cluster` forces this for a scope below the threshold; `key_order = 'auto'` then sorts by the primary parent first ([ANALYTICS.md](ANALYTICS.md#2-physical-sort-and-partitioning-key_order)).
- **Top-K Ring (`768 B/key`):** Stores the newest 64 rows per scope key so unfiltered `ORDER BY time DESC LIMIT N` completes in O(1). Each column named in `topk(...)` gets its own ring (multi-column Top-K), and the rings that pay for themselves are chosen automatically.
- **MonthMap + 256-Row Time-Stride Index:** Descending month bitmaps and 0.0625 B/row per-granule time strides accelerate filtered `ORDER BY time DESC LIMIT N` and narrow date-range queries.
- **Zero-Temp Build & Fast CIC:** Both `CREATE INDEX` and `CREATE INDEX CONCURRENTLY` Pass 2 stream scope sections bottom-up with **0 bytes of temp-file I/O** and ≤ 5% disk overhead over the entity index (≤ 1% with `cluster`).

## 3. Live Mutations, Pair Edits, and MVCC

`pg_sift` attaches lightweight journal triggers to declared pair relations (`tenant_members`) so membership grants and revokes remain 100% consistent with PostgreSQL MVCC snapshots:

- **Fact-table deletes & updates:** Queries subtract unvacuumed deletes (from Copy-on-Write deletion vectors) directly from L2/L1 cubes on the fly — eliminating full-index invalidation cliffs.
- **Pair-table grants & revokes:** When members are added to or removed from a scope key, queries patch only the changed members (`O(|changed|)`) against the active snapshot.
- **Incremental background refresh:** When `sift.async_maintenance = on`, background workers refresh only dirty L1/L2 nodes rather than rebuilding the full scope metadata.

## 4. Inspecting and Troubleshooting Scopes

Use `sift.scope_stats()` to inspect cube levels, Top-K rings, dirty keys, and byte footprint per scope:

```sql
SELECT scope_name, nkeys, l0_slices, l1_nodes, l2_roots, topk_keys, dirty_keys,
       pg_size_pretty(bytes) AS size, clustered
FROM sift.scope_stats('events_col');
```

Check `EXPLAIN (ANALYZE, SIFT)` (PG 18+) or `sift.explain_why(query)` to verify scope usage:
- `Scope Index: events_col (tenant)` confirms the query used the scope summary tree.
- `Scope Fallback` explains if a query fell back to scanning entity parts (for example, when reading columns outside the index or when an unsupported predicate prevented scope pruning).

## 5. Comparing Multi-Tenant Approaches

| Approach | Fact Storage | Pair Grant / Revoke | Arbitrary Filters & Latest-N | Setup |
|---|---:|---|---|---|
| **`pg_sift` 6.0 `scope`** | **1×** (+≤ 5% meta) | Incremental (O(changed)) | Yes (O(1) Top-K + MonthMap) | One `CREATE INDEX` |
| Trigger-denormalized copy | G× (2–5×) | Heavy multi-row write | Yes (with extra B-trees) | Custom triggers + table |
| `pg_ivm` matview | Per-view | Synchronous on commit | Fixed query shapes only | One matview per query |
| Periodic `REFRESH MATERIALIZED VIEW` | Per-view | Stale until refresh | Fixed query shapes only | Cron + full refresh |
