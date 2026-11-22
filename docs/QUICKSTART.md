# Quickstart: pg_sift in Five Minutes

Try `pg_sift` locally with Docker — nothing is compiled or installed on your host, and cleanup is one command. The published multi-architecture image (`linux/amd64`, `linux/arm64`) is the official `postgres` image with the extension (see [CONTAINERS.md](CONTAINERS.md)); packages for your own server are in [INSTALL-REPOS.md](INSTALL-REPOS.md).

## Which Access Method?

`pg_sift` has three access methods. A plain `CREATE INDEX` or `CREATE TABLE` with one of them needs no `WITH (...)` clause and no `SET`:

| You need | Use | Statement | Stores |
|---|---|---|---|
| Fast analytics on an existing OLTP table | `sift_col` *(index)* | `CREATE INDEX ... ON tbl USING sift_col (c1, c2, ...)` | A compressed columnar copy beside the heap |
| A native columnar table with no heap copy | `sift_table` *(table)* | `CREATE TABLE tbl (...) USING sift_table` | The columns only, compressed (see [TABLES.md](TABLES.md)) |
| Exact BM25 full-text search | `sift` *(index)* | `CREATE INDEX ... ON tbl USING sift (body)` | An inverted index (see [SEARCH.md](SEARCH.md)) |

A `sift` and a `sift_col` index on the same table work together automatically for text search plus analytics.

## 1. Start PostgreSQL with pg_sift

```sh
docker run -d --name sift-try -e POSTGRES_PASSWORD=example ghcr.io/brauliobo/pg_sift:pg18
docker exec -it sift-try psql -U postgres
```

The container runs `CREATE EXTENSION pg_sift` in `postgres` and `template1`, so `\dx` lists it and every new database inherits it. PostgreSQL 17 and 19 work the same way.

## 2. Load 100,000 Sample Orders

Create an ordinary heap table and add two `pg_sift` indexes: `sift_col` for columnar analytics and `sift` for exact BM25 full-text search.

```sql
CREATE TABLE orders AS
SELECT g AS id,
       1 + (g * 7919) % 5000 AS customer_id,
       (ARRAY['DE','FR','US','BR','JP'])[1 + g % 5] AS country,
       (ARRAY['cat_a','cat_b','cat_c','cat_d'])[1 + (g / 7) % 4] AS category,
       (ARRAY['paid','shipped','delivered','cancelled'])[1 + (g / 3) % 4] AS status,
       (5 + (g * 7919) % 29500)::numeric / 100 AS amount,
       timestamp '2026-01-01' + (g * 311 % 31536000) * interval '1 second' AS created_at,
       (ARRAY['wireless','portable','compact','premium','rugged','slim','smart','classic'])[1 + g % 8] || ' ' ||
       (ARRAY['charger','speaker','headphones','keyboard','lamp','monitor','camera'])[1 + (g / 8) % 7] ||
       CASE WHEN g % 3 = 0 THEN ' with case' ELSE '' END AS title
FROM generate_series(1, 100000) AS g;

ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, created_at);
CREATE INDEX orders_title ON orders USING sift (title);
VACUUM ANALYZE orders;
```

No `WITH (...)` is needed: `key_order = 'auto'` sorts the parts by the best grouping and time columns (here `created_at`), `preagg = 'auto'` keeps granule summaries, and `sift.parallel_scan` is `on`. Each automatic choice is reported once as a `NOTICE` and can be overridden or switched off (`key_order = 'off'`).

If an indexed column is `jsonb`, `paths = 'auto'` is inferred the same way: the keys present in at least 10% of a sample of 4,096 rows (up to 64 leaves) are shredded into typed columns, and the `NOTICE` names how many. An empty table is accepted and the paths are inferred at its first compaction. Use `WITH (paths = 'off')` to keep a `jsonb` column unshredded.

Check that nothing needs attention:

```sql
SELECT * FROM sift.health();   -- zero rows when every relation is healthy
SELECT relation, am, state, format, rows, size_bytes, util_pct FROM sift.indexes;
```

## 3. Run Columnar Analytics (`SiftAgg`)

Keep your standard SQL. PostgreSQL's planner selects `Custom Scan (SiftAgg)` on `orders_col` and prunes granules outside the date range.

```sql
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT category, count(*), count(DISTINCT customer_id), sum(amount)
FROM orders
WHERE country = 'DE' AND created_at >= '2026-06-01'
GROUP BY category;
```

If a query stays on the heap, ask why. `sift.explain_why()` names each candidate path, the reason, and the fix:

```sql
SELECT arm, used, slug, reason, action
FROM sift.explain_why($$SELECT category, count(*) FROM orders WHERE country = 'DE' GROUP BY category$$);
```

Disable pushdown at any time to verify exact equivalence against PostgreSQL's native planner:

```sql
SET sift.enable_agg = off;
SELECT category, count(*), count(DISTINCT customer_id), sum(amount)
FROM orders
WHERE country = 'DE' AND created_at >= '2026-06-01'
GROUP BY category;
RESET sift.enable_agg;
```

## 4. Run Exact BM25 Full-Text Search

```sql
SELECT id, title, round(sift.score(ctid)::numeric, 3) AS score
FROM orders
WHERE title @@> 'wireless headphones'
ORDER BY sift.score(ctid) DESC, id
LIMIT 3;

SELECT count(*) FROM orders WHERE title @@> 'wireless headphones';
```

`@@>` supports `AND`, `OR`, `NOT`, `"phrases"`, `prefix*`, `wild?cards`, `/regex/`, and `fuzzy~1`. See [SEARCH.md](SEARCH.md) for the full query language.

## 5. Three Essential Switches

`pg_sift` is designed to work out of the box with defaults. These three settings cover day-one testing and troubleshooting (see [OPTIONS.md](OPTIONS.md) for all 27 settings):

| Setting | Default | When to change it |
|---|---:|---|
| `sift.enable_agg` | `on` | Set `off` in a session to compare results or bypass `SiftAgg` immediately |
| `sift.explain_refusals` | `off` | Set `on` (or use `EXPLAIN (SIFT)` on PG 18+) to see why a query stayed on the heap |
| `sift.backend_cache_kb` | `16384` (16 MB) | Lower for high connection counts; raise for analytical sessions with wide schemas |

## 6. Benchmark on Your Machine and Clean Up

Run the self-contained Docker comparison kit (loads 100,000 rows, verifies answer equality, and prints a comparison table):

```sh
bench/bench kit --rows 200000 --cpus 2 --runs 3
```

Remove the quickstart container when finished:

```sh
docker rm -f -v sift-try
```

Next steps: [ANALYTICS.md](ANALYTICS.md), [SCOPES.md](SCOPES.md), [TABLES.md](TABLES.md), [SEARCH.md](SEARCH.md), and [OPERATIONS.md](OPERATIONS.md) for `sift.indexes`, `sift.status()`, `sift.health()`, and `sift.activity()`.
