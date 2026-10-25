# Quickstart: pg_sift in five minutes

You need only Docker; everything below is removed at the end. The image is built from this repository on the official
`postgres` image ([CONTAINERS.md](CONTAINERS.md)) and takes a few minutes the first time.

## 1. Start PostgreSQL with pg_sift

```sh
packaging/docker/build.sh 18        # pg_sift:pg18 (17 works the same way)
docker run -d --name sift-try -e POSTGRES_PASSWORD=example pg_sift:pg18
docker exec -it sift-try psql -U postgres
```

`CREATE EXTENSION pg_sift` has already run for the `postgres` database and `template1`, so `\dx` lists it and every new database has it.

## 2. Load a small data set

100,000 orders built in SQL, with the two indexes pg_sift adds: `sift_col` for aggregates over the columns it lists, `sift` for BM25 search on the title.

```sql
CREATE TABLE orders AS
SELECT g AS id,
       1 + (g * 7919) % 5000 AS customer_id,
       (ARRAY['DE','FR','US','BR','JP'])[1 + g % 5] AS country,
       (ARRAY['cat_a','cat_b','cat_c','cat_d'])[1 + (g / 7) % 4] AS category,
       (ARRAY['paid','shipped','delivered','cancelled'])[1 + (g / 3) % 4] AS status,
       (5 + (g * 7919) % 29500)::numeric / 100 AS amount,
       timestamp '2025-01-01' + (g * 311 % 31536000) * interval '1 second' AS created_at,
       (ARRAY['wireless','portable','compact','premium','rugged','slim','smart','classic'])[1 + g % 8] || ' ' ||
       (ARRAY['charger','speaker','headphones','keyboard','lamp','monitor','camera'])[1 + (g / 8) % 7] ||
       CASE WHEN g % 3 = 0 THEN ' with case' ELSE '' END AS title
FROM generate_series(1, 100000) AS g;
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, created_at);
CREATE INDEX orders_title ON orders USING sift (title);
VACUUM ANALYZE orders;
```

## 3. BM25 search

```sql
SELECT id, title, round(sift.score(ctid)::numeric, 3) AS score FROM orders
WHERE title @@> 'wireless headphones' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
--  id  |        title        | score
-- -----+---------------------+-------
--   16 | wireless headphones | 4.484
--  128 | wireless headphones | 4.484
--  184 | wireless headphones | 4.484

SELECT count(*) FROM orders WHERE title @@> 'wireless headphones';      -- 1786, from the index
```

`@@>` matches all the words; `OR`, `NOT`, `"phrases"`, `batt*`, `/regex/` and `word~1` are in [USAGE.md](USAGE.md#queries). The score is exact BM25: a longer
title with the same words ("wireless headphones with case") scores lower.

## 4. An aggregate and its plan

```sql
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT category, count(*), sum(amount) FROM orders
WHERE country = 'DE' AND created_at >= '2025-06-01' GROUP BY category;
--  Custom Scan (SiftAgg) (actual rows=4.00 loops=1)
--    Index: orders_col
--    Group Keys: category
--    Pushed Aggregates: count(*), sum(orders.amount)
--    Units: 4
--    Units Pruned: 1
--    Rows Scanned: 77918
--    Tuples Aggregated: 11611
--    Buffers: shared hit=75
```

The aggregate ran inside the column index (`Custom Scan (SiftAgg)`); `Units Pruned: 1` is a unit skipped unread. With the pushdown off the same query
takes the plain PostgreSQL plan and returns the same answer:

```sql
SET sift.enable_agg = off;
-- the same EXPLAIN:   HashAggregate -> Seq Scan on orders   (Rows Removed by Filter: 88389, Buffers: shared hit=1216)
RESET sift.enable_agg;
```

(`EXPLAIN` lines are shortened; figures are from PostgreSQL 18.6 with default `shared_buffers`.) When a query keeps the normal plan, `SET sift.explain_refusals = on`
says why ([USAGE.md](USAGE.md#why-a-query-kept-the-normal-plan)).

## 5. The opt-in switches

The quickstart used only defaults. These settings change behaviour or disk format, so you turn them on yourself (`SHOW` in a session that has used the extension prints the default) ([USAGE.md](USAGE.md#maintenance-and-operations) has the full list):

| Setting | Default | What it does |
|---|---|---|
| `sift.enable_col_scan` | off | plan a `sift_col` index built with the `sift.col_*_ops` classes as a bitmap index scan for queries that return rows ([COL-SCAN.md](COL-SCAN.md)) |
| `sift.parallel_scan` | off | let the planner add parallel BM25 scans and counts |
| `sift.col_auto_merge` | on | a background worker vacuums the table after churn and, after the `VACUUM`, merges a `sift_col` index that churn has scattered (within 5% of the WAL the server writes; `off` stops it) |
| `sift.table_async_promote` | off | write-path variant of `sift_table` (no gain measured on a 4-client insert loop) |
| `sift.table_bulk_groups` | on | `COPY`, `CREATE TABLE AS` and `REFRESH MATERIALIZED VIEW` write column groups straight away |
| `sift.explain_refusals` | off | report why a query kept the normal plan |
| `sift.agg_lookup_shared_kb` | -1 (the smaller of the cache of a backend and 5% of `shared_buffers`) | shared memory that lets backends reuse the lookup tables a `SiftAgg` join loaded |
| `sift.col_format_minor` | 15 (the newest 3.3 writes) | a 3.2 library cannot read the `sift_table` tables and the indexes with new options that 3.3 writes at this setting: lower it (6 for 3.2, 4 for 3.1, 3 for 3.0) before writing what an older release must read ([SUPPORT.md](SUPPORT.md#format-minors-and-the-downgrade-rule)) |

`ALTER SYSTEM` or `ALTER DATABASE ... SET` make a setting stick; `sift.enable_agg = off` is the way back to plain PostgreSQL at any time.

## Check the numbers on your machine

```sh
bench/bench kit                      # docker only; builds the image if needed
bench/bench kit --rows 200000 --cpus 2 --runs 3
```

It starts a throwaway container, loads the orders data of the benchmark (100,000 orders by default), runs the ten questions with and without the pushdown, compares every
answer, and prints a table (exit status 3 when an answer differs). It took about 25 seconds once the image existed (100,000 orders, the only size run);
a laptop with fewer, slower cores differs in times and ratios.
Citus, Elasticsearch and ClickHouse (about an hour).

## Clean up

```sh
docker rm -f -v sift-try
```
