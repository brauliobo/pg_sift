-- ORDER BY <column> LIMIT n with parallel workers: the workers take windows of superblocks from a shared counter, each keeps a heap of its best
-- rows, they share the first-key rank of their n-th row so that a worker leaves out the units that rank excludes, and the leader merges the
-- heaps. The rows and their order must be those of the plain plan whatever the workers (0, 2, 4), for a key that follows the table, one in
-- blocks of values (the walk), a random one and one with ties and NULLs, after deletes, moved rows, rows in the write buffer and the promotion;
-- and for a text search, a generic plan, a node that is scanned again and a cursor.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET client_min_messages = warning;

-- pad makes a page hold about 5 rows: 30000 rows are about 24 superblocks
CREATE TABLE pw (id serial PRIMARY KEY, g int, k_clu int, k_band int, k_rand int, k_mix int, f float8, memo text, pad text DEFAULT repeat('x', 1500));
ALTER TABLE pw ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO pw (g, k_clu, k_band, k_rand, k_mix, f, memo)
SELECT i % 5, i, (((i / 1500) * 7 + 3) % 20) * 1000 + (hashint4(i) & 2147483647) % 1000, (hashint4(i + 1) & 2147483647) % 100000,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE (hashint4(i + 2) & 2147483647) % 50 END,
	   CASE WHEN i % 997 = 0 THEN 'NaN'::float8 WHEN i % 37 = 0 THEN NULL ELSE ((hashint4(i + 3) & 2147483647) % 100000) / 7.0 END,
	   (ARRAY['alpha', 'beta', 'gamma', 'delta', 'omega', 'sigma'])[1 + (hashint4(i + 4) & 2147483647) % 6] || ' ' ||
	   (ARRAY['red', 'green', 'blue', 'amber'])[1 + (hashint4(i + 5) & 2147483647) % 4]
FROM generate_series(1, 30000) i;
CREATE INDEX pw_bm ON pw USING sift (memo);
CREATE INDEX pw_col ON pw USING sift_col (id, g, k_clu, k_band, k_rand, k_mix, f) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) pw;

-- the plan of the reference: no pushdown, no workers; the pushed one: the top-N scan with the workers asked for and free worker costs
CREATE FUNCTION pw_mode(pushed bool, workers int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, false);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, false);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, false);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, false);
	PERFORM set_config('max_parallel_workers_per_gather', CASE WHEN pushed THEN workers ELSE 0 END::text, false);
	PERFORM set_config('parallel_setup_cost', '0', false);
	PERFORM set_config('parallel_tuple_cost', '0', false);
	PERFORM set_config('min_parallel_table_scan_size', '0', false);
END $$;

CREATE FUNCTION pw_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO r;
	RETURN r;
END $$;

-- serial: the top-N scan alone; parallel: with workers planned; none: another plan
CREATE FUNCTION pw_kind(q text, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; par bool := false; topn bool := false;
BEGIN
	PERFORM pw_mode(true, workers);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		par := par OR line ~ 'Workers Planned: ';
		topn := topn OR line ~ 'Top-N: ';
	END LOOP;
	RETURN CASE WHEN topn AND par THEN 'parallel' WHEN topn THEN 'serial' ELSE 'none' END;
END $$;

-- one query: whether 0, 2 and 4 workers give the rows of the plain plan, and the kind with 4
CREATE FUNCTION pw_check(q text, OUT same bool, OUT kind text) LANGUAGE plpgsql AS $$
DECLARE base text; w int;
BEGIN
	PERFORM pw_mode(false, 0);
	base := pw_rows(q);
	same := true;
	FOREACH w IN ARRAY ARRAY[0, 2, 4] LOOP
		PERFORM pw_mode(true, w);
		same := same AND pw_rows(q) IS NOT DISTINCT FROM base;
	END LOOP;
	kind := pw_kind(q, 4);
END $$;

-- the query number i of a stage, from hashes of (seed, i)
CREATE FUNCTION pw_query(seed int, i int, tbl text) RETURNS text LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	h int[] := ARRAY(SELECT hashint4(seed * 100003 + i * 31 + x) & 2147483647 FROM generate_series(1, 10) x);
	key text := (ARRAY['k_clu', 'k_band', 'k_rand', 'k_mix', 'f'])[1 + h[1] % 5];
	ord text := key || CASE h[2] % 2 WHEN 0 THEN '' ELSE ' DESC' END || (ARRAY['', ' NULLS FIRST', ' NULLS LAST'])[1 + h[3] % 3];
	lim int := (ARRAY[1, 3, 10, 100, 700])[1 + h[4] % 5];
	off int := (ARRAY[0, 0, 0, 7, 50])[1 + h[5] % 5];
	flt text := CASE h[6] % 7
		WHEN 0 THEN ''
		WHEN 1 THEN 'WHERE g = ' || h[7] % 5
		WHEN 2 THEN 'WHERE id BETWEEN ' || h[7] % 20000 || ' AND ' || (h[7] % 20000 + 1 + h[8] % 9000)
		WHEN 3 THEN 'WHERE k_rand < ' || h[7] % 100000
		WHEN 4 THEN 'WHERE k_mix IS NULL OR k_clu % 7 = 0'
		WHEN 5 THEN 'WHERE memo @@> ''' || (ARRAY['alpha', 'beta', 'red', 'blue amber', 'omega OR sigma'])[1 + h[7] % 5] || ''''
		ELSE 'WHERE g IN (' || h[7] % 5 || ', ' || (h[7] + 2) % 5 || ') AND k_band >= ' || h[8] % 20000 END;
BEGIN
	RETURN CASE h[9] % 3
		WHEN 0 THEN format('SELECT id, %s FROM %s %s ORDER BY %s, id LIMIT %s OFFSET %s', key, tbl, flt, ord, lim, off)
		WHEN 1 THEN format('SELECT id, %s FROM %s %s ORDER BY %s, id DESC LIMIT %s OFFSET %s', key, tbl, flt, ord, lim, off)
		ELSE format('SELECT %s FROM %s %s ORDER BY %s LIMIT %s OFFSET %s', key, tbl, flt, ord, lim, off) END;
END $$;

-- N queries of a stage; the first differences are listed, so that a failure names its queries
CREATE FUNCTION pw_fuzz(label text, seed int, n int, tbl text DEFAULT 'pw') RETURNS TABLE (stage text, queries int, different int, mostly_parallel bool, first_different text)
LANGUAGE plpgsql AS $$
DECLARE r record; q text; bad int := 0; par int := 0; first text[] := '{}';
BEGIN
	FOR i IN 1..n LOOP
		q := pw_query(seed, i, tbl);
		SELECT * INTO r FROM pw_check(q);
		IF r.kind = 'parallel' THEN par := par + 1; END IF;
		IF NOT r.same THEN
			bad := bad + 1;
			IF bad <= 3 THEN first := first || q; END IF;
		END IF;
	END LOOP;
	RETURN QUERY SELECT label, n, bad, par * 2 >= n, array_to_string(first, E'\n');
END $$;

-- the plan: the shapes that a top-N plans workers for, with their kind (4 workers allowed)
SELECT q, pw_kind(q, 4) AS kind_w4, pw_kind(q, 0) AS kind_w0
FROM (VALUES ('SELECT id, k_clu FROM pw ORDER BY k_clu DESC, id LIMIT 10'),
			 ('SELECT id, k_band FROM pw ORDER BY k_band, id LIMIT 10'),
			 ('SELECT id, k_rand FROM pw WHERE g = 2 ORDER BY k_rand DESC, id LIMIT 100'),
			 ('SELECT id, k_mix FROM pw ORDER BY k_mix NULLS FIRST, id LIMIT 20'),
			 ('SELECT id, k_clu FROM pw WHERE memo @@> ''beta'' ORDER BY k_clu DESC, id LIMIT 10')) v (q);

-- shapes the workers must take apart: the bound of the n-th row and the NULLs of the key
SELECT q, (pw_check(q)).* FROM (VALUES
	('SELECT id, k_rand FROM pw ORDER BY k_rand, id LIMIT 1'),
	('SELECT id, k_rand FROM pw ORDER BY k_rand DESC, id LIMIT 700 OFFSET 50'),
	('SELECT id, k_band FROM pw ORDER BY k_band, id LIMIT 10'),
	('SELECT id, k_band FROM pw ORDER BY k_band DESC, id DESC LIMIT 10'),
	('SELECT id, k_mix FROM pw ORDER BY k_mix NULLS FIRST, id LIMIT 30'),
	('SELECT id, k_mix FROM pw ORDER BY k_mix DESC NULLS LAST, id LIMIT 30'),
	('SELECT id, k_mix, k_rand FROM pw ORDER BY k_mix, k_rand DESC, id LIMIT 25'),
	('SELECT k_mix FROM pw ORDER BY k_mix LIMIT 5000'),
	('SELECT f FROM pw ORDER BY f DESC NULLS LAST LIMIT 60'),
	('SELECT id FROM pw WHERE g = 7 ORDER BY id LIMIT 5'),
	('SELECT id, k_clu FROM pw WHERE memo @@> ''alpha'' AND g = 1 ORDER BY k_clu, id LIMIT 12'),
	('SELECT id, k_rand FROM pw WHERE memo @@> ''omega OR sigma'' ORDER BY k_rand DESC, id LIMIT 12')) v (q);

SELECT * FROM pw_fuzz('build', 21, 40);

-- a first key that ties between units (k_mix: 50 values and NULLs in every unit) is decided by the zone of the next key (id follows the table), for
-- the workers as for the serial walk: the units read with and without sift.topn_zone_keys (the count of the workers depends on who takes what: a bound)
CREATE FUNCTION pw_units(q text, workers int, zk bool) RETURNS int LANGUAGE plpgsql AS $$
DECLARE line text; n int;
BEGIN
	PERFORM pw_mode(true, workers);
	PERFORM set_config('sift.topn_zone_keys', zk::text, true);
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF line ~ '^ *Units: ' THEN n := regexp_replace(line, '^ *Units: ', '')::int; END IF;
	END LOOP;
	RETURN n;
END $$;
SELECT q, pw_kind(q, 2) AS kind_w2, pw_units(q, 2, true) <= 8 AS zone_keys_read_few, pw_units(q, 2, true) < pw_units(q, 2, false) AS fewer_than_without
FROM (VALUES ('SELECT id, k_mix FROM pw ORDER BY k_mix, id LIMIT 10'), ('SELECT id, k_mix FROM pw ORDER BY k_mix DESC, id DESC LIMIT 10'),
			 ('SELECT id, k_mix FROM pw WHERE g = 2 ORDER BY k_mix, id LIMIT 10')) v (q);
SELECT q, (pw_check(q)).* FROM (VALUES
	('SELECT id, k_mix FROM pw ORDER BY k_mix, id LIMIT 10'),
	('SELECT id, k_mix FROM pw ORDER BY k_mix DESC, id DESC LIMIT 10'),
	('SELECT id, k_mix FROM pw ORDER BY k_mix NULLS FIRST, id DESC LIMIT 10'),
	('SELECT id, k_mix, k_rand FROM pw ORDER BY k_mix, k_rand, id LIMIT 30')) v (q);

-- a generic plan: LIMIT and OFFSET are parameters; up to the cap the workers keep a heap each, NULL (all rows) and past the cap the scan sorts, serial
SET plan_cache_mode = force_generic_plan;
SELECT pw_mode(false, 0);
PREPARE pw_gen (bigint, bigint) AS SELECT id, k_rand FROM pw ORDER BY k_rand DESC, id LIMIT $1 OFFSET $2;
CREATE TEMP TABLE pw_ref_a AS EXECUTE pw_gen(5, 0);
CREATE TEMP TABLE pw_ref_b AS EXECUTE pw_gen(700, 50);
CREATE TEMP TABLE pw_ref_c AS EXECUTE pw_gen(NULL, 0);
CREATE TEMP TABLE pw_ref_d AS EXECUTE pw_gen(0, 0);
DEALLOCATE pw_gen;
SELECT pw_mode(true, 4);
PREPARE pw_gen (bigint, bigint) AS SELECT id, k_rand FROM pw ORDER BY k_rand DESC, id LIMIT $1 OFFSET $2;
CREATE FUNCTION pw_gen_kind() RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; par bool := false; topn bool := false;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE pw_gen(5, 0)' LOOP
		par := par OR line ~ 'Workers Planned: ';
		topn := topn OR line ~ 'Top-N: ';
	END LOOP;
	RETURN CASE WHEN topn AND par THEN 'parallel' WHEN topn THEN 'serial' ELSE 'none' END;
END $$;
SELECT pw_gen_kind() AS generic_kind;
CREATE TEMP TABLE pw_new_a AS EXECUTE pw_gen(5, 0);
CREATE TEMP TABLE pw_new_b AS EXECUTE pw_gen(700, 50);
CREATE TEMP TABLE pw_new_c AS EXECUTE pw_gen(NULL, 0);
CREATE TEMP TABLE pw_new_d AS EXECUTE pw_gen(0, 0);
SELECT (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM pw_ref_a) = (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM pw_new_a) AS lim5,
	   (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM pw_ref_b) = (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM pw_new_b) AS lim700_off50,
	   (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM pw_ref_c) = (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM pw_new_c) AS lim_null,
	   (SELECT count(*) FROM pw_new_c) AS lim_null_rows, (SELECT count(*) FROM pw_new_d) AS lim0_rows;
DEALLOCATE pw_gen;
RESET plan_cache_mode;

-- a node that is scanned again (the inner side of a nested loop) and a cursor
SELECT pw_mode(true, 4);
SET enable_material = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT g.x, count(*), sum(s.id) FROM generate_series(1, 3) g (x) CROSS JOIN (SELECT id FROM pw ORDER BY k_rand, id LIMIT 7) s GROUP BY g.x ORDER BY g.x;
RESET enable_material;
RESET enable_hashjoin;
RESET enable_mergejoin;
BEGIN;
DECLARE pw_cur CURSOR FOR SELECT id, k_band FROM pw ORDER BY k_band DESC, id LIMIT 12;
FETCH 3 FROM pw_cur;
FETCH 4 FROM pw_cur;
CLOSE pw_cur;
COMMIT;

-- ids 8000 to 8300 are deleted (not yet vacuumed: dirty units); the new versions of the updated rows are stored in other pages
DELETE FROM pw WHERE id BETWEEN 8000 AND 8300;
SELECT * FROM pw_fuzz('dirty', 22, 40);
VACUUM pw;
UPDATE pw SET k_band = -5, k_clu = -5, k_rand = -5, k_mix = -5, f = -5 WHERE id IN (5, 6000);
UPDATE pw SET k_band = 999999, k_clu = 999999, k_rand = 999999, k_mix = 999999, f = 'Infinity' WHERE id IN (7, 15000);
UPDATE pw SET k_band = NULL, k_clu = NULL, k_rand = NULL, k_mix = NULL, f = NULL WHERE id IN (9, 22000);
UPDATE pw SET k_band = k_band + 500 WHERE id BETWEEN 12000 AND 12040;
SELECT * FROM pw_fuzz('moved', 23, 40);

-- rows in the write buffer: new rows with values at both ends and in the middle, and updates in place
INSERT INTO pw (g, k_clu, k_band, k_rand, k_mix, f, memo)
SELECT i % 5, 100000 + i, (i * 37) % 20000, (i * 91) % 100000, i % 50, i / 3.0, 'alpha red' FROM generate_series(1, 150) i;
INSERT INTO pw (g, k_clu, k_band, k_rand, k_mix, f, memo) VALUES (1, -1000, -1000, -1000, -1000, -1000, 'beta'), (2, 2000000, 2000000, 2000000, 2000000, 2000000, 'beta'), (3, NULL, NULL, NULL, NULL, NULL, 'beta');
UPDATE pw SET k_rand = k_rand / 2, k_mix = 3 WHERE id % 211 = 0;
SELECT * FROM pw_fuzz('write buffer', 24, 40);

-- the buffer promoted into units, and the index after a vacuum
VACUUM pw;
SELECT sift.col_promote('pw_col') >= 0 AS promoted;
SELECT * FROM pw_fuzz('promoted', 25, 40);

-- a sift_table: the same queries over its column groups and write buffers
CREATE TABLE pwt (LIKE pw INCLUDING DEFAULTS) USING sift_table;
INSERT INTO pwt SELECT * FROM pw;
CREATE INDEX pwt_bm ON pwt USING sift (memo);
VACUUM (ANALYZE) pwt;
SELECT * FROM pw_fuzz('table', 26, 40, 'pwt');
UPDATE pwt SET k_rand = -3, k_band = 999999 WHERE id % 1500 = 0;
INSERT INTO pwt (g, k_clu, k_band, k_rand, k_mix, f, memo) SELECT i % 5, 200000 + i, i, i, i % 50, i, 'beta red' FROM generate_series(1, 120) i;
SELECT * FROM pw_fuzz('table, written', 27, 40, 'pwt');

DROP TABLE pwt, pw;
DROP FUNCTION pw_mode(bool, int), pw_rows(text), pw_kind(text, int), pw_check(text), pw_query(int, int, text), pw_fuzz(text, int, int, text), pw_gen_kind(), pw_units(text, int, bool);
DROP EXTENSION pg_sift;
