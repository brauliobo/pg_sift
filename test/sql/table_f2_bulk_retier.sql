-- A bulk COPY into a sift_table writes its parts at level 1 with the light codecs (no dictionaries or skip indexes at load); once the transaction commits the maintenance worker rewrites them as parts of the cold tier, rows unchanged.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET sift.async_maintenance = on;

-- the rows of a load as the awk program that prints them: id, k (a key of the shape), g, label (some NULL); a COPY of it is a bulk insert
CREATE FUNCTION gen_csv(n int, shape text, base int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT format($q$awk 'BEGIN { for (i = 1; i <= %s; i++) print (i + %s) "," %s "," (i %% 97) "," (i %% 13 == 0 ? "" : "item " (i %% 1009) " of group " (i %% 17) " and " i * 7) }'$q$, n, base,
		CASE shape WHEN 'ordered' THEN 'i' WHEN 'reversed' THEN format('(%s + 1 - i)', n) WHEN 'ties' THEN '(i % 5)'
				   WHEN 'nulls' THEN '(i % 3 == 0 ? "" : (i * 7919) % 10007)' ELSE '((i * 7919) % 100003)' END)
$$;
CREATE FUNCTION load(tbl regclass, n int, shape text, base int DEFAULT 0) RETURNS void LANGUAGE plpgsql AS $$
BEGIN EXECUTE format('COPY %s FROM PROGRAM %L (FORMAT csv)', tbl, gen_csv(n, shape, base)); END $$;
CREATE FUNCTION part_bytes(rel regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT sum(bytes)::bigint FROM sift.parts(rel) $$;
-- the parts a bulk insert wrote (level 1, a granule of rows or more); the rows the last flush of a load leaves are a small part of level 1 that the merges of level 1 take
CREATE FUNCTION bulk_parts(rel regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM sift.parts(rel) WHERE level = 1 AND rows >= 8192 $$;
CREATE FUNCTION rows_md5(rel regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN EXECUTE format('SELECT md5(string_agg(t::text, %L ORDER BY id)) FROM %s t', '|', rel) INTO r; RETURN r; END $$;
CREATE FUNCTION problems(rel regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM sift.check(rel) $$;
-- the worker has rewritten the parts of a bulk insert of the table (it was asked at the commit of the load)
CREATE FUNCTION retiered(rel regclass) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	stop timestamptz := clock_timestamp() + interval '60 seconds';
BEGIN
	WHILE clock_timestamp() < stop LOOP
		IF bulk_parts(rel) = 0 THEN RETURN true; END IF;
		PERFORM pg_sleep(0.05);
	END LOOP;
	RETURN false;
END $$;

-- the load, its retier and the rows of the heap, for each shape of the key (the key_order is k: the rows of the load come in that order or not); the transaction of the load holds the requests back until its end
CREATE TABLE shapes (shape text, warm_parts bigint, warm_bytes bigint, rows_equal_heap bool, problems_warm bigint, cold_bytes bigint, rows_equal_after bool, problems_after bigint);
DO $$
DECLARE s text;
BEGIN
	FOREACH s IN ARRAY ARRAY['ordered', 'reversed', 'ties', 'nulls', 'random'] LOOP
		EXECUTE format('CREATE TABLE %I (id int, k int, g int, label text)', 'h_' || s);
		EXECUTE format('CREATE TABLE %I (id int, k int, g int, label text) USING sift_table WITH (autovacuum_enabled = false)', 's_' || s);
		PERFORM sift.table_set_option(('s_' || s)::regclass, 'key_order', 'k');
		PERFORM load(('h_' || s)::regclass, 70000, s);
		PERFORM load(('s_' || s)::regclass, 70000, s);
		INSERT INTO shapes (shape, warm_parts, warm_bytes, rows_equal_heap, problems_warm)
			VALUES (s, bulk_parts(('s_' || s)::regclass), part_bytes(('s_' || s)::regclass), rows_md5(('s_' || s)::regclass) = rows_md5(('h_' || s)::regclass), problems(('s_' || s)::regclass));
	END LOOP;
END $$;
SELECT shape, retiered(('s_' || shape)::regclass) AS worker_rewrote FROM shapes ORDER BY shape;
UPDATE shapes SET cold_bytes = part_bytes(('s_' || shape)::regclass), rows_equal_after = rows_md5(('s_' || shape)::regclass) = rows_md5(('h_' || shape)::regclass),
	problems_after = problems(('s_' || shape)::regclass);
SELECT shape, warm_parts > 0 AS had_warm_parts, rows_equal_heap, problems_warm, cold_bytes < warm_bytes AS cold_smaller, rows_equal_after, problems_after FROM shapes ORDER BY shape;

-- the rewrite of a table with a primary key loaded in reverse (VACUUM FULL writes its parts as a bulk load does) keeps its rows and its key order; the next load of the table has the worker rewrite them with its own
CREATE TABLE kv (id int PRIMARY KEY, k int, g int, label text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE kv_heap (id int PRIMARY KEY, k int, g int, label text);
DO $$ BEGIN PERFORM load('kv', 70000, 'reversed'); PERFORM load('kv_heap', 70000, 'reversed'); END $$;
SELECT retiered('kv') AS worker_rewrote_the_load;
VACUUM FULL kv;
SELECT bulk_parts('kv') > 0 AS rewrite_wrote_bulk_parts, problems('kv') AS problems_after, rows_md5('kv') = rows_md5('kv_heap') AS rows_equal_heap;
DO $$ BEGIN PERFORM load('kv', 40000, 'ordered', 100000); PERFORM load('kv_heap', 40000, 'ordered', 100000); END $$;
SELECT retiered('kv') AS worker_rewrote_both;
SELECT problems('kv') AS problems_after, rows_md5('kv') = rows_md5('kv_heap') AS rows_equal_heap;

-- the compression option of the table reaches the writer of a bulk load: each policy writes its own sizes, at load and once retiered
CREATE TABLE zc (policy text, warm_bytes bigint, cold_bytes bigint);
DO $$
DECLARE p text;
BEGIN
	FOREACH p IN ARRAY ARRAY['none', 'lz4', 'zstd:1', 'auto'] LOOP
		EXECUTE format('CREATE TABLE %I (id int, k int, g int, label text) USING sift_table WITH (autovacuum_enabled = false)', 'zc_' || replace(p, ':', '_'));
		PERFORM sift.table_set_option(('zc_' || replace(p, ':', '_'))::regclass, 'compression', p);
		PERFORM load(('zc_' || replace(p, ':', '_'))::regclass, 70000, 'random');
		INSERT INTO zc VALUES (p, part_bytes(('zc_' || replace(p, ':', '_'))::regclass));
	END LOOP;
END $$;
SELECT policy, retiered(('zc_' || replace(policy, ':', '_'))::regclass) AS worker_rewrote FROM zc ORDER BY policy;
UPDATE zc SET cold_bytes = part_bytes(('zc_' || replace(policy, ':', '_'))::regclass);
SELECT count(DISTINCT warm_bytes) = 4 AS warm_sizes_differ, count(DISTINCT cold_bytes) = 4 AS cold_sizes_differ,
	(SELECT warm_bytes FROM zc WHERE policy = 'none') > (SELECT warm_bytes FROM zc WHERE policy = 'lz4') AS none_larger_than_lz4_warm,
	(SELECT cold_bytes FROM zc WHERE policy = 'none') > (SELECT cold_bytes FROM zc WHERE policy = 'zstd:1') AS none_larger_than_zstd_cold FROM zc;

DO $$ DECLARE r record; BEGIN FOR r IN SELECT relname FROM pg_class WHERE relnamespace = 'public'::regnamespace AND relkind = 'r' LOOP EXECUTE format('DROP TABLE %I', r.relname); END LOOP; END $$;
DROP EXTENSION pg_sift;
