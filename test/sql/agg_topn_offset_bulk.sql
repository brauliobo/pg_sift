-- ORDER BY key, id LIMIT n OFFSET m (the key breaks ties by a second key) with an OFFSET of thousands: the global order takes the front of the OFFSET off its sources in one cut (a bucket
-- of a number, a string of a text key) and the walk queues the rest, whole groups of ties and the group that holds the first row to keep. The rows are the plan's without the extension,
-- and those of the walk of every entry (test_col_gorder_noskip), through ties, NULLs, ASC and DESC, NULLS FIRST and LAST, deleted, updated and vacuumed rows, new rows in the write
-- buffer, and a second pass of the same scan (a rewind).
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

CREATE TABLE ob (id int PRIMARY KEY, n int, t text COLLATE "C", pad text DEFAULT repeat('x', 100));
ALTER TABLE ob ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ob (id, n, t)
SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE (i * 7919) % 700 END, CASE WHEN i % 43 = 0 THEN NULL ELSE 't' || lpad(((i::bigint * 7919) % 1500)::text, 5, '0') END
FROM generate_series(1, 24000) i;
CREATE INDEX ob_i ON ob USING sift_col (id sift.col_int4_ops, n sift.col_int4_ops, t sift.col_text_ops) WITH (order_columns = 'id, n', order_array_columns = 'id, n, t');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ob;
SET sift.test_topn_heads = off;

-- the answer says DIFFERS with the query when the rows of the scan are not those of the plan without the extension or of the walk of every entry; 'bulk' when the scan used the global order
CREATE FUNCTION ob_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; ka text; kb text; kc text;
	rows text := 'SELECT string_agg(id || '':'' || coalesce(k::text, ''N''), '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) r) x';
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		used := used OR pl LIKE '%Global Order%';
	END LOOP;
	EXECUTE format(rows, q) INTO ka;
	SET LOCAL sift.test_col_gorder_noskip = on;
	EXECUTE format(rows, q) INTO kc;
	SET LOCAL sift.test_col_gorder_noskip = off;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE format(rows, q) INTO kb;
	RETURN CASE WHEN ka IS DISTINCT FROM kb OR kc IS DISTINCT FROM kb THEN 'DIFFERS ' || q WHEN NOT used THEN 'units' ELSE 'bulk' END;
END $$;

-- the same scan read twice by a join (the inner side is rewound, not materialized)
CREATE FUNCTION ob_twice(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off; SET LOCAL enable_material = off;
	SET LOCAL enable_hashjoin = off; SET LOCAL enable_mergejoin = off;
	EXECUTE format('SELECT string_agg(g || '':'' || id, '','' ORDER BY g, ord) FROM generate_series(1, 3) g, (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', q) INTO a;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE format('SELECT string_agg(g || '':'' || id, '','' ORDER BY g, ord) FROM generate_series(1, 3) g, (SELECT row_number() OVER () AS ord, * FROM (%s) r) x', q) INTO b;
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same' ELSE 'DIFFERS ' || q END;
END $$;

CREATE FUNCTION ob_all() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT ob_chk(format('SELECT id, %s AS k FROM ob ORDER BY %s %s NULLS %s, id %s LIMIT 20 OFFSET %s', c, c, d, nl, d2, o)) AS result, count(*)
   FROM unnest(ARRAY['n', 't']) c, unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['FIRST', 'LAST']) nl, unnest(ARRAY['ASC', 'DESC']) d2, unnest(ARRAY[2100, 9000, 17000]) o GROUP BY 1 ORDER BY 1 $$;

SELECT * FROM ob_all();
SELECT ob_twice('SELECT id, t AS k FROM ob ORDER BY t, id LIMIT 20 OFFSET 9000') AS t_rewind, ob_twice('SELECT id, n AS k FROM ob ORDER BY n, id LIMIT 20 OFFSET 9000') AS n_rewind;
-- deleted and updated rows, a VACUUM, new rows in the write buffer
DELETE FROM ob WHERE id % 53 = 0;
UPDATE ob SET n = n + 1, t = 'u' || t WHERE id % 97 = 0;
SELECT * FROM ob_all();
VACUUM ob;
SELECT * FROM ob_all();
INSERT INTO ob (id, n, t) SELECT 30000 + i, (i * 31) % 700, 't' || lpad(((i * 17) % 1500)::text, 5, '0') FROM generate_series(1, 300) i;
SELECT * FROM ob_all();
SELECT ob_twice('SELECT id, t AS k FROM ob ORDER BY t, id LIMIT 20 OFFSET 9000') AS t_rewind, ob_twice('SELECT id, n AS k FROM ob ORDER BY n, id LIMIT 20 OFFSET 9000') AS n_rewind;

DROP FUNCTION ob_all(), ob_twice(text), ob_chk(text);
DROP TABLE ob;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
