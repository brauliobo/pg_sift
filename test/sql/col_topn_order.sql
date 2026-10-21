-- ORDER BY <column> LIMIT n over a sift_col index for keys that lie in the table in different ways: one that follows it, one in blocks of
-- values (the blocks in no order), a random one, and one with many ties and NULLs. A bounded differential fuzz (fixed seed, queries made from
-- hashes) compares the pushed top-N with the plain plan (sift.enable_agg off) in order: both directions and NULLS placements, ties with and
-- without a tie-break, offsets, filters, and after deletes, moved rows, rows in the write buffer and the promotion of the buffer.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

-- pad makes a page hold about 5 rows: 30000 rows are about 24 superblocks
CREATE TABLE ow (id serial PRIMARY KEY, g int, k_clu int, k_band int, k_rand int, k_mix int, f float8, pad text DEFAULT repeat('x', 1500));
ALTER TABLE ow ALTER COLUMN pad SET STORAGE PLAIN;
-- k_band: blocks of 1500 consecutive rows, each block holds the values of one band of 1000 (20 bands), the bands in a scrambled order
INSERT INTO ow (g, k_clu, k_band, k_rand, k_mix, f)
SELECT i % 5, i, (((i / 1500) * 7 + 3) % 20) * 1000 + (hashint4(i) & 2147483647) % 1000, (hashint4(i + 1) & 2147483647) % 100000,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE (hashint4(i + 2) & 2147483647) % 50 END,
	   CASE WHEN i % 997 = 0 THEN 'NaN'::float8 WHEN i % 37 = 0 THEN NULL ELSE ((hashint4(i + 3) & 2147483647) % 100000) / 7.0 END
FROM generate_series(1, 30000) i;
CREATE INDEX ow_col ON ow USING sift_col (id, g, k_clu, k_band, k_rand, k_mix, f) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) ow;

CREATE FUNCTION ow_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

-- one query in both plans: the md5 of its rows in order, and whether the pushed plan is the top-N scan
CREATE FUNCTION ow_run(q text, OUT pushed_md5 text, OUT plain_md5 text, OUT topn bool) LANGUAGE plpgsql AS $$
DECLARE line text; plan text := '';
BEGIN
	PERFORM ow_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	topn := plan ~ 'Top-N: ';
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO pushed_md5;
	PERFORM ow_mode(false);
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO plain_md5;
END $$;

-- the query number i of a stage, from hashes of (seed, i)
CREATE FUNCTION ow_query(seed int, i int) RETURNS text LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	h int[] := ARRAY(SELECT hashint4(seed * 100003 + i * 31 + x) & 2147483647 FROM generate_series(1, 10) x);
	key text := (ARRAY['k_clu', 'k_band', 'k_rand', 'k_mix', 'f'])[1 + h[1] % 5];
	ord text := key || CASE h[2] % 2 WHEN 0 THEN '' ELSE ' DESC' END || (ARRAY['', ' NULLS FIRST', ' NULLS LAST'])[1 + h[3] % 3];
	lim int := (ARRAY[1, 3, 10, 100, 700])[1 + h[4] % 5];
	off int := (ARRAY[0, 0, 0, 7, 50])[1 + h[5] % 5];
	flt text := CASE h[6] % 6
		WHEN 0 THEN ''
		WHEN 1 THEN 'WHERE g = ' || h[7] % 5
		WHEN 2 THEN 'WHERE id BETWEEN ' || h[7] % 20000 || ' AND ' || (h[7] % 20000 + 1 + h[8] % 9000)
		WHEN 3 THEN 'WHERE k_rand < ' || h[7] % 100000
		WHEN 4 THEN 'WHERE k_mix IS NULL OR k_clu % 7 = 0'
		ELSE 'WHERE g IN (' || h[7] % 5 || ', ' || (h[7] + 2) % 5 || ') AND k_band >= ' || h[8] % 20000 END;
BEGIN
	RETURN CASE h[9] % 3
		WHEN 0 THEN format('SELECT id, %s FROM ow %s ORDER BY %s, id LIMIT %s OFFSET %s', key, flt, ord, lim, off)
		WHEN 1 THEN format('SELECT id, %s FROM ow %s ORDER BY %s, id DESC LIMIT %s OFFSET %s', key, flt, ord, lim, off)
		ELSE format('SELECT %s FROM ow %s ORDER BY %s LIMIT %s OFFSET %s', key, flt, ord, lim, off) END;
END $$;

-- N queries of a stage; the first differences are listed, so that a failure names its queries
CREATE FUNCTION ow_fuzz(label text, seed int, n int) RETURNS TABLE (stage text, queries int, different int, mostly_topn bool, first_different text)
LANGUAGE plpgsql AS $$
DECLARE r record; q text; bad int := 0; pushed int := 0; first text[] := '{}';
BEGIN
	FOR i IN 1..n LOOP
		q := ow_query(seed, i);
		SELECT * INTO r FROM ow_run(q);
		IF r.topn THEN pushed := pushed + 1; END IF;
		IF r.pushed_md5 IS DISTINCT FROM r.plain_md5 THEN
			bad := bad + 1;
			IF bad <= 3 THEN first := first || q; END IF;
		END IF;
	END LOOP;
	RETURN QUERY SELECT label, n, bad, pushed * 2 >= n, array_to_string(first, E'\n');
END $$;

SELECT * FROM ow_fuzz('build', 11, 120);

-- ids 8000 to 8300 are deleted; the new versions of the updated rows are stored in other pages (the end of the table), far from the neighbours of their values
DELETE FROM ow WHERE id BETWEEN 8000 AND 8300;
VACUUM ow;
UPDATE ow SET k_band = -5, k_clu = -5, k_rand = -5, k_mix = -5, f = -5 WHERE id IN (5, 6000);
UPDATE ow SET k_band = 999999, k_clu = 999999, k_rand = 999999, k_mix = 999999, f = 'Infinity' WHERE id IN (7, 15000);
UPDATE ow SET k_band = NULL, k_clu = NULL, k_rand = NULL, k_mix = NULL, f = NULL WHERE id IN (9, 22000);
UPDATE ow SET k_band = k_band + 500 WHERE id BETWEEN 12000 AND 12040;
SELECT id, (ctid::text::point)[0] >= 6000 AS moved_to_the_end FROM ow WHERE id IN (5, 7) ORDER BY id;
SELECT * FROM ow_fuzz('moved', 12, 120);

-- rows in the write buffer: new rows with values at both ends and in the middle, and updates in place
INSERT INTO ow (g, k_clu, k_band, k_rand, k_mix, f)
SELECT i % 5, 100000 + i, (i * 37) % 20000, (i * 91) % 100000, i % 50, i / 3.0 FROM generate_series(1, 150) i;
INSERT INTO ow (g, k_clu, k_band, k_rand, k_mix, f) VALUES (1, -1000, -1000, -1000, -1000, -1000), (2, 2000000, 2000000, 2000000, 2000000, 2000000), (3, NULL, NULL, NULL, NULL, NULL);
UPDATE ow SET k_rand = k_rand / 2, k_mix = 3 WHERE id % 211 = 0;
SELECT * FROM ow_fuzz('write buffer', 13, 120);

-- the buffer promoted into units, and the index after a vacuum
VACUUM ow;
SELECT sift.col_promote('ow_col') >= 0 AS promoted;
SELECT * FROM ow_fuzz('promoted', 14, 120);

DROP TABLE ow;
DROP FUNCTION ow_mode(bool), ow_run(text), ow_query(int, int), ow_fuzz(text, int, int);
DROP EXTENSION pg_sift;
