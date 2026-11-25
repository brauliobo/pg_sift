-- The top-N scans. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_topn_second_key
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- A top-N whose first key ties over thousands of rows (10 entities of 10,000 rows) reads the second key of every row of the tie: the order-array walk of one key is cheap, of two it is not.
-- With a deep OFFSET the (entity, ts) index-only walk (the second order, group after group) is the plan, as it is by time (0.8 against 6.5 ms for 300,000 rows over a random heap);
-- the one-key top-N has no tie to read and stays the scan.
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE sk (id int, ent_id int NOT NULL, ts timestamptz NOT NULL, amount float8);
INSERT INTO sk SELECT i, 1 + i % 10, timestamptz '2026-01-01' + (i * 7 % 100000) * interval '1 second', i % 97 FROM generate_series(1, 100000) i ORDER BY random();
CREATE INDEX sk_col ON sk USING sift_col (ent_id, ts, amount) WITH (key_order = 'ent_id, ts', order_layout = 'whole', track_changes = 'off');
VACUUM ANALYZE sk;

CREATE FUNCTION sk_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'other';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg%' THEN r := 'scan'; ELSIF l LIKE '%Index Only Scan%' THEN r := 'index only'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT n, sk_plan(format('SELECT ent_id, ts FROM sk ORDER BY ent_id, ts LIMIT 20 OFFSET %s', n)) AS two_keys, sk_plan(format('SELECT ent_id FROM sk ORDER BY ent_id LIMIT 20 OFFSET %s', n)) AS one_key
FROM unnest(ARRAY[0, 3000, 10000]) n ORDER BY n;

-- the rows are the heap's
SELECT count(*) AS rows_same FROM (SELECT ent_id, ts FROM sk ORDER BY ent_id, ts LIMIT 20 OFFSET 10000) a
JOIN (SELECT ent_id, ts FROM (SELECT ent_id, ts, row_number() OVER (ORDER BY ent_id, ts) rn FROM sk) w WHERE rn BETWEEN 10001 AND 10020) b USING (ent_id, ts);

DROP TABLE sk;
DROP FUNCTION sk_plan(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_second_key

-- >>> agg_topn_ties
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- The contract of a page with its totals when the sort key has ties (`ORDER BY created_at DESC LIMIT 10 OFFSET 20`, `count(*) OVER ()`, `sum(amount) OVER ()`):
-- the statement has no tie-breaker, so the rows of one value of the key may come in another order from the top-N scan than from PostgreSQL's plan, and neither
-- is a wrong answer. What must be the same is the key of each row of the page, the totals, and that each row is a row of the table with that key. Ordered by a
-- total order (the key and the id) the two plans give the same rows in the same order. Found by the 30,000,000-row stage of the 3.6.0 record (topn_page_totals).
SET client_min_messages = warning;

-- 24,000 rows over 3,000 values of the key: every value of it is shared by 8 rows
CREATE TABLE tt (id int PRIMARY KEY, country text, amount numeric(10,2), created_at timestamp, pad text DEFAULT repeat('x', 1500));
ALTER TABLE tt ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO tt (id, country, amount, created_at)
SELECT i, CASE WHEN i % 3 = 0 THEN 'US' ELSE 'BR' END, ((hashint4(i) & 2147483647) % 30000)::numeric / 100, timestamp '2026-01-01' + (i % 3000) * interval '1 minute'
FROM generate_series(1, 24000) i;
CREATE INDEX tt_col ON tt USING sift_col (id, country, amount, created_at) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) tt;

CREATE FUNCTION tt_mode(pushed bool) RETURNS void LANGUAGE sql AS $$ SELECT sift_helpers.agg_mode(pushed) $$;

-- the page by the scan (a) and by PostgreSQL's plan (b): 'same rows' when the rows equal in order (a total order must give that), 'same key and totals' when the key
-- and the totals of each position equal and every row of a is the row of the table of that key (what ties must give; the order of the ties may be the same by chance,
-- so the statements with ties print 'ok' for either), 'DIFFERENT' otherwise
CREATE FUNCTION tt_page(q text, ties bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE same_rows bool; same_keys bool; valid bool; n bigint;
BEGIN
	DROP TABLE IF EXISTS tt_a, tt_b;
	PERFORM tt_mode(true);
	EXECUTE 'CREATE TEMP TABLE tt_a AS SELECT row_number() OVER () AS pos, x.* FROM (' || q || ') x';
	PERFORM tt_mode(false);
	EXECUTE 'CREATE TEMP TABLE tt_b AS SELECT row_number() OVER () AS pos, x.* FROM (' || q || ') x';
	SELECT count(*) INTO n FROM tt_b;
	same_rows := NOT EXISTS (SELECT * FROM tt_a a FULL JOIN tt_b b USING (pos) WHERE a IS DISTINCT FROM b);
	same_keys := NOT EXISTS (SELECT * FROM tt_a a FULL JOIN tt_b b USING (pos) WHERE (a.created_at, a.total, a.revenue) IS DISTINCT FROM (b.created_at, b.total, b.revenue));
	valid := (SELECT count(*) FROM tt_a a JOIN tt ON tt.id = a.id AND tt.created_at = a.created_at) = n AND (SELECT count(DISTINCT id) FROM tt_a) = n;
	RETURN CASE WHEN same_rows AND NOT ties THEN 'same rows' WHEN ties AND (same_rows OR (same_keys AND valid)) THEN 'ok' ELSE 'DIFFERENT' END || ' ' || n;
END $$;

CREATE TABLE tt_q (tag text PRIMARY KEY, ties bool, q text NOT NULL);
INSERT INTO tt_q VALUES
	('desc_total_order', false, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at DESC, id DESC LIMIT 10 OFFSET 20$q$),
	('asc_total_order',  false, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at, id LIMIT 10 OFFSET 20$q$),
	('desc_ties',        true, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at DESC LIMIT 10 OFFSET 20$q$),
	('asc_ties',         true, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'US' ORDER BY created_at LIMIT 10 OFFSET 20$q$),
	('desc_ties_first',  true, $q$SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM tt WHERE country = 'BR' ORDER BY created_at DESC LIMIT 5$q$);
SELECT tag, tt_page(q, ties) FROM tt_q ORDER BY tag;

DROP TABLE tt, tt_q;
DROP FUNCTION tt_page(text, boolean), tt_mode(boolean);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_ties

-- >>> agg_topn_rescan
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- A top-N over the global order that an enclosing query starts over for each of its rows (a LATERAL join, a correlated sub-select) keeps its source open from one probe to the
-- next while the source has read nothing but its filters. The answers must be the heap's through every shape of probe (a pinned group and one the index lacks, a NULL, the same operand
-- again, a bound that pins nothing, a list, a text key, an offset, a probe that feeds the next), every state of the index (a write buffer, deleted and updated rows, a plain
-- order, a promotion), a cursor read both ways, a prepared statement, workers, and the memory of the source must not grow with the number of probes.
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE gg (id int, g int, k int, t timestamp, s text COLLATE "C", pad text DEFAULT repeat('x', 200));
ALTER TABLE gg ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gg (id, g, k, t, s)
SELECT i, CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 7) % 9 END, CASE WHEN i % 31 = 0 THEN NULL ELSE (i * 7919) % 500 END,
	   CASE WHEN i % 43 = 0 THEN NULL ELSE TIMESTAMP '2000-01-01' + ((i * 104729) % 90000) * interval '1 second' END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE 's' || lpad(((i * 104729) % 3000)::text, 4, '0') END
FROM generate_series(1, 12000) i;
-- a group that is only NULL keys, a group of one row
INSERT INTO gg (id, g, k, t, s) SELECT 20000 + i, 100, NULL, NULL, NULL FROM generate_series(1, 30) i;
INSERT INTO gg (id, g, k, t, s) VALUES (20100, 101, 5, TIMESTAMP '2001-01-01', 's0005');
CREATE TABLE dd AS SELECT i AS id, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 12 END AS g FROM generate_series(1, 6000) i;
CREATE INDEX gg_i ON gg USING sift_col (id sift.col_int4_ops, g sift.col_int4_ops, k sift.col_int4_ops, t sift.col_timestamp_ops, s sift.col_text_ops)
	WITH (order_array_columns = 'k, t, s', order_array_group = 'g');
SELECT sift.col_verify('gg_i');
ANALYZE gg;
ANALYZE dd;

-- the rows are the heap's, in order; 'sift' says the statement ran the custom scan
CREATE FUNCTION rs_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP used := used OR pl LIKE '%Custom Scan (SiftAgg%'; END LOOP;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN used THEN 'sift' ELSE 'native' END;
END $$;

-- the outer rows: a group twice in a row and again later, NULL, a group of NULL keys, of one row, none, the extremes
CREATE TABLE rs_o (i int, g int);
INSERT INTO rs_o VALUES (1, 3), (2, 3), (3, NULL), (4, 100), (5, 101), (6, 7), (7, 3), (8, 55), (9, 0), (10, 8), (11, 3), (12, 2), (13, 9), (14, NULL), (15, 100);
ANALYZE rs_o;
CREATE TABLE rs_q (n int, name text, q text);
INSERT INTO rs_q VALUES
 (1, 'top1_desc', 'SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 1) x ORDER BY v.i'),
 (2, 'top1_left', 'SELECT v.i, x.id, x.k FROM rs_o v LEFT JOIN LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 1) x ON true ORDER BY v.i'),
 (3, 'top3_asc_bound', 'SELECT v.i, x.id, x.t FROM rs_o v, LATERAL (SELECT id, t FROM gg WHERE g = v.g AND t > TIMESTAMP ''2000-01-01'' + v.i * interval ''2 hours'' ORDER BY t, id LIMIT 3) x ORDER BY v.i, x.t, x.id'),
 (4, 'id_bound', 'SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g AND id > v.i * 500 ORDER BY k, id LIMIT 2) x ORDER BY v.i, x.k, x.id'),
 (5, 'key_bound_no_pin', 'SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE k > v.i * 30 ORDER BY k, id LIMIT 2) x ORDER BY v.i, x.k, x.id'),
 (6, 'in_list', 'SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g IN (v.g, v.g + 1) ORDER BY k DESC, id LIMIT 3) x ORDER BY v.i, x.k DESC, x.id'),
 (7, 'subselect', 'SELECT v.i, (SELECT k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 1) FROM rs_o v ORDER BY v.i'),
 (8, 'subselect_max', 'SELECT v.i, (SELECT max(k) FROM gg WHERE g = v.g) FROM rs_o v ORDER BY v.i'),
 (9, 'offset', 'SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k, id LIMIT 2 OFFSET 1) x ORDER BY v.i, x.k, x.id'),
 (10, 'nulls_first', 'SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k NULLS FIRST, id LIMIT 2) x ORDER BY v.i, x.k NULLS FIRST, x.id'),
 (11, 'text_key', 'SELECT v.i, x.id, x.s FROM rs_o v, LATERAL (SELECT id, s FROM gg WHERE g = v.g ORDER BY s DESC, id LIMIT 2) x ORDER BY v.i, x.s DESC, x.id'),
 (12, 'text_bound', 'SELECT v.i, x.id, x.s FROM rs_o v, LATERAL (SELECT id, s FROM gg WHERE g = v.g AND s > ''s'' || lpad((v.i * 150)::text, 4, ''0'') ORDER BY s, id LIMIT 2) x ORDER BY v.i, x.s, x.id'),
 (13, 'other_filter', 'SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g AND t IS NOT NULL AND id % 2 = v.i % 2 ORDER BY k DESC, id LIMIT 2) x ORDER BY v.i, x.k DESC, x.id'),
 (14, 'feeds_next', 'SELECT v.i, x.id, y.id FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 2) x, LATERAL (SELECT id FROM gg WHERE g = x.k % 9 ORDER BY k, id LIMIT 1) y ORDER BY v.i, x.id'),
 (15, 'two_sources', 'SELECT v.i, x.id, y.id FROM rs_o v, LATERAL (SELECT id FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 1) x, LATERAL (SELECT id FROM gg WHERE g = v.g ORDER BY t, id LIMIT 1) y ORDER BY v.i'),
 (16, 'nested_loop_outer_table', 'SELECT d.id, x.id FROM dd d, LATERAL (SELECT id FROM gg WHERE g = d.g ORDER BY k DESC, id LIMIT 1) x WHERE d.id % 40 = 0 ORDER BY d.id'),
 (17, 'aggregate_over', 'SELECT v.i, x.n, x.m FROM rs_o v, LATERAL (SELECT count(*) AS n, max(k) AS m FROM (SELECT k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 5) z) x ORDER BY v.i'),
 (18, 'exists', 'SELECT v.i, EXISTS (SELECT 1 FROM gg WHERE g = v.g AND k > 400) FROM rs_o v ORDER BY v.i');
CREATE FUNCTION rs_all() RETURNS TABLE (n int, name text, result text) LANGUAGE sql AS $$ SELECT n, name, rs_chk(q) FROM rs_q ORDER BY n $$;
SELECT * FROM rs_all();

-- rows in the write buffer, then deleted and updated rows (the lists of dead rows and the dirty units are read at each probe), VACUUM, a promotion
INSERT INTO gg (id, g, k, t, s) SELECT 30000 + i, i % 11, (i * 13) % 300 - 20, TIMESTAMP '2003-01-01' + i * interval '1 minute', 's' || lpad((i % 3000)::text, 4, '0') FROM generate_series(1, 2500) i;
SELECT result, count(*) FROM rs_all() GROUP BY 1 ORDER BY 1;
DELETE FROM gg WHERE id % 11 = 0;
UPDATE gg SET g = coalesce(g, 0) + 1 WHERE id % 7 = 0;
UPDATE gg SET k = coalesce(k, 0) + 1000 WHERE id % 19 = 0;
SELECT result, count(*) FROM rs_all() GROUP BY 1 ORDER BY 1;
VACUUM gg;
SELECT result, count(*) FROM rs_all() GROUP BY 1 ORDER BY 1;
SELECT sift.col_promote('gg_i'::regclass) >= 0 AS promoted;
SELECT result, count(*) FROM rs_all() GROUP BY 1 ORDER BY 1;

-- the memory of the source does not grow with the probes: 20,000 probes over ten groups, with a write buffer, the context is the size of one
INSERT INTO gg (id, g, k, t, s) SELECT 60000 + i, i % 10, i % 700, NULL, 's' || lpad((i % 3000)::text, 4, '0') FROM generate_series(1, 1500) i;
-- the context and its descendants: by path where the view has it (PostgreSQL 18), else by parent name and level
CREATE FUNCTION rs_mem() RETURNS bigint LANGUAGE plpgsql AS
$$ DECLARE n bigint;
BEGIN
	IF EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = 'pg_backend_memory_contexts'::regclass AND attname = 'path') THEN
		EXECUTE $q$ SELECT coalesce(sum(c.total_bytes), 0)::bigint FROM pg_backend_memory_contexts c, pg_backend_memory_contexts r
			WHERE r.name = 'sift agg source' AND c.path @> ARRAY[r.path[cardinality(r.path)]] $q$ INTO n;
	ELSE
		EXECUTE $q$ WITH RECURSIVE t(name, level, total_bytes) AS (
			SELECT name, level, total_bytes FROM pg_backend_memory_contexts WHERE name = 'sift agg source'
			UNION ALL
			SELECT c.name, c.level, c.total_bytes FROM pg_backend_memory_contexts c JOIN t ON c.parent = t.name AND c.level = t.level + 1)
			SELECT coalesce(sum(total_bytes), 0)::bigint FROM t $q$ INTO n;
	END IF;
	RETURN n;
END $$;
SELECT max(CASE WHEN v.g = 20000 THEN rs_mem() END) BETWEEN 1 AND 1000000 AS source_memory_is_bounded
FROM generate_series(1, 20000) v(g), LATERAL (SELECT id FROM gg WHERE g = v.g % 10 ORDER BY k DESC, id LIMIT 1) x;
DROP FUNCTION rs_mem();

-- a cursor read forward and back starts the probes over; a prepared statement with a generic plan, the parameter changing between executions
DO $$
DECLARE c refcursor; r record; fwd text := ''; bwd text := ''; want text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
	OPEN c SCROLL FOR SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 1) x;
	LOOP FETCH NEXT FROM c INTO r; EXIT WHEN NOT FOUND; fwd := fwd || r::text; END LOOP;
	LOOP FETCH PRIOR FROM c INTO r; EXIT WHEN NOT FOUND; bwd := r::text || bwd; END LOOP;
	CLOSE c;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	SELECT string_agg(x::text, '' ORDER BY i) INTO want FROM (SELECT v.i, x.id, x.k FROM rs_o v, LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 1) x) x;
	RAISE WARNING 'cursor: % %', CASE WHEN fwd = want THEN 'forward is the heap''s' ELSE 'DIFFERS forward' END, CASE WHEN bwd = want THEN 'backward is the same' ELSE 'DIFFERS backward' END;
END $$;
CREATE FUNCTION rs_fn(p int, q int) RETURNS text LANGUAGE plpgsql SET plan_cache_mode = force_generic_plan SET sift.enable_agg = on SET sift.agg_force = on SET enable_seqscan = off SET enable_sort = off AS $$
DECLARE r text;
BEGIN
	SELECT string_agg(z::text, ',') INTO r FROM (SELECT v.i, x.id FROM rs_o v, LATERAL (SELECT id FROM gg WHERE g = v.g + p AND k > q ORDER BY k DESC, id LIMIT 1) x ORDER BY v.i) z;
	RETURN r;
END $$;
CREATE FUNCTION rs_fn_heap(p int, q int) RETURNS text LANGUAGE plpgsql SET plan_cache_mode = force_generic_plan SET sift.enable_agg = off AS $$
DECLARE r text;
BEGIN
	SELECT string_agg(z::text, ',') INTO r FROM (SELECT v.i, x.id FROM rs_o v, LATERAL (SELECT id FROM gg WHERE g = v.g + p AND k > q ORDER BY k DESC, id LIMIT 1) x ORDER BY v.i) z;
	RETURN r;
END $$;
SELECT p, q, rs_fn(p, q) IS NOT DISTINCT FROM rs_fn_heap(p, q) AS agree FROM (VALUES (0, -1), (1, 100), (0, -1), (5, 400), (-3, 0), (0, 450), (NULL, 1), (2, NULL), (0, -1)) t(p, q);
DROP FUNCTION rs_fn(int, int), rs_fn_heap(int, int);

-- writes in the middle of the statement: the outer rows call a function that deletes, updates, inserts 500 rows and promotes the write buffer between two probes; the statement keeps its
-- snapshot, so its rows are those of the same statement before the writes
CREATE FUNCTION rs_mut(i int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	IF i = 12 AND current_setting('rs.mut', true) = 'on' THEN
		DELETE FROM gg WHERE id % 13 = 0 AND id < 12000;
		UPDATE gg SET k = coalesce(k, 0) + 7 WHERE id % 17 = 0 AND id < 12000;
		INSERT INTO gg (id, g, k, t, s) SELECT 90000 + n, n % 10, n % 400, NULL, 's' || lpad((n % 3000)::text, 4, '0') FROM generate_series(1, 500) n;
		PERFORM sift.col_promote('gg_i'::regclass);
	END IF;
	RETURN i;
END $$;
DO $$
DECLARE before text; during text; q text := 'SELECT string_agg(z::text, '','' ORDER BY v.i, x.k DESC, x.id) FROM (SELECT i, i % 10 AS g, rs_mut(i) AS m FROM generate_series(1, 40) i OFFSET 0) v, LATERAL (SELECT id, k FROM gg WHERE g = v.g ORDER BY k DESC, id LIMIT 3) x, LATERAL (SELECT v.i, x.id, x.k) z';
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE q INTO before;
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	PERFORM set_config('rs.mut', 'on', true);
	EXECUTE q INTO during;
	RAISE WARNING 'writes in the statement: % (% rows)', CASE WHEN during = before THEN 'the rows of the snapshot' ELSE 'DIFFERS' END, length(during) - length(replace(during, '),(', '),')) + 1;
END $$;
SELECT result, count(*) FROM rs_all() GROUP BY 1 ORDER BY 1;
DROP FUNCTION rs_mut(int);

-- workers: the outer rows are read by several processes, each starting the probes over in its own
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT n, name, result FROM rs_all() WHERE n IN (16, 17) ORDER BY n;

DROP TABLE gg, dd, rs_o, rs_q;
DROP FUNCTION rs_chk(text), rs_all();
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_topn_rescan

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
