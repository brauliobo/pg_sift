-- ORDER BY an int4, int8 or timestamp column LIMIT n over units of 512 rows or more: the unit's rows go to the heap through the range and
-- min/max kernels (the segments of a unit by the best value they hold, those that cannot pass the bar left out) instead of one by one. Every key
-- shape of the kernels (negative values, the ends of the integer range, ties, NULLs and many NULLs, a column that follows the table and one
-- that does not, both directions, NULLS FIRST and LAST) under filters that leave most, some and few rows of a unit, compared with the plain plan.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

-- pad makes a page hold about 5 rows: 30000 rows are about 24 superblocks of about 1250 rows
CREATE TABLE tv (id serial PRIMARY KEY, g int, b_clu bigint, b_rand bigint, b_ext bigint, i_ext int, ts_rand timestamp, b_mix bigint, b_nul bigint, pad text DEFAULT repeat('x', 1500));
ALTER TABLE tv ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO tv (g, b_clu, b_rand, b_ext, i_ext, ts_rand, b_mix, b_nul)
SELECT i % 10, i::bigint * 1000003 - 15000000000, (hashint4(i) & 2147483647) % 2000000 - 1000000,
	   CASE WHEN i % 4999 = 0 THEN 9223372036854775807 WHEN i % 4999 = 1 THEN (-9223372036854775807 - 1) ELSE ((hashint4(i + 1) & 2147483647)::bigint << 20) - 1099511627776 END,
	   CASE WHEN i % 4999 = 0 THEN 2147483647 WHEN i % 4999 = 1 THEN -2147483648 ELSE (hashint4(i + 2) & 2147483647) - 1073741824 END,
	   timestamp '2020-01-01' + ((hashint4(i + 3) & 2147483647) % 100000000) * interval '1 second' + (i % 7) * interval '1 microsecond',
	   CASE WHEN i % 9 = 0 THEN NULL ELSE (hashint4(i + 4) & 2147483647) % 50 END,
	   CASE WHEN i % 10 = 0 THEN (hashint4(i + 5) & 2147483647) % 1000 END
FROM generate_series(1, 30000) i;
CREATE INDEX tv_col ON tv USING sift_col (id, g, b_clu, b_rand, b_ext, i_ext, ts_rand, b_mix, b_nul);
VACUUM (ANALYZE) tv;

CREATE FUNCTION tv_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

-- one query in both plans: the md5 of its rows in order and whether the pushed plan is the top-N scan
CREATE FUNCTION tv_run(q text, OUT pushed_md5 text, OUT plain_md5 text, OUT topn bool) LANGUAGE plpgsql AS $$
DECLARE line text; plan text := '';
BEGIN
	PERFORM tv_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	topn := plan ~ 'Top-N: ';
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO pushed_md5;
	PERFORM tv_mode(false);
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO plain_md5;
END $$;

CREATE FUNCTION tv_stage(OUT queries int, OUT differ int, OUT not_topn int) LANGUAGE plpgsql AS $$
DECLARE q text; r record;
BEGIN
	queries := 0;
	differ := 0;
	not_topn := 0;
	FOR q IN
		SELECT format('SELECT id, %1$s FROM tv %2$s ORDER BY %1$s %3$s %4$s, id LIMIT %5$s', k, f, d, nl, l)
		FROM unnest(ARRAY['b_clu', 'b_rand', 'b_ext', 'i_ext', 'ts_rand', 'b_mix', 'b_nul']) k,
			 unnest(ARRAY['', 'WHERE g < 8', 'WHERE g = 3', 'WHERE id % 97 = 0']) f,
			 unnest(ARRAY['', 'DESC']) d, unnest(ARRAY['', 'NULLS FIRST', 'NULLS LAST']) nl, unnest(ARRAY[3, 100, 700]) l
	LOOP
		SELECT * INTO r FROM tv_run(q);
		queries := queries + 1;
		differ := differ + (r.pushed_md5 IS DISTINCT FROM r.plain_md5)::int;
		not_topn := not_topn + (NOT r.topn)::int;
	END LOOP;
END $$;

SELECT * FROM tv_stage();
-- after deletes, updates that move rows and rows in the write buffer
DELETE FROM tv WHERE id % 11 = 0;
UPDATE tv SET b_rand = -b_rand, b_clu = b_clu + 7, ts_rand = ts_rand + interval '1 day' WHERE id % 13 = 0;
INSERT INTO tv (g, b_clu, b_rand, b_ext, i_ext, ts_rand, b_mix, b_nul)
SELECT i % 10, i * 7, i * 3 - 4000, i::bigint * 100000000000, i, timestamp '2021-01-01' + i * interval '1 minute', i % 5, NULL FROM generate_series(1, 200) i;
SELECT * FROM tv_stage();

DROP FUNCTION tv_stage();
DROP FUNCTION tv_run(text);
DROP FUNCTION tv_mode(bool);
DROP TABLE tv;
DROP EXTENSION pg_sift;
