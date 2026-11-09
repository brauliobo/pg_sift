-- A connection holds the key mapping and nothing of the parent's columns: a statement that reads a parent column through it reads the leaves of the parent's own sift_col index (no
-- heap fetch of the parent), whether the scan is of the child (the parent as the lookup) or the parent's columns decide the child's rows, and a column of the child is read from the leaves of
-- the child's index in both directions. The rows are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE FUNCTION reads(q text, OUT same boolean, OUT scans text, OUT leaf_reads int, OUT heap_fetches int, OUT proofs int)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	scans := coalesce((SELECT string_agg(DISTINCT v #>> '{}', ' ' ORDER BY v #>> '{}') FROM jsonb_path_query(p, '$.**."Index"') v), 'none');
	leaf_reads := coalesce(jsonb_path_query_first(p, '$.**."Parent Leaf Reads"')::text::int, 0);
	heap_fetches := coalesce(jsonb_path_query_first(p, '$.**."Parent Heap Fetches"')::text::int, 0);
	proofs := coalesce(jsonb_path_query_first(p, '$.**."Found Proofs"')::text::int, 0);
END $$;

CREATE TABLE dim (id int PRIMARY KEY, region int NOT NULL, label text NOT NULL, opened date NOT NULL, tier numeric(8, 2) NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, 'l' || i % 30, date '2026-01-01' + i % 60, i / 4.0 FROM generate_series(1, 20000) i;
CREATE INDEX dim_col ON dim USING sift_col (id, region, label, opened, tier);
CREATE TABLE fact (id int PRIMARY KEY, dim_id int NOT NULL REFERENCES dim, v int NOT NULL, ok boolean NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO fact SELECT i, 1 + (i::bigint * 7919) % 20000, i % 1000, i % 3 = 0 FROM generate_series(1, 200000) i;
CREATE INDEX fact_col ON fact USING sift_col (id, dim_id, v, ok);
VACUUM (FREEZE, ANALYZE) dim, fact;
-- the first child of a parent is built before the journal of the parent starts: its segments are filled by the next rewrite
SELECT sift.col_maintain('fact_col') AS rewritten;

CREATE TABLE shapes (n int, direction text, sql text);
INSERT INTO shapes VALUES
-- child to parent: the parent's columns of each kind are read from its leaves
	(1, 'child to parent', 'SELECT d.region, count(*), sum(f.v) FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY 1 ORDER BY 1'),
	(2, 'child to parent', 'SELECT d.label, count(*) FROM fact f JOIN dim d ON d.id = f.dim_id WHERE f.ok GROUP BY 1 ORDER BY 1'),
	(3, 'child to parent', 'SELECT d.opened, count(*) FROM fact f JOIN dim d ON d.id = f.dim_id WHERE f.v < 100 GROUP BY 1 ORDER BY 1'),
	(4, 'child to parent', 'SELECT count(*), sum(d.tier) FROM fact f JOIN dim d ON d.id = f.dim_id WHERE d.region = 3 AND f.ok'),
-- parent to child: the child scanned, the parent's columns as its lookup, the child's columns from its own leaves
	(5, 'parent to child', 'SELECT d.region, count(f.id), sum(f.v) FROM dim d LEFT JOIN fact f ON f.dim_id = d.id GROUP BY 1 ORDER BY 1'),
	(6, 'parent to child', 'SELECT d.label, count(f.id) FROM dim d LEFT JOIN fact f ON f.dim_id = d.id AND f.ok GROUP BY 1 ORDER BY 1'),
-- parent to child: the parents that have a child, counted from the child's keys and the flag of the connection
	(7, 'parent to child', 'SELECT count(*) FROM dim d WHERE EXISTS (SELECT 1 FROM fact f WHERE f.dim_id = d.id AND f.v < 3)'),
	(8, 'parent to child', 'SELECT count(*) FROM dim d WHERE NOT EXISTS (SELECT 1 FROM fact f WHERE f.dim_id = d.id AND f.v < 1)');

SELECT s.n, s.direction, r.same, r.scans, r.leaf_reads > 0 AS parent_leaves, r.heap_fetches AS parent_heap, r.proofs
	FROM shapes s, LATERAL reads(s.sql) r ORDER BY s.n;

DROP TABLE fact, dim, shapes CASCADE;
DROP FUNCTION reads(text);
DROP EXTENSION pg_sift;
